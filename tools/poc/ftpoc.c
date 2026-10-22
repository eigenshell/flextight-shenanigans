/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftpoc.c — FlexTight II (Precision II) proof-of-concept console tool.
 *
 * Talks to the scanner directly (ftio.h): on Windows through the Microsoft
 * scsiscan.sys driver, replicating the exact 44-byte SRB + ioctl 0x190012
 * transport that FlexColor.exe uses (validated against 67k+ hooked
 * DeviceIoControl calls, sessions 2-6; ftio_win.c), on Linux through SG_IO
 * on the scanner's /dev/sgN (ftio_posix.c).
 *
 * Standalone process: FlexColor must be CLOSED (the driver opens exclusively;
 * CreateFile fails with ERROR_ACCESS_DENIED otherwise — that's the check).
 *
 * Build: Windows 32-bit (i686-w64-mingw32-gcc or cl), see build.bat;
 * Linux: make (Makefile).
 *
 * Device: the scanner answers on all 8 LUNs of its target, so Windows makes
 * several \\.\ScannerN; only one is live (session-4 box: Scanner4). The
 * others fail every command with UNEXPECTED_BUS_FREE. open_scanner() picks
 * the one whose INQUIRY says Imacon.
 *
 * SAFETY: every descriptor sent is one FlexColor sent, byte for byte. The
 * only fields ever patched are the resolution (BE u32 @10, goto/move on the
 * idle template) and the start line (BE u32 @18, scan -y; FlexColor's own
 * 3f frames differ only there), both clamped to ranges FlexColor used, plus
 * the window and lens/CCD fields of synthesized crops (crop) and the -p/-l
 * overrides, each within the values FlexColor sent. Park is FlexColor's
 * shutdown descriptor.
 */
#include "ft_calib.h"
#include "ft_detail.h"
#include "ft_detect.h"
#include "ft_dplan.h"
#include "ft_endian.h"
#include "ft_focus.h"
#include "ft_geom.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_maint.h"
#include "ft_plan.h"
#include "ft_png.h"
#include "ft_preview.h"
#include "ft_progress.h"
#include "ft_share.h"
#include "ft_share_cut.h"
#include "ft_upload.h"
#include "ftio.h"
#include "ring.h"
#include "worker.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMEOUT_MS 5000

/* A detailed-scan pass in progress (sub-project 2b): cmd_scan collects
 * the calibration lines in RAM, then starts the worker (ft_detail behind
 * a ring) and floods the image lines into the ring instead of a file.
 * NULL for every other scan. */
static struct detail_run {
  ft_detail_opts o;
  ft_detail *d;
  ring *r;
  pass_worker w;
  int ring_mb;
  unsigned short *cal;   /* FT_CAL_LINES lines */
  size_t cal_n, cal_cap; /* bytes */
  int phase;             /* 0: calibration lines, 1: image */
} *g_detail;

static int detail_put(const BYTE *p, DWORD n);
static int detail_start(const char *out);

/* ---------------------------------------------------------------- transport */

/* Current resolution field (ppi x 10000), as far as this process has
 * commanded it. Seeded by park(), updated by move/step. Display only. */
static long g_pos = -1;
static int g_pos_known = 0;

/* Like FlexColor's FindDevices: open every \\.\ScannerN (Linux: every
 * Imacon /dev/sgN, or --device) and keep the one
 * whose INQUIRY names Imacon. Other indexes can be stale or foreign
 * devices that never answer. A fresh handle can fail its first few
 * INQUIRYs with ERROR_IO_DEVICE (FlexColor sees 2-4), so retry briefly. */
/* The kept device's INQUIRY answer (g_inq_n bytes; 0: none yet). */
static BYTE g_inq[96];
static int g_inq_n;

enum { PROBE_PREFER_MICROCODE, PROBE_MICROCODE_ONLY };

/* The \\.\ScannerN that answers INQUIRY as Imacon (g_dev, g_inq set), or
 * IO_NONE. The loader and the microcode can answer on
 * different N (seen: Scanner0 loader, Scanner4 microcode), so the
 * microcode wins: a loader is kept only while no later N answers with
 * the microcode (PROBE_PREFER_MICROCODE), or never (PROBE_MICROCODE_ONLY,
 * after an upload). */
static io_dev probe_scanner(int want) {
  static const BYTE inq[6] = {0x12, 0, 0, 0, 0x60, 0};
  io_dev loader = IO_NONE;
  BYTE loader_inq[96];
  int i, attempt, loader_n = 0;
  char path[IO_PATH_MAX + 1];

  for (i = 0; io_dev_path(i, path, sizeof path); i++) {
    io_dev h;
    int keep = 0;
    h = io_open(path);
    if (h == IO_NONE)
      continue;
    g_dev = h;
    for (attempt = 0; attempt < 8; attempt++) {
      BYTE buf[96];
      int r;
      memset(buf, 0, sizeof buf);
      r = srb_exec(inq, 6, 0x40, buf, sizeof buf, 3000);
      if (r >= 0) {
        printf("%s: %.8s %.16s %.4s\n", path, buf + 8, buf + 16, buf + 32);
        if (memcmp(buf + 8, "Imacon", 6) != 0)
          break;
        if (!ft_upload_is_loader(buf, (size_t)r)) {
          if (loader != IO_NONE)
            io_close(loader);
          memcpy(g_inq, buf, sizeof g_inq);
          g_inq_n = r;
          return h;
        }
        if (want == PROBE_PREFER_MICROCODE && loader == IO_NONE) {
          loader = h; /* unless the microcode answers later */
          memcpy(loader_inq, buf, sizeof loader_inq);
          loader_n = r;
          keep = 1;
        }
        break;
      }
    }
    if (attempt == 8)
      printf("%s: no INQUIRY answer, skipping\n", path);
    if (!keep)
      io_close(h);
    g_dev = IO_NONE;
  }
  if (loader != IO_NONE) {
    g_dev = loader;
    memcpy(g_inq, loader_inq, sizeof g_inq);
    g_inq_n = loader_n;
  }
  return loader;
}

static io_dev open_scanner(void) {
  io_dev h = probe_scanner(PROBE_PREFER_MICROCODE);
  if (h != IO_NONE)
    return h;
  fprintf(stderr, "ftpoc: no " IO_DEV_NAME
                  " answered as Imacon. Is the scanner powered,\n"
                  "and FlexColor (or anything else holding it) closed?\n");
  exit(1);
}

/* ----------------------------------------------------------------- commands */

static const BYTE CDB_INQUIRY[6] = {0x12, 0, 0, 0, 0x60, 0};
static const BYTE CDB_POLL[6] = {0xdf, 0, 0, 0, 0x02, 0};
static const BYTE CDB_WIN[10] = {0x24, 0, 0, 0, 0, 0, 0, 0x54, 0, 0};
static const BYTE CDB_GRAB[10] = {0x28, 0, 0, 0, 0x28, 0, 0, 0, 0, 0};
static const BYTE CDB_PAGE[10] = {0x3c, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const BYTE CDB_SCANSTOP[6] = {0xdb, 0, 0, 0, 0, 0}; /* kHwiScanStop */

/* SetWindow 84-byte descriptors, generated byte-for-byte from the session-4
 * sidecars (flextrace_bufs.bin, cand 0x60, when=0) — NOT typed by hand: an
 * earlier hand copy silently dropped bytes and C zero-padded the [84]
 * array, shifting every field. The typedefs fail the build unless each
 * template is exactly 84 bytes. Regenerate from the captures, never edit. */
/* session 5 (pid 412) seq 73: preview scan @5250000, 3996-B blocks; the
 * CCD/lens positions @56/@60 are per-session (session 4 seq 292 differed only
 * in bytes 58-59, 62-63) */
static const BYTE SCAN_WIN[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x50,
    0x1b, 0xd0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x9a, 0x00, 0x00, 0x12, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0c, 0x00, 0xe6, 0x00, 0x01, 0x00, 0x03, 0xe3, 0xd7,
    0x00, 0x0f, 0x52, 0xd6, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char SCAN_WIN_is_84[sizeof SCAN_WIN == 84 ? 1 : -1];
/* session 5 (pid 412) seq 64719: end of scan / Stop, sent twice */
static const BYTE END_WIN[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x01, 0x00, 0x03, 0xe3, 0xd7,
    0x00, 0x0f, 0x52, 0xd6, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char END_WIN_is_84[sizeof END_WIN == 84 ? 1 : -1];
/* session 4 seq 67521: monitor idle heartbeat @9000000 (template for moves) */
static const BYTE IDLE_WIN[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x89,
    0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0xce, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00, 0x06, 0xb7, 0x31,
    0x00, 0x0d, 0xa8, 0xe4, 0x00, 0x00, 0x64, 0xc8, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x1e, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char IDLE_WIN_is_84[sizeof IDLE_WIN == 84 ? 1 : -1];
/* session 4 seq 67705: FlexColor shutdown descriptor @36000000 (rest position)
 */
static const BYTE EXIT_WIN[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x02, 0x25,
    0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x06, 0xb7, 0x31,
    0x00, 0x0d, 0xa8, 0xe4, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char EXIT_WIN_is_84[sizeof EXIT_WIN == 84 ? 1 : -1];
/* session 6 (pid 1428) seq 64676: 2000 ppi crop, 1818 px x 2807 lines from line
 * 9314 */
static const BYTE SCAN_2000[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x01, 0x31,
    0x2d, 0x00, 0x00, 0x00, 0x01, 0x9b, 0x00, 0x00, 0x24, 0x62, 0x00, 0x00,
    0x07, 0x1a, 0x00, 0x00, 0x0a, 0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0xe6, 0x00, 0x04, 0x00, 0x04, 0x49, 0x6f,
    0x00, 0x0f, 0x37, 0xb0, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char SCAN_2000_is_84[sizeof SCAN_2000 == 84 ? 1 : -1];
/* session 6 seq 225610: its end descriptor, sent twice */
static const BYTE END_2000[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x04, 0x00, 0x04, 0x49, 0x6f,
    0x00, 0x0f, 0x37, 0xb0, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char END_2000_is_84[sizeof END_2000 == 84 ? 1 : -1];
/* session 6 seq 225620: 1000 ppi crop, 1142 px x 1490 lines from line 6097 */
static const BYTE SCAN_1000[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x98,
    0x96, 0x80, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x17, 0xd1, 0x00, 0x00,
    0x04, 0x76, 0x00, 0x00, 0x05, 0xd2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x06, 0x00, 0xe6, 0x00, 0x02, 0x00, 0x04, 0x49, 0x6f,
    0x00, 0x0f, 0x37, 0xb0, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char SCAN_1000_is_84[sizeof SCAN_1000 == 84 ? 1 : -1];
/* session 6 seq 265797: its end descriptor, sent twice */
static const BYTE END_1000[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x02, 0x00, 0x04, 0x49, 0x6f,
    0x00, 0x0f, 0x37, 0xb0, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char END_1000_is_84[sizeof END_1000 == 84 ? 1 : -1];
/* session 6 seq 265806: 3f strip frame 1 of 6, 630 ppi, 655 px x 992 lines from
 * line 50 */
static const BYTE SCAN_3F[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x60,
    0x21, 0x60, 0x00, 0x00, 0x01, 0x56, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00,
    0x02, 0x8f, 0x00, 0x00, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x06, 0x00, 0xe6, 0x00, 0x02, 0x00, 0x06, 0xb2, 0x06,
    0x00, 0x0d, 0xe6, 0xca, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char SCAN_3F_is_84[sizeof SCAN_3F == 84 ? 1 : -1];
/* session 6 seq 296339: its end descriptor (all six frames), sent twice */
static const BYTE END_3F[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x02, 0x00, 0x06, 0xb2, 0x06,
    0x00, 0x0d, 0xe6, 0xca, 0x00, 0x00, 0x5f, 0xb4, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x1e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4,
};
typedef char END_3F_is_84[sizeof END_3F == 84 ? 1 : -1];

/* Descriptor geometry, all big-endian u32 (session 6: 2000/1000 ppi crops
 * and a 3f strip, checked against the TIFFs FlexColor saved):
 *  @10 resolution x 10000 (5250000 = 525 ppi preview, 63000000 = 6300 ppi
 *      max): line pitch along the strip.
 *  @14 first pixel across the film     @22 pixels per line (block = px*6)
 *  @18 first line along the strip      @26 number of lines
 *  @56 CCD position, @60 lens position, @64 drum position (motor steps,
 *      the CCD/OPTIC/DRUM columns of the scanner's FP<serial>.txt). The host
 *      picks them; they set magnification and focus, see F_CCD.
 * The six 3f frames differ ONLY in @18; each end descriptor is its scan
 * descriptor with bytes 8-52 zeroed. Never command outside the observed
 * range. */
#define POS_OFF 10
#define POS_MAX 63000000L
#define F_X 14
#define F_Y 18
#define F_PX 22
#define F_LINES 26
#define F_BIN                                                                  \
  51 /* CCD pixels summed per output pixel: 12 preview,                        \
        6 for 1000 ppi and 3f, 3 for 2000 ppi; byte 55                         \
        is always 12 / bin. Output pixel j covers CCD                          \
        pixels (x + j) * bin + 144 ... + bin - 1 */

/* Captured scan setups. Each pairs a scan descriptor with the end
 * descriptor FlexColor sent after it. */
static const struct preset {
  const char *name;
  const BYTE *scan, *end;
  const char *what;
} PRESETS[] = {
    {"preview", SCAN_WIN, END_WIN, "525 ppi, whole holder"},
    {"2000", SCAN_2000, END_2000, "2000 ppi, one frame (session 6 crop)"},
    {"1000", SCAN_1000, END_1000, "1000 ppi, one frame (session 6 crop)"},
    {"3f", SCAN_3F, END_3F, "3f raw, 630 ppi, autoframe frame"},
};
/* 3f autoframe start lines for frames 1-6 of the 24x36x6 holder */
static const long F3_Y[6] = {50, 967, 1910, 2852, 3795, 4737};
/* holder length in lines at 525 ppi (preview @26); scales with resolution */
#define HOLDER_LINES_525 FT_PREVIEW_LINES_525

/* Lens and CCD positions (BE u32, motor steps; 2026-10-12 sweep): moving
 * either one changes magnification and focus, and with them the white
 * level (fewer CCD pixels per mm of film collect more light). Neither is an
 * exposure: the same pair gives the same white level at 1000-6300 ppi,
 * whatever the line rate. FlexColor sends one pair per optical scale,
 * ppi x bin = CCD pixels per inch of film:
 *   6300  254935 / 1004246   preview (session 4: 254123 / 1004988)
 *   6000  280943 /  997296   1000 and 2000 ppi crops (square pixels)
 *   3780  438790 /  911050   3f
 *   3600  440113 /  895204   idle and exit descriptors
 * and 1020000 / 1021000 in session 4. The two seem to share a rail: the
 * CCD stays below the lens (see parse_pos). The limits are FT_CCD_MIN..
 * FT_OPTIC_MAX in libft/ft_focus.h. */
#define F_CCD 56
#define F_OPTIC 60
typedef char
    F_CCD_is_libft[F_CCD == FT_DESC_CCD && F_OPTIC == FT_DESC_OPTIC ? 1 : -1];
static long g_ccd = -1, g_optic = -1; /* -1: keep the template's */

/* With the CCD past the lens (1020000 / 997296) the calibration lines
 * flickered, the white batch never came and the carriage did not move to
 * the start line. Scans keep the lens >= FT_GAP_MIN steps below the CCD
 * (see libft/ft_focus.h). */
static int pos_ok(long a, long b) {
  if (ft_positions_ok(a, b))
    return 1;
  fprintf(stderr,
          "CCD/lens %ld/%ld outside the safe range (CCD "
          "%ld..%ld, lens %ld..%ld, lens - CCD >= %ld)\n",
          a, b, FT_CCD_MIN, FT_CCD_MAX, FT_OPTIC_MIN, FT_OPTIC_MAX, FT_GAP_MIN);
  return 0;
}

static int parse_pos(const char *v) {
  long a, b;
  if (sscanf(v, "%ld,%ld", &a, &b) != 2) {
    fprintf(stderr, "-p wants <ccd>,<lens>\n");
    return 1;
  }
  if (!pos_ok(a, b))
    return 1;
  g_ccd = a;
  g_optic = b;
  return 0;
}

/* Lamp level (u8 @76), the only light lever (2026-10-12): FlexColor's
 * maintenance Monitor sends 0..255 here as its lamp spinner moves; its
 * scan descriptors send 0, its idle/exit descriptors 1. 0 keeps the
 * current level, any other value sets it and it sticks across scans.
 * White level vs lamp (1000 ppi crop optics, R): 1 -> 47 %, 32 -> 67 %,
 * 64 -> 84 %, 80 -> 92 %, 96..255 -> 100 % (flat), no hysteresis; dimmer
 * is slightly redder. Nothing raises the light above 96; integration
 * time is not a descriptor field. */
#define F_LAMP 76
static int g_lamp = -1; /* -1: keep the template's */

static int parse_lamp(const char *v) {
  char *end;
  long l = strtol(v, &end, 10);
  if (*end || !(l >= 0 && l <= 255)) {
    fprintf(stderr, "-l wants a lamp level 0..255\n");
    return 1;
  }
  g_lamp = (int)l;
  return 0;
}

/* Ctrl+C / Ctrl+Break (scan --job, frames, calibrate-focus): read_flood
 * stops, cmd_scan still sends the end descriptor, and no new scan starts. */
static volatile int g_abort;

/* --progress (any position): machine-readable lines for the web server
 * (ft_progress.h) instead of the \r line counter. Lines are counted only
 * while a unit's own scan runs (g_pg_track), not during focus crops. */
static int g_progress, g_pg_track, g_pg_refused;
static long g_pg_crops; /* focus crops in this unit */
static int g_pg_frame;  /* the frame focus crops are on */

static void pg_emit(const ft_progress *p) {
  char buf[320];
  if (g_progress && ft_progress_fmt(p, buf, sizeof buf) < sizeof buf) {
    fputs(buf, stdout);
    fflush(stdout);
  }
}

static void pg_begin(ft_pg_kind kind, int n) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_BEGIN;
  p.kind = kind;
  p.n = n;
  pg_emit(&p);
}

static void pg_unit(ft_pg_kind kind, int i, int label) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_UNIT;
  p.kind = kind;
  p.n = i;
  p.label = label;
  g_pg_crops = 0;
  pg_emit(&p);
}

static void pg_lines(long k, long total) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_LINES;
  p.k = k;
  p.total = total;
  pg_emit(&p);
}

static void pg_focus(long k, long off) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_FOCUS;
  p.k = k;
  p.label = g_pg_frame;
  p.off = off;
  pg_emit(&p);
}

static void pg_sharp(long off, double s) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_SHARP;
  p.off = off;
  p.sharp = s;
  pg_emit(&p);
}

static void pg_focused(long off, ft_pg_how how) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_FOCUSED;
  p.off = off;
  p.how = how;
  pg_emit(&p);
}

static void pg_file(const char *path) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_FILE;
  if (strlen(path) < sizeof p.path) {
    strcpy(p.path, path);
    pg_emit(&p);
  }
}

static void pg_end(ft_pg_status s) {
  ft_progress p;
  memset(&p, 0, sizeof p);
  p.type = FT_PG_END;
  p.status = s;
  pg_emit(&p);
}

/* The first scan of the process holds at least this long before its
 * calibration, whatever the caller asks (lamp warm-up); frames, scan --job
 * and calibrate-focus set it with -h, 0..120 s (e.g. 35 s after a cold
 * power-up). An automatic microcode upload (a power-up: the lamp is
 * cold) raises it to FIRST_HOLD_COLD; their -h, parsed later, still
 * wins. */
#define FIRST_HOLD_COLD 35000
static DWORD g_first_hold = 10000;

/* -h: hold before calibration, 0..120 s */
static int parse_hold(const char *v, DWORD *ms) {
  char *end;
  long s = strtol(v, &end, 10);
  if (end == v || *end || !(s >= 0 && s <= 120)) {
    fprintf(stderr, "-h wants a hold of 0..120 s\n");
    return 1;
  }
  *ms = (DWORD)s * 1000;
  return 0;
}

static long desc_pos(const BYTE *d) { return read_big_endian32(d, POS_OFF); }

static void set_pos(BYTE *d, long pos) { write_big_endian32(d, POS_OFF, pos); }

static int cmd_inquiry(void) {
  BYTE buf[96];
  int r, i;
  memset(buf, 0, sizeof buf);
  r = srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
  if (r < 0) {
    printf("INQUIRY: %s\n", srb_errstr(r));
    return 1;
  }
  printf("INQUIRY: %d bytes\n", r);
  printf("  vendor : %.8s\n", buf + 8);
  printf("  product: %.16s\n", buf + 16);
  printf("  version: %.4s\n", buf + 32);
  printf("  raw    :");
  for (i = 0; i < r; i++)
    printf("%s %02x", i && i % 16 == 0 ? "\n          " : "", buf[i]);
  printf("\n");
  /* microcode live fields (firmware walkthrough §4, handler 3000:2199) */
  if (r > 0x36 && !ft_upload_is_loader(buf, (size_t)r))
    printf("  live   : sensors(0x310) %02x  ring write 8614 %" PRIu32
           " read 8610 %" PRIu32 " state 8ecd %u  phase 8f36 %u\n",
           buf[0x24], read_little_endian32(buf + 0x25),
           read_little_endian32(buf + 0x29), buf[0x35], buf[0x36]);
  /* Loader identity means the microcode upload dance is required first. */
  if (ft_upload_is_loader(buf, (size_t)r))
    printf("  NOTE: loader firmware (after a power-up) — any other "
           "command uploads MICROCOD.3XX first.\n");
  return 0;
}

static const char *poll_name(unsigned v) {
  switch (v) {
  case 0x0000:
    return "idle";
  case 0x0400:
    return "busy";
  case 0x1060:
    return "DATA READY";
  case 0x9299:
    return "monitor-live";
  case 0x0800:
    return "pending (7639)";
  case 0x0500:
    return "move ack";
  case 0x4894:
    return "settling";
  case 0xbb12:
    return "position done";
  case 0x38c9:
    return "upload commit";
  case 0x9000:
    return "scan armed";
  default:
    return "?";
  }
}

/* One ScannerReady poll; returns the 16-bit status (big-endian on the wire)
 * or a negative transport code. */
static int poll_once(void) {
  BYTE buf[2];
  int r;
  memset(buf, 0, sizeof buf);
  r = srb_exec(CDB_POLL, 6, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
  if (r < 0)
    return r;
#ifndef _WIN32
  /* SG_IO completes an underrun as a short read (scsiscan: -3); a
   * short status reply is no status, never "idle" (the zeroed buffer) */
  if (r != (int)sizeof buf)
    return IO_SHORT;
#endif
  return (buf[0] << 8) | buf[1];
}

static int cmd_poll(int n, int quiet) {
  int i;
  for (i = 0; i < n; i++) {
    int v = poll_once();
    if (v < 0) {
      printf("poll %d: %s\n", i, srb_errstr(v));
      return 1;
    }
    if (!quiet)
      printf("poll %d: %04x  %s\n", i, v, poll_name((unsigned)v));
    else if (i == 0 || i == n - 1)
      printf("poll %d: %04x  %s\n", i, v, poll_name((unsigned)v));
  }
  return 0;
}

/* One SetWindow exchange: send descriptor, device overlays status into the
 * same buffer. Returns transport code (>=0 = bytes) and prints both sides
 * when trace is set. */
static int win_exchange(BYTE desc[84], int trace) {
  BYTE send[84];
  int r, i;
  memcpy(send, desc, 84);
  r = srb_exec(CDB_WIN, 10, SRB_DIR_OUT, desc, 84, TIMEOUT_MS);
  if (trace) {
    printf("  sent:");
    for (i = 0; i < 84; i++)
      printf(" %02x", send[i]);
    printf("\n  recv:");
    for (i = 0; i < 84; i++)
      printf(" %02x", desc[i]);
    printf("\n");
  }
  return r;
}

/* Send one template verbatim (copied: the device overlays status). */
static int send_win(const BYTE *tmpl, const char *label) {
  BYTE desc[84];
  int r;
  memcpy(desc, tmpl, 84);
  r = win_exchange(desc, 0);
  if (r < 0)
    printf("SetWindow (%s): %s\n", label, srb_errstr(r));
  return r < 0 ? 1 : 0;
}

static int cmd_win(void) {
  BYTE desc[84];
  int r;
  memcpy(desc, IDLE_WIN, 84);
  r = win_exchange(desc, 1);
  if (r < 0) {
    printf("SetWindow: %s\n", srb_errstr(r));
    return 1;
  }
  printf("SetWindow: %d bytes; device state bytes 0-7 = %02x %02x %02x %02x "
         "%02x %02x %02x %02x\n",
         r, desc[0], desc[1], desc[2], desc[3], desc[4], desc[5], desc[6],
         desc[7]);
  return 0;
}

/* Wait until the device reports idle (df == 0000) after a command that
 * may take a moment to start: a 0000 only counts once something else was
 * seen, or after 3 s of uninterrupted 0000. Prints status changes only. */
static int wait_settled(DWORD timeout_ms) {
  DWORD start = io_ms(), now;
  int v, last = -1, seen_busy = 0;
  for (;;) {
    v = poll_once();
    if (v < 0)
      return v;
    now = io_ms() - start;
    if (v != last)
      printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
             (unsigned long)now);
    last = v;
    if (v != 0x0000)
      seen_busy = 1;
    else if (seen_busy || now > 3000)
      return 0;
    if (now > timeout_ms)
      return -100;
    io_sleep(150);
  }
}

/* Absolute move: idle template with the position patched, then settle. */
static int move_to(long pos, const char *why) {
  BYTE desc[84];
  int r;
  if (!(pos >= 0 && pos <= POS_MAX)) {
    fprintf(stderr, "refusing position %ld (observed range 0..%ld)\n", pos,
            POS_MAX);
    return 1;
  }
  memcpy(desc, IDLE_WIN, 84);
  set_pos(desc, pos);
  printf("move -> %ld (%s)\n", pos, why);
  r = win_exchange(desc, 0);
  if (r < 0) {
    printf("SetWindow: %s\n", srb_errstr(r));
    return 1;
  }
  r = wait_settled(60000);
  if (r != 0) {
    printf("warning: carriage did not report idle (%d)\n", r);
    return 1;
  }
  g_pos = pos;
  g_pos_known = 1;
  printf("at %ld, idle\n", pos);
  return 0;
}

/* Rest position = FlexColor's own shutdown descriptor, sent verbatim. */
static int cmd_park(void) {
  printf("park -> %ld (FlexColor shutdown descriptor)\n", desc_pos(EXIT_WIN));
  if (send_win(EXIT_WIN, "park"))
    return 1;
  if (wait_settled(60000)) {
    printf("warning: carriage did not report idle\n");
    return 1;
  }
  g_pos = desc_pos(EXIT_WIN);
  g_pos_known = 1;
  printf("parked at %ld, idle\n", g_pos);
  return 0;
}

/* End-of-scan descriptor x2: what FlexColor sends both after the last
 * ReadExt of a full preview and when the user stops a preview halfway
 * (session 5, pid 412 seq 64719 / 80710). FlexColor does not park after. */
static int cmd_stop(void) {
  printf("end of scan (END_WIN x2)\n");
  return send_win(END_WIN, "end") || send_win(END_WIN, "end");
}

/* 0xdb ScanStop: in FlexColor's opcode table (kHwiScanStop) but never seen
 * on the wire; 6-byte by the CDB length rule, no data phase. */
static int cmd_scanstop(void) {
  /* scsiscan rejects a NULL carrier (GLE 87): try no-data flags with a
   * dummy buffer first, then a 2-byte data-in. */
  BYTE dummy[2] = {0, 0};
  int r = srb_exec(CDB_SCANSTOP, 6, 0x00, dummy, 0, TIMEOUT_MS);
  printf("ScanStop (no data): %s\n", r >= 0 ? "ok" : srb_errstr(r));
  if (r < 0) {
    r = srb_exec(CDB_SCANSTOP, 6, SRB_DIR_IN, dummy, 2, TIMEOUT_MS);
    printf("ScanStop (2-byte in): %s\n", r >= 0 ? "ok" : srb_errstr(r));
  }
  return r < 0;
}

static int cmd_goto(long pos) { return move_to(pos, "absolute"); }

static int cmd_move(long delta, int steps) {
  int s;
  if (steps < 1)
    steps = 1;
  if (steps > 50) {
    fprintf(stderr, "steps capped at 50\n");
    return 1;
  }
  if (!(delta != 0 && delta >= -5000000L && delta <= 5000000L)) {
    fprintf(stderr,
            "per-step delta must be within +/-5000000 "
            "(given %ld)\n",
            delta);
    return 1;
  }
  if (!g_pos_known) {
    printf("position unknown — parking first for a known reference\n");
    if (cmd_park())
      return 1;
  }
  for (s = 0; s < steps; s++) {
    if (g_abort) {
      printf("interrupted after %d of %d steps\n", s, steps);
      return 1;
    }
    if (move_to(g_pos + delta, "relative step"))
      return 1;
  }
  return 0;
}

static int read_pages(int verbose) {
  /* CDB: 3c 00 SS SS OO OO LL LL LL 00. The microcode treats READ BUFFER
   * as a memory read: segment @2, offset @4 (16-bit big-endian each),
   * 24-bit big-endian length @6. So "buffer e0, offset 0x400" is memory
   * E000:0400 (Faul Pixs, boot flash) and d8 is D800:xxxx. The id/off
   * fields below encode the same bytes; see cmd_maint for raw peeks. */
  static const struct {
    BYTE id;
    DWORD off;
    WORD len;
    const char *name;
  } pages[] = {
      {0xd8, 0x000000, 0x0042, "descriptor page"},
      {0xd8, 0x000037, 0x000a, "descriptor tail"},
      {0xe0, 0x000400, 0x0016, "FaulPixs header"},
      {0xe0, 0x000400, 0x013e, "FaulPixs table"},
      {0xe0, 0x000000, 0x0400, "buffer e0/400"},
  };
  size_t i;
  for (i = 0; i < sizeof pages / sizeof pages[0]; i++) {
    BYTE cdb[10], buf[1024];
    int r, j;
    memcpy(cdb, CDB_PAGE, 10);
    cdb[2] = pages[i].id;
    cdb[3] = (BYTE)(pages[i].off >> 16);
    cdb[4] = (BYTE)(pages[i].off >> 8);
    cdb[5] = (BYTE)(pages[i].off & 0xff);
    cdb[7] = (BYTE)(pages[i].len >> 8);
    cdb[8] = (BYTE)(pages[i].len & 0xff);
    memset(buf, 0, sizeof buf);
    r = srb_exec(cdb, 10, SRB_DIR_IN, buf, pages[i].len, TIMEOUT_MS);
    if (r < 0) {
      printf("%s: %s\n", pages[i].name, srb_errstr(r));
      continue;
    }
    if (!verbose)
      continue;
    printf("%s (%02x/%06lx, %u bytes):", pages[i].name, pages[i].id,
           (unsigned long)pages[i].off, pages[i].len);
    for (j = 0; j < r && j < 48; j++)
      printf(" %02x", buf[j]);
    printf("%s\n", r > 48 ? " ..." : "");
  }
  return 0;
}

#define MAINT_SETTLE_MS 3000 /* bus silence after a maintenance write */

/* Poll until idle (0000), printing every status; 1 after 10 s. Poll 0800
 * = flag 7639 "3b/24 command pending" (the df reply is 08 00 while it is
 * set), not a motor state. */
static int maint_wait_idle(void) {
  DWORD start = io_ms();
  int v;
  for (;;) {
    v = poll_once();
    printf("  poll %04x %s\n", v < 0 ? 0 : v,
           v < 0 ? "poll failed" : poll_name((unsigned)v));
    if (v == 0x0000)
      return 0;
    if (io_ms() - start > 10000) {
      printf("not idle after 10 s: STOP, power-cycle and re-upload "
             "(README: Maintenance writes)\n");
      return 1;
    }
    io_sleep(200);
  }
}

/* maint dump-flash <file>: the non-volatile memory, as a backup in case a
 * flash is ever damaged. File layout: E000:0000..F000:FFFF (upper chip
 * select: parameter flash, Faul Pixs, reset stub; 128 KiB), then
 * C000:0000..7FFF (loader ROM, chip select 3; 32 KiB). 1 KiB READ BUFFER
 * pieces; the first is byte for byte FlexColor's "buffer e0/400" read.
 * Reads only; run it twice and compare the files. */
static int maint_dump_flash(const char *path) {
  static const struct {
    unsigned seg;
    unsigned long len;
  } part[] = {
      {0xe000, 0x10000},
      {0xf000, 0x10000},
      {0xc000, FT_MAINT_LOADER_LEN},
  };
  static BYTE img[2 * 0x10000 + FT_MAINT_LOADER_LEN];
  BYTE cdb[10];
  unsigned seg, off;
  unsigned long sum = 0, fnv = 2166136261UL;
  size_t at = 0, i, p;
  ft_err e;
  FILE *f;
  int r, v;

  v = poll_once();
  if (v != 0x0000) {
    fprintf(stderr, "maint: scanner not idle (%04x %s); refusing\n",
            v < 0 ? 0 : v, v < 0 ? "poll failed" : poll_name((unsigned)v));
    return 1;
  }
  for (p = 0; p < sizeof part / sizeof part[0]; p++) {
    seg = part[p].seg;
    for (off = 0; off < part[p].len; off += FT_MAINT_MAX_PEEK) {
      if (ft_maint_peek(seg, off, FT_MAINT_MAX_PEEK, cdb, &e) < 0) {
        fprintf(stderr, "%s\n", e.msg);
        return 1;
      }
      r = srb_exec(cdb, 10, SRB_DIR_IN, img + at, FT_MAINT_MAX_PEEK,
                   TIMEOUT_MS);
      if (r != FT_MAINT_MAX_PEEK) {
        printf("%04x:%04x: %s; dump incomplete, nothing written\n", seg, off,
               r < 0 ? srb_errstr(r) : "short read");
        maint_wait_idle();
        return 1;
      }
      at += FT_MAINT_MAX_PEEK;
      if (off % 0x4000 == 0x3c00)
        printf("  %04x:%04x done\n", seg, off);
    }
  }
  for (i = 0; i < at; i++) {
    sum += img[i];
    fnv = ((fnv ^ img[i]) * 16777619UL) & 0xffffffffUL;
  }
  f = fopen(path, "wb");
  if (!f || fwrite(img, 1, at, f) != at) {
    perror(path);
    if (f)
      fclose(f);
    return 1;
  }
  fclose(f);
  printf("wrote %s: %lu bytes, sum %08lx, fnv1a %08lx\n", path,
         (unsigned long)at, sum & 0xffffffffUL, fnv);
  printf("E000:0400: \"%.16s\" (Faul Pixs header)\n",
         (const char *)img + 0x400);
  printf("F000:FFF0:");
  for (i = 0x1fff0; i < 0x20000; i++)
    printf(" %02x", img[i]);
  printf(" (reset vector)\n");
  for (i = 0x20000; i + 11 <= at; i++)
    if (memcmp(img + i, "SCSI Loader", 11) == 0)
      break;
  if (i + 11 <= at)
    printf("C000:%04lx: \"SCSI Loader\" (loader identity)\n",
           (unsigned long)(i - 0x20000));
  else
    printf("no \"SCSI Loader\" string in C000:0000..7FFF\n");
  return maint_wait_idle();
}

/* maint io <0x342|0x344> <byte> --confirm
 * maint mem 4000:<off> <hexbytes> --confirm
 * maint peek <3000|4000|c000|e000|f000>:<off> <len>
 * maint dump-flash <file>
 * One whitelisted microcode maintenance packet (libft/ft_maint.c), only
 * while the scanner is idle. Writes complete on the bus before the device
 * acts on them, so "send ok" proves nothing: check with peek. */
static int cmd_maint(int argc, char **argv) {
  /* buf holds a peek reply (<= FT_MAINT_MAX_PEEK) or a write packet
   * (FT_MAINT_HDR + FT_MAINT_MAX_DATA, smaller) */
  BYTE cdb[10], buf[FT_MAINT_MAX_PEEK], data[FT_MAINT_MAX_DATA];
  ft_err e;
  int len, i, v, r, peek = 0;
  unsigned seg = 0, off = 0;
  size_t n = 0;

  if (argc == 2 && strcmp(argv[0], "dump-flash") == 0)
    return maint_dump_flash(argv[1]);
  if (argc == 3 && strcmp(argv[0], "peek") == 0) {
    unsigned long cnt = strtoul(argv[2], NULL, 0);
    if (sscanf(argv[1], "%x:%x", &seg, &off) != 2) {
      fprintf(stderr, "maint: address must be seg:off in hex\n");
      return 2;
    }
    len = ft_maint_peek(seg, off, (size_t)cnt, cdb, &e) < 0 ? -1 : (int)cnt;
    peek = 1;
  } else if (argc == 4 && strcmp(argv[3], "--confirm") == 0 &&
             strcmp(argv[0], "io") == 0) {
    unsigned long port = strtoul(argv[1], NULL, 0);
    unsigned long val = strtoul(argv[2], NULL, 0);
    if (val > 0xff) {
      fprintf(stderr, "maint: byte > 0xff\n");
      return 2;
    }
    len = ft_maint_io((unsigned)port, (unsigned char)val, cdb, buf, sizeof buf,
                      &e);
  } else if (argc == 4 && strcmp(argv[3], "--confirm") == 0 &&
             strcmp(argv[0], "mem") == 0) {
    const char *h = argv[2];
    if (sscanf(argv[1], "%x:%x", &seg, &off) != 2) {
      fprintf(stderr, "maint: address must be seg:off in hex\n");
      return 2;
    }
    while (h[0] && h[1] && n < sizeof data) {
      unsigned b;
      if (sscanf(h, "%2x", &b) != 1)
        break;
      data[n++] = (BYTE)b;
      h += 2;
    }
    if (*h) {
      fprintf(stderr, "maint: bad hex bytes\n");
      return 2;
    }
    len = ft_maint_mem(seg, off, data, n, cdb, buf, sizeof buf, &e);
  } else {
    fprintf(stderr, "maint io <0x342|0x344> <byte> --confirm\n"
                    "maint mem 4000:<off> <hexbytes> --confirm\n"
                    "maint peek <3000|4000|c000|e000|f000>:<off> <len>\n"
                    "maint dump-flash <file>\n");
    return 2;
  }
  if (len < 0) {
    fprintf(stderr, "%s\n", e.msg);
    return 2;
  }

  v = poll_once();
  if (v != 0x0000) {
    fprintf(stderr, "maint: scanner not idle (%04x %s); refusing\n",
            v < 0 ? 0 : v, v < 0 ? "poll failed" : poll_name((unsigned)v));
    return 1;
  }
  printf("cdb:");
  for (i = 0; i < 10; i++)
    printf(" %02x", cdb[i]);
  printf("\n");

  if (peek) {
    memset(buf, 0, sizeof buf);
    r = srb_exec(cdb, 10, SRB_DIR_IN, buf, (DWORD)len, TIMEOUT_MS);
    if (r < 0) {
      printf("peek: %s\n", srb_errstr(r));
      return 1;
    }
    for (i = 0; i < r; i++) {
      if (i % 16 == 0)
        printf("%s%04x:%04x", i ? "\n" : "", seg, off + i);
      printf(" %02x", buf[i]);
    }
    printf("\n");
    return maint_wait_idle();
  }

  printf("data:");
  for (i = 0; i < len; i++)
    printf(" %02x", buf[i]);
  printf("\n");
  r = srb_exec(cdb, 10, SRB_DIR_OUT, buf, (DWORD)len, TIMEOUT_MS);
  printf("send: %s\n", r < 0 ? srb_errstr(r) : "ok (check with peek)");
  if (r < 0)
    return 1;
  /* The completed 3b sets "pending" (7639, poll 0800) and the main loop
   * runs it only while the last CDB opcode (8f42) is still 3b: any
   * command sent before that (a poll) overwrites 8f42, the packet is never
   * run and 7639 stays set until the next scan. So stay off the bus for
   * a while first. */
  io_sleep(MAINT_SETTLE_MS);
  if (maint_wait_idle()) {
    printf("still pending: the write was probably NOT executed\n");
    return 1;
  }
  return 0;
}

/* Save the device's Faul Pixs table (e0 @ 0x400, 318 B; ASCII, per
 * channel a "100 0" line then "<ccd pixel>\t<+999 | -n>" lines) so the
 * host can interpolate the defective CCD pixels, as FlexColor does. */
static void save_faul(const char *path) {
  BYTE cdb[10], buf[0x13e];
  FILE *f;
  int r;
  memcpy(cdb, CDB_PAGE, 10);
  cdb[2] = 0xe0;
  cdb[4] = 0x04;
  cdb[7] = 0x01;
  cdb[8] = 0x3e;
  r = srb_exec(cdb, 10, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
  if (r < 0) {
    printf("Faul Pixs: %s\n", srb_errstr(r));
    return;
  }
  f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  fwrite(buf, 1, (size_t)r, f);
  fclose(f);
}

static int cmd_grab(long blocks, WORD blklen, const char *out) {
  BYTE cdb[10];
  BYTE *buf;
  FILE *f;
  long b;
  int bad = 0;

  if (blklen == 0)
    blklen = 3996;
  if (blklen > 65000) {
    fprintf(stderr, "block len too big\n");
    return 1;
  }
  buf = malloc(blklen);
  if (!buf)
    return 1;
  memcpy(cdb, CDB_GRAB, 10);
  cdb[7] = (BYTE)(blklen >> 8);
  cdb[8] = (BYTE)(blklen & 0xff);

  f = fopen(out, "wb");
  if (!f) {
    perror(out);
    free(buf);
    return 1;
  }

  for (b = 0; b < blocks; b++) {
    int r = srb_exec(cdb, 10, SRB_DIR_IN, buf, blklen, TIMEOUT_MS);
    if (r < 0) {
      printf("grab %ld: %s, stopping\n", b, srb_errstr(r));
      bad = 1;
      break;
    }
    if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
      bad = 1;
      break;
    }
    if (blocks > 20 && (b % 100) == 0)
      printf("grab %ld/%ld\r", b, blocks), fflush(stdout);
  }
  fclose(f);
  free(buf);
  printf("\nwrote %ld blocks x %u bytes -> %s%s\n", blocks, blklen, out,
         bad ? " (INCOMPLETE)" : "");
  return bad;
}

/* GET WINDOW (0x25, handler 3000:22ee): the firmware writes the live CCD
 * (8ed8) and lens (8ee0) positions big-endian at @56/@60 of its window
 * buffer and returns it. Length in CDB bytes 6-7, as SET WINDOW. */
static int cmd_getwin(void) {
  static const BYTE cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0x54, 0, 0};
  BYTE buf[0x100];
  int r, i;
  memset(buf, 0, sizeof buf);
  r = srb_exec(cdb, 10, SRB_DIR_IN, buf, 0x54, TIMEOUT_MS);
  if (r < 0) {
    printf("getwin: %s\n", srb_errstr(r));
    return 1;
  }
  printf("getwin: %d bytes", r);
  for (i = 0; i < r && i < 0x54; i++)
    printf("%s %02x", i % 16 == 0 ? "\n  " : "", buf[i]);
  printf("\n  @56 ccd %" PRIu32 " @60 lens %" PRIu32 "\n",
         read_big_endian32(buf, F_CCD), read_big_endian32(buf, F_OPTIC));
  return 0;
}

/* --probe L[:kinds][,L...] and --stall L:ms (scan, crop): verification
 * hooks in the image flood only (not calibration). At kept line L the read
 * queue is drained, then: i = INQUIRY (live ring indices, state), g = GET
 * WINDOW, p = peek 4000:8ed0 (drum pos + target). Default kinds "i".
 * A stall stops reading for ms at line L (host-stall test). */
#define MAX_PROBES 16
static struct {
  long line;
  char kinds[4];
} g_probe[MAX_PROBES];
static int g_nprobe;
static long g_stall_line = -1;
static DWORD g_stall_ms;
static int g_hooks_on; /* set around the image flood only */

static int parse_probe(const char *s) {
  while (*s) {
    char *e;
    long l = strtol(s, &e, 10);
    size_t k = 0;
    if (e == s || l < 0 || g_nprobe == MAX_PROBES)
      return 1;
    g_probe[g_nprobe].line = l;
    if (*e == ':')
      for (e++; *e && *e != ',' && k < 3; e++) {
        if (!strchr("igp", *e))
          return 1;
        g_probe[g_nprobe].kinds[k++] = *e;
      }
    if (!k)
      g_probe[g_nprobe].kinds[k++] = 'i';
    g_probe[g_nprobe].kinds[k] = 0;
    g_nprobe++;
    if (*e == ',')
      e++;
    else if (*e)
      return 1;
    s = e;
  }
  return 0;
}

static void run_probe(long line, const char *kinds, DWORD t) {
  BYTE cdb[10], buf[96];
  ft_err e;
  int r, i;
  printf("\nprobe line %ld (+%lu ms):\n", line, (unsigned long)t);
  for (; *kinds; kinds++) {
    if (*kinds == 'i') {
      cmd_inquiry();
    } else if (*kinds == 'g') {
      cmd_getwin();
    } else if (*kinds == 'p' &&
               ft_maint_peek(0x4000, 0x8ed0, 8, cdb, &e) >= 0) {
      memset(buf, 0, sizeof buf);
      r = srb_exec(cdb, 10, SRB_DIR_IN, buf, 8, TIMEOUT_MS);
      if (r < 0) {
        printf("peek 8ed0: %s\n", srb_errstr(r));
        continue;
      }
      printf("peek 4000:8ed0:");
      for (i = 0; i < r; i++)
        printf(" %02x", buf[i]);
      printf("  drum pos %" PRIu32 " tgt %" PRIu32 "\n",
             read_little_endian32(buf), read_little_endian32(buf + 4));
    }
  }
  fflush(stdout);
}

/* Pipelined flood: keep DEPTH ReadExt requests queued (ftio.h io_queue:
 * overlapped on Windows, sg's async interface on Linux) so the port
 * driver issues them back-to-back (FlexColor reaches ~1700 reads/s;
 * one-at-a-time reads top out at ~460/s).
 * Most reads come back EMPTY (an underrun: 0 bytes, SRB 0x12 on
 * Windows; 10 marker bytes on Linux): the CCD delivers ~130 lines/s and the
 * flood is really a polling loop. Reads continue until n full blocks are kept.
 * Completes in order; returns the clean in-order prefix written to f. */
#define DEPTH 8

static long read_flood(long n, WORD blklen, FILE *f) {
  io_queue *q;
  BYTE cdb[10];
  long issued = 0, done = 0, kept = 0, shorts = 0, short_bytes = 0;
  DWORD last_full;
  int i, err = 0, hook_due = 0, stalled = 0;
  char probed[MAX_PROBES] = {0};
  DWORD t0 = io_ms(), dt;

  memcpy(cdb, CDB_GRAB, 10);
  cdb[7] = (BYTE)(blklen >> 8);
  cdb[8] = (BYTE)(blklen & 0xff);
  q = ioq_open(DEPTH, cdb, blklen, TIMEOUT_MS);
  if (!q)
    return 0;

  last_full = io_ms();
  while (kept < n) {
    if (g_abort && !err) {
      printf("\ninterrupted\n");
      err = 1;
    }
    /* top up the queue: empty reads are normal, so the number of reads
     * is open-ended; stop issuing once the queue could fill n */
    if (g_hooks_on && !err) {
      hook_due = !stalled && g_stall_line >= 0 && kept >= g_stall_line;
      for (i = 0; i < g_nprobe; i++)
        if (!probed[i] && kept >= g_probe[i].line)
          hook_due = 1;
    }
    /* a hook runs on an empty queue: stop issuing until it has */
    if (hook_due && done == issued) {
      for (i = 0; i < g_nprobe; i++)
        if (!probed[i] && kept >= g_probe[i].line) {
          probed[i] = 1;
          run_probe(kept, g_probe[i].kinds, io_ms() - t0);
        }
      if (!stalled && g_stall_line >= 0 && kept >= g_stall_line) {
        stalled = 1;
        printf("\nstall at line %ld: no reads for %lu ms\n", kept,
               (unsigned long)g_stall_ms);
        fflush(stdout);
        io_sleep(g_stall_ms);
      }
      hook_due = 0;
      last_full = io_ms();
    }
    while (!err && !hook_due && kept + (issued - done) < n &&
           issued - done < DEPTH) {
      if (ioq_issue(q, (int)(issued % DEPTH), issued)) {
        err = 1;
        break;
      }
      issued++;
    }
    if (done == issued) {
      if (hook_due)
        continue;
      break;
    }
    if (io_ms() - last_full > 5000) {
      printf("\nno full block for 5 s; scan data ended?\n");
      err = 1;
    }
    /* reap the oldest */
    {
      int slot = (int)(done % DEPTH);
      BYTE *buf = ioq_buf(q, slot);
      DWORD got = 0;
      int r = ioq_reap(q, slot, done, TIMEOUT_MS, &got);
      if (r == IOQ_TIMEOUT) {
        printf("\nflood read %ld: timed out\n", done);
        err = 1;
        break;
      }
      if (r == IOQ_SHORT) {
        /* a short transfer, the host is ahead of the CCD: no
         * line. Its bytes are never kept: on Windows there are
         * none, on Linux every empty read brings 10 (ff ff ff ff
         * ff ff 00 00 00 00), and any kept would shift every
         * later line of the file. Counted in the summary only
         * (one line per read would bury ftwebd's log). */
        shorts++;
        short_bytes += got;
      } else if (r != IOQ_FULL) {
        err = 1; /* ioq_reap said why */
      } else if (!err) {
        if (f && fwrite(buf, 1, got, f) != got)
          err = 1;
        else if (!f && detail_put(buf, got))
          err = 1;
        else
          kept++;
        last_full = io_ms();
      }
      done++;
      if (g_progress) {
        if (g_pg_track && !err && got == blklen &&
            (kept % 50 == 0 || kept == n))
          pg_lines(kept, n);
      } else if (!err && kept && kept % 500 == 0 && got == blklen) {
        printf("  %ld/%ld\r", kept, n), fflush(stdout);
      }
    }
  }
  ioq_close(q, err);
  dt = io_ms() - t0;
  printf("  [flood: %ld reads in %lu ms; %ld full, %ld short (%ld bytes)]\n",
         done, (unsigned long)dt, kept, shorts, short_bytes);
  return kept;
}

/* End descriptor x2, as FlexColor after the last line or on Stop. */
static int send_end(const struct preset *p) {
  printf("end of scan (%s end descriptor x2)\n", p->name);
  return send_win(p->end, "end") || send_win(p->end, "end");
}

/* Poll (265 ms, like FlexColor) until the carriage move that follows a
 * calibration batch is over: idle, and at least min_ms since the batch. */
static int wait_move(DWORD min_ms, DWORD timeout_ms, const char *what) {
  DWORD t0 = io_ms();
  int v, last = -1;
  for (;;) {
    io_sleep(265);
    v = poll_once();
    if (v < 0) {
      printf("poll: %s\n", srb_errstr(v));
      return 1;
    }
    if (v != last)
      printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
             (unsigned long)(io_ms() - t0));
    last = v;
    if (v == 0x0000 && io_ms() - t0 >= min_ms)
      return 0;
    if (io_ms() - t0 > timeout_ms) {
      printf("%s: carriage never went idle\n", what);
      return 1;
    }
  }
}

/* One scan, as FlexColor does it for previews, crops and each 3f frame
 * (sessions 5-6): page burst -> scan descriptor -> hold (FlexColor 0.8-8 s
 * warm, 32 s cold) -> 200 calibration lines -> carriage move -> 50 lines
 * -> move to the start line -> line flood -> end descriptor x2.
 *
 * The 250 calibration lines are reference data for the HOST: FlexColor's
 * CCalibrateLineHandler/CLineHandlerCorrection build the per-pixel
 * correction from them (the device does not correct the CCD). They go to
 * <out>.cal; geometry goes to <out>.txt.
 *
 * y >= 0 moves the window along the strip (only @18 changes, exactly as
 * between FlexColor's 3f frames). keep > 0 stops after that many lines
 * with the end descriptor, as FlexColor's Stop does. */
static int cmd_scan(const struct preset *p, long y, long keep, DWORD hold_ms,
                    const char *out) {
  static BYTE xscan[84], xend[84];
  static struct preset xp;
  static int scanned; /* a scan descriptor went out before */
  BYTE desc[84];
  FILE *f, *fc;
  long got, maxl;
  uint32_t lines, res, px;
  WORD blklen;
  int v, last = -1;
  DWORD start;
  char meta[260];

  if (g_abort) {
    printf("interrupted: not starting another scan\n");
    return 1;
  }
  if (strlen(out) + sizeof ".faul" > sizeof meta) {
    fprintf(stderr, "output name too long: %s\n", out);
    return 1;
  }
  if (g_ccd >= 0 || g_lamp >= 0) {
    /* the end descriptor carries these too (bytes 53+) */
    memcpy(xscan, p->scan, 84);
    if (g_ccd >= 0) {
      write_big_endian32(xscan, F_CCD, g_ccd);
      write_big_endian32(xscan, F_OPTIC, g_optic);
    }
    if (g_lamp >= 0)
      xscan[F_LAMP] = (BYTE)g_lamp;
    memcpy(xend, xscan, 84);
    memset(xend + 8, 0, 53 - 8);
    xp.name = p->name;
    xp.what = p->what;
    xp.scan = xscan;
    xp.end = xend;
    p = &xp;
  }
  memcpy(desc, p->scan, 84);
  res = read_big_endian32(desc, POS_OFF);
  px = read_big_endian32(desc, F_PX);
  lines = read_big_endian32(desc, F_LINES);
  maxl = (long)((double)HOLDER_LINES_525 * res / 5250000.0);
  if (y >= 0) {
    if (y + lines > maxl) {
      fprintf(stderr,
              "start line %ld"
              " + %" PRIu32 " lines is past the holder end "
              "(%ld"
              "lines at %" PRIu32 " ppi)\n",
              y, lines, maxl, res / 10000);
      return 1;
    }
    write_big_endian32(desc, F_Y, y);
  }
  if (keep <= 0 || keep > lines)
    keep = lines;
  if (!scanned && hold_ms < g_first_hold)
    hold_ms = g_first_hold; /* first scan of the process */
  blklen = (WORD)(px * 6);
  printf("%s: %s\n  %" PRIu32 " ppi, %" PRIu32 " px from %" PRIu32 " x %" PRIu32
         " lines from %" PRIu32 ", "
         "%u-byte lines; keeping %ld; CCD/lens %" PRIu32 "/%" PRIu32
         ", lamp %u\n",
         p->name, p->what, res / 10000, px, read_big_endian32(desc, F_X), lines,
         read_big_endian32(desc, F_Y), blklen, keep,
         read_big_endian32(desc, F_CCD), read_big_endian32(desc, F_OPTIC),
         desc[F_LAMP]);

  printf("page burst\n");
  read_pages(0);
  sprintf(meta, "%s.faul", out);
  save_faul(meta);
  printf("scan descriptor\n");
  {
    int r = win_exchange(desc, 0);
    if (r < 0) {
      printf("SetWindow (scan): %s\n", srb_errstr(r));
      return 1;
    }
    scanned = 1;
  }
  start = io_ms();
  if (wait_settled(120000)) {
    printf("device never settled; stopping\n");
    send_end(p);
    return 1;
  }
  printf("holding %lu ms before calibration\n", (unsigned long)hold_ms);
  while (io_ms() - start < hold_ms) {
    v = poll_once();
    if (v < 0) {
      printf("poll: %s\n", srb_errstr(v));
      send_end(p);
      return 1;
    }
    if (v != last)
      printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
             (unsigned long)(io_ms() - start));
    last = v;
    io_sleep(265);
  }

  /* Calibration: count FULL lines — at high resolution many reads come
   * back empty (FlexColor issues ~300 reads for the 200 at 2000 ppi). */
  sprintf(meta, "%s.cal", out);
  fc = g_detail ? NULL : fopen(meta, "wb");
  if (!fc && !g_detail) {
    perror(meta);
    send_end(p);
    return 1;
  }
  printf("calibration: 200 lines\n");
  got = read_flood(200, blklen, fc);
  if (got < 200) {
    printf("calibration got %ld/200 lines; stopping\n", got);
    if (fc)
      fclose(fc);
    send_end(p);
    return 1;
  }
  /* carriage move between the batches (~550 ms); reading during it
   * means the scan never starts */
  if (wait_move(1000, 30000, "between calibration batches")) {
    if (fc)
      fclose(fc);
    send_end(p);
    return 1;
  }
  printf("calibration: 50 lines\n");
  got = read_flood(50, blklen, fc);
  if (fc)
    fclose(fc);
  if (got < 50) {
    printf("calibration got %ld/50 lines; stopping\n", got);
    send_end(p);
    return 1;
  }
  /* detailed pass: the worker starts while the carriage moves */
  if (g_detail && detail_start(out)) {
    send_end(p);
    return 1;
  }

  /* move to the start line (2.5 s preview, ~7.5 s for a crop deep in
   * the holder); the flood starts on busy -> idle */
  printf("waiting for carriage at the start line (up to 90 s)\n");
  {
    int seen_busy = 0;
    start = io_ms();
    last = -1;
    for (;;) {
      v = poll_once();
      if (v < 0)
        break;
      if (v != last)
        printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
               (unsigned long)(io_ms() - start));
      last = v;
      if (v == 0x1060 || (v == 0x0000 && seen_busy))
        break;
      if (v != 0x0000)
        seen_busy = 1;
      if (io_ms() - start > 90000) {
        v = -100;
        break;
      }
      io_sleep(150);
    }
  }
  if (v != 0x1060 && v != 0x0000) {
    printf("carriage never reached the start line (%d); stopping\n", v);
    send_end(p);
    return 1;
  }

  if (g_detail) {
    printf("reading %ld lines -> pass worker\n", keep);
    got = read_flood(keep, blklen, NULL);
    printf("\nread %ld x %u B%s\n", got, blklen,
           got == keep ? "" : " (INCOMPLETE)");
    send_end(p);
    return got == keep ? 0 : 1;
  }
  printf("reading %ld lines -> %s\n", keep, out);
  f = fopen(out, "wb");
  if (!f) {
    perror(out);
    send_end(p);
    return 1;
  }
  g_hooks_on = 1;
  got = read_flood(keep, blklen, f);
  g_hooks_on = 0;
  fclose(f);
  printf("\nwrote %ld x %u B -> %s%s\n", got, blklen, out,
         got == keep ? "" : " (INCOMPLETE)");
  send_end(p);

  sprintf(meta, "%s.txt", out);
  f = fopen(meta, "w");
  if (f) {
    fprintf(f,
            "preset=%s\n"
            "ppi=%" PRIu32 "\n"
            "px=%" PRIu32 "\n"
            "x=%" PRIu32 "\n"
            "lines=%" PRIu32 "\n"
            "y=%" PRIu32 "\n"
            "bin=%u\n"
            "blklen=%u\n"
            "kept=%ld\n"
            "cal_lines=250\n"
            "ccd=%" PRIu32 "\n"
            "optic=%" PRIu32 "\n"
            "lamp=%u\n",
            p->name, res / 10000, px, read_big_endian32(desc, F_X), lines,
            read_big_endian32(desc, F_Y), desc[F_BIN], blklen, got,
            read_big_endian32(desc, F_CCD), read_big_endian32(desc, F_OPTIC),
            desc[F_LAMP]);
    fclose(f);
  }
  return got == keep ? 0 : 1;
}

/* MICROCOD.3XX upload (libft/ft_upload.h): 16 chunks of a 10-byte header
 * + 4096 code bytes, then the 10-byte run trailer, then poll until idle.
 * Refused unless INQUIRY names the loader ("SCSI Loader"), as FlexColor
 * only uploads then: the resident microcode parses the same packets as
 * maintenance commands. Ends on the microcode's handle (await_microcode). */
static int await_microcode(void);

static int upload_microcode(const char *file) {
  static BYTE code[FT_UPLOAD_CHUNK], buf[FT_UPLOAD_CHUNK + 10];
  BYTE cdb[10], inq[96];
  FILE *f;
  ft_err e;
  int i, r, v, len;
  DWORD start;
  long sz;

  memset(inq, 0, sizeof inq);
  r = srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, inq, sizeof inq, TIMEOUT_MS);
  if (r < 0) {
    printf("INQUIRY: %s; not uploading\n", srb_errstr(r));
    return 1;
  }
  if (!ft_upload_is_loader(inq, (size_t)r)) {
    fprintf(stderr,
            "upload: device is not the loader (product \"%.16s\"); "
            "refusing. Power-cycle first.\n",
            inq + 16);
    return 1;
  }
  f = fopen(file, "rb");
  if (!f) {
    perror(file);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz != (long)FT_UPLOAD_CHUNK * FT_UPLOAD_CHUNKS) {
    fprintf(stderr, "%s is %ld bytes, expected 65536 (MICROCOD.3XX)\n", file,
            sz);
    fclose(f);
    return 1;
  }
  {
    /* FlexColor reads the D800 page from the loader right before the
     * chunks (cold-boot traces); do the same */
    static const BYTE pg[10] = {0x3c, 0, 0xd8, 0, 0, 0, 0, 0, 0x42, 0};
    r = srb_exec(pg, 10, SRB_DIR_IN, buf, 0x42, TIMEOUT_MS);
    printf("loader page d800 (0x42): %s\n", r < 0 ? srb_errstr(r) : "ok");
  }
  for (i = 0; i < FT_UPLOAD_CHUNKS; i++) {
    if (fread(code, 1, sizeof code, f) != sizeof code) {
      fclose(f);
      return 1;
    }
    len = ft_upload_chunk(i, code, cdb, buf, sizeof buf, &e);
    if (len < 0) {
      fprintf(stderr, "%s\n", e.msg);
      fclose(f);
      return 1;
    }
    r = srb_exec(cdb, 10, SRB_DIR_OUT, buf, (DWORD)len, 10000);
    printf("chunk %2d (to %02x00:0000): %s\n", i, 0x30 + i,
           r < 0 ? srb_errstr(r) : "ok");
    if (r < 0) {
      /* never send the run trailer after a partial load */
      fclose(f);
      printf("upload aborted; power-cycle and retry\n");
      return 1;
    }
  }
  fclose(f);
  len = ft_upload_trailer(cdb, buf, sizeof buf, &e);
  if (len < 0) {
    fprintf(stderr, "%s\n", e.msg);
    return 1;
  }
  r = srb_exec(cdb, 10, SRB_DIR_OUT, buf, (DWORD)len, 10000);
  printf("trailer: %s\n", r < 0 ? srb_errstr(r) : "ok");
  printf("waiting for firmware commit (poll until idle)\n");
  start = io_ms();
  {
    int last = 0x10000, fails = 0;
    for (;;) {
      v = poll_once();
      if (v == 0x0000)
        break;
      if (v != last) {
        if (v < 0)
          printf("  %5lu ms: poll %s\n", (unsigned long)(io_ms() - start),
                 srb_errstr(v));
        else
          printf("  %5lu ms: %04x %s\n", (unsigned long)(io_ms() - start), v,
                 poll_name((unsigned)v));
        last = v;
      }
      /* the microcode may answer on another \\.\ScannerN than the
       * loader did: after 2 s of failed polls, probe again */
      if (v < 0 && ++fails >= 10) {
        fails = 0;
        if (g_dev != IO_NONE)
          io_close(g_dev);
        g_dev = IO_NONE;
        if (probe_scanner(PROBE_MICROCODE_ONLY) == IO_NONE)
          printf("  no microcode answering yet\n");
        last = 0x10000;
      } else if (v >= 0) {
        fails = 0;
      }
      if (io_ms() - start > 60000) {
        printf("timed out; upload may have failed\n");
        return 1;
      }
      io_sleep(200);
    }
  }
  printf("device idle after %lu ms\n", (unsigned long)(io_ms() - start));
  if (await_microcode())
    return 1;
  printf("upload complete. Don't upload again until a power cycle\n");
  return 0;
}

/* After the commit the microcode, not the loader, must answer INQUIRY;
 * it can come up on another \\.\ScannerN (seen: Scanner0 -> Scanner4),
 * so probe again (INQUIRY only) for up to 30 s. 0: g_dev is the
 * microcode's handle. */
static int await_microcode(void) {
  DWORD start = io_ms();
  int r;
  memset(g_inq, 0, sizeof g_inq);
  r = g_dev == IO_NONE ? -1
                       : srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, g_inq,
                                  sizeof g_inq, TIMEOUT_MS);
  g_inq_n = r < 0 ? 0 : r;
  if (r >= 0 && memcmp(g_inq + 8, "Imacon", 6) == 0 &&
      !ft_upload_is_loader(g_inq, (size_t)r)) {
    printf("microcode: %.16s %.4s\n", g_inq + 16, g_inq + 32);
    return 0;
  }
  printf("the microcode does not answer on this handle yet; probing\n");
  for (;;) {
    if (g_dev != IO_NONE)
      io_close(g_dev);
    g_dev = IO_NONE;
    if (probe_scanner(PROBE_MICROCODE_ONLY) != IO_NONE)
      return 0;
    if (io_ms() - start > 30000)
      break;
    io_sleep(1000);
  }
  fprintf(stderr,
          "ftpoc: the microcode did not answer within 30 s of the upload\n"
          "(only the loader, or nothing). Power the scanner off (>= 10 s) and\n"
          "on, then retry.\n");
  return 1;
}

/* --microcode PATH (NULL: beside ftpoc.exe, then the current directory) */
static const char *g_microcode;

/* The MICROCOD.3XX to upload (libft ft_upload_paths) into path; 0, or 1
 * with the paths tried on stderr. Nothing else is ever tried. */
static int find_microcode(char *path, size_t cap) {
  static char cand[2][FT_UPLOAD_PATH];
  char exe[IO_PATH_MAX + 1];
  int i, k;
  io_exe_path(exe);
  k = ft_upload_paths(g_microcode, exe, cand);
  for (i = 0; i < k; i++) {
    FILE *f = fopen(cand[i], "rb");
    if (f && strlen(cand[i]) < cap) {
      fclose(f);
      strcpy(path, cand[i]);
      return 0;
    }
    if (f)
      fclose(f);
  }
  fprintf(stderr, "ftpoc: no microcode file. Tried:\n");
  for (i = 0; i < k; i++)
    fprintf(stderr, "  %s%s\n", cand[i],
            i == k - 1 && !g_microcode ? " (current directory)" : "");
  if (k == 0)
    fprintf(stderr, "  (--microcode: empty or too long a path)\n");
  fprintf(stderr, "Put %s beside ftpoc.exe or give --microcode PATH.\n",
          FT_UPLOAD_FILE);
  return 1;
}

static int cmd_upload(const char *file) {
  char path[FT_UPLOAD_PATH];
  if (!file) {
    if (find_microcode(path, sizeof path))
      return 1;
    file = path;
  }
  return upload_microcode(file);
}

/* main, before every command but upload and inquiry: the scanner answers
 * as its loader after a power-up; upload the microcode (as FlexColor
 * does at its start) and carry on on the microcode's handle. */
static int auto_upload(void) {
  char path[FT_UPLOAD_PATH];
  fflush(stdout); /* the probe's lines first in a pipe */
  fprintf(stderr,
          "ftpoc: the scanner is in its loader (\"%.16s\", "
          "after a power-up): uploading its microcode first\n",
          g_inq + 16);
  if (find_microcode(path, sizeof path))
    return 1;
  fprintf(stderr, "ftpoc: microcode %s\n", path);
  pg_begin(FT_PG_UPLOAD, 1);
  if (upload_microcode(path)) {
    fprintf(stderr, "ftpoc: the microcode upload failed (above); the "
                    "command did not run\n");
    return 1;
  }
  fprintf(stderr, "ftpoc: microcode running; carrying on\n");
  g_first_hold = FIRST_HOLD_COLD; /* after a power-up: lamp cold */
  return 0;
}

/* ------------------------------------------------------- files and images */

/* Whole file into a malloc'd buffer (NULL on any error; *size set). */
static void *load_file(const char *path, long *size) {
  FILE *f = fopen(path, "rb");
  void *buf = NULL;
  long n;
  *size = 0;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
      fseek(f, 0, SEEK_SET) == 0) {
    buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
      free(buf);
      buf = NULL;
    }
    if (buf)
      *size = n;
  }
  fclose(f);
  return buf;
}

/* key=value from a scan's .txt; -1 if absent */
static long meta_long(const char *scan, const char *key) {
  char path[300], line[128];
  size_t n = strlen(key);
  long v = -1;
  FILE *f;
  if (snprintf(path, sizeof path, "%s.txt", scan) >= (int)sizeof path)
    return -1;
  f = fopen(path, "r");
  if (!f)
    return -1;
  while (fgets(line, sizeof line, f))
    if (strncmp(line, key, n) == 0 && line[n] == '=')
      v = atol(line + n + 1);
  fclose(f);
  return v;
}

/* Load <scan>, <scan>.cal (and .faul when x >= 0: x = the scan's first
 * output pixel) as a flat-fielded, aligned image: returns lines*px*3
 * floats (caller frees), *lines_out set; NULL on failure. */
static float *load_corrected(const char *scan, int px, int bin, int x,
                             int *lines_out) {
  char path[300];
  long nraw = 0, ncal = 0, nfaul = 0, lines, cl;
  unsigned short *raw = NULL, *cal = NULL;
  float *dark = NULL, *white = NULL, *t = NULL;
  ft_err e;

  if (px < 1 || px > 20000 || bin < 1 || bin > 12) {
    fprintf(stderr, "%s: bad geometry (px %d, bin %d)\n", scan, px, bin);
    return NULL;
  }
  if (snprintf(path, sizeof path, "%s.cal", scan) >= (int)sizeof path) {
    fprintf(stderr, "%s: name too long\n", scan);
    return NULL;
  }
  raw = load_file(scan, &nraw);
  cal = load_file(path, &ncal);
  dark = malloc((size_t)px * 3 * sizeof *dark);
  white = malloc((size_t)px * 3 * sizeof *white);
  lines = nraw / ((long)px * 6);
  cl = ncal / ((long)px * 6);
  if (!raw || !cal || !dark || !white || lines < 1 || cl < 1 ||
      lines > 1000000) {
    fprintf(stderr, "%s: cannot load scan data\n", scan);
    goto out;
  }
  if (ft_split_cal(cal, (int)cl, px, dark, white, &e)) {
    fprintf(stderr, "%s: %s\n", scan, e.msg);
    goto out;
  }
  t = malloc((size_t)lines * px * 3 * sizeof *t);
  if (!t) {
    fprintf(stderr, "%s: out of memory\n", scan);
    goto out;
  }
  ft_flatfield(raw, (int)lines, px, dark, white, t);
  if (x >= 0) {
    char *faul;
    snprintf(path, sizeof path, "%s.faul", scan);
    faul = load_file(path, &nfaul);
    if (faul) {
      int pixs[3][FT_FAUL_MAX], cnt[3];
      ft_faul_parse(faul, (size_t)nfaul, pixs, cnt);
      free(faul);
      if (ft_faul_fix(t, (int)lines, px, pixs, cnt, x, bin)) {
        fprintf(stderr, "%s: defective pixel fix failed\n", scan);
        free(t);
        t = NULL;
        goto out;
      }
    } else {
      printf("note: no %s; defective pixels left as they are\n", path);
    }
  }
  *lines_out = ft_align_rgb(t, (int)lines, px, 12 / bin);
out:
  free(raw);
  free(cal);
  free(dark);
  free(white);
  return t;
}

#define WHITE_CLIP 15500.0f /* raw white this high: R clips (~15860) */

/* Load a 1000/1500 ppi preview as its detection image: flat-fielded and
 * defect-fixed at full resolution in chunks of lines, box-averaged by f,
 * then RGB-aligned (k / f lines). The full-size image is never held: a
 * 1500 ppi preview is ~13700 x 1900 px, too big as floats for a 32-bit
 * process. Returns (lines/f) x (px/f) x 3 floats, NULL on failure. */
static float *load_preview_ds(const char *scan, int px, int bin, int x, int f,
                              int *lines_out, int *px_out) {
  char path[300], *faul;
  long ncal = 0, nfaul = 0, total, cl, done, n, lo, po, i;
  unsigned short *cal = NULL, *buf = NULL;
  float *dark = NULL, *white = NULL, *tf = NULL, *t = NULL;
  int pixs[3][FT_FAUL_MAX], cnt[3] = {0, 0, 0}, have_faul = 0, ok = 0;
  long chunk = 64L * f;
  FILE *fr = NULL;
  ft_err e;

  if (px < 1 || px > 20000 || bin < 1 || bin > 12 || f < 1 || (12 / bin) % f) {
    fprintf(stderr, "%s: bad geometry (px %d, bin %d, f %d)\n", scan, px, bin,
            f);
    return NULL;
  }
  if (snprintf(path, sizeof path, "%s.cal", scan) >= (int)sizeof path) {
    fprintf(stderr, "%s: name too long\n", scan);
    return NULL;
  }
  cal = load_file(path, &ncal);
  dark = malloc((size_t)px * 3 * sizeof *dark);
  white = malloc((size_t)px * 3 * sizeof *white);
  cl = ncal / ((long)px * 6);
  if (!cal || !dark || !white || cl < 1) {
    fprintf(stderr, "%s: cannot load the calibration lines\n", scan);
    goto out;
  }
  if (ft_split_cal(cal, (int)cl, px, dark, white, &e)) {
    fprintf(stderr, "%s: %s\n", scan, e.msg);
    goto out;
  }
  for (i = 0; i < (long)px * 3; i++)
    if (white[i] >= WHITE_CLIP) {
      printf("WARNING: white reference at %.0f counts (clips at "
             "~15860): open areas may clip; dim the lamp (-l)\n",
             white[i]);
      break;
    }
  snprintf(path, sizeof path, "%s.faul", scan);
  faul = load_file(path, &nfaul);
  if (faul) {
    ft_faul_parse(faul, (size_t)nfaul, pixs, cnt);
    free(faul);
    have_faul = 1;
  } else {
    printf("note: no %s; defective pixels left as they are\n", path);
  }
  fr = fopen(scan, "rb");
  if (!fr || fseek(fr, 0, SEEK_END) != 0 || (total = ftell(fr)) < 0 ||
      fseek(fr, 0, SEEK_SET) != 0) {
    fprintf(stderr, "%s: cannot read the scan\n", scan);
    goto out;
  }
  total /= (long)px * 6;
  lo = total / f;
  po = px / f;
  if (lo < 1 || po < 1 || total > 1000000) {
    fprintf(stderr, "%s: cannot load scan data\n", scan);
    goto out;
  }
  t = malloc((size_t)lo * po * 3 * sizeof *t);
  buf = malloc((size_t)chunk * px * 3 * sizeof *buf);
  tf = malloc((size_t)chunk * px * 3 * sizeof *tf);
  if (!t || !buf || !tf) {
    fprintf(stderr, "%s: out of memory\n", scan);
    goto out;
  }
  for (done = 0; done < lo * f; done += n) {
    n = lo * f - done < chunk ? lo * f - done : chunk; /* multiple of f */
    if (fread(buf, (size_t)px * 6, (size_t)n, fr) != (size_t)n) {
      fprintf(stderr, "%s: short read\n", scan);
      goto out;
    }
    ft_flatfield(buf, (int)n, px, dark, white, tf);
    if (have_faul && ft_faul_fix(tf, (int)n, px, pixs, cnt, x, bin)) {
      fprintf(stderr, "%s: defective pixel fix failed\n", scan);
      goto out;
    }
    ft_box_down(tf, (int)n, px, f, t + (size_t)(done / f) * po * 3);
  }
  *lines_out = ft_align_rgb(t, (int)lo, (int)po, 12 / bin / f);
  *px_out = (int)po;
  ok = 1;
out:
  if (fr)
    fclose(fr);
  free(cal);
  free(dark);
  free(white);
  free(buf);
  free(tf);
  if (!ok) {
    free(t);
    t = NULL;
  }
  return t;
}

/* sharpness of the green channel of a lines x px RGB image; NaN when out
 * of memory (ft_peak refuses non-finite values) */
static double green_sharpness(const float *t, int lines, int px) {
  size_t i, n = (size_t)lines * px;
  float *gch = malloc((n ? n : 1) * sizeof *gch);
  double s;
  if (!gch)
    return NAN;
  for (i = 0; i < n; i++)
    gch[i] = t[i * 3 + 1];
  s = ft_sharpness(gch, px, lines);
  free(gch);
  return s;
}

/* ------------------------------------------------------------------- driver */

/* Synthesized crop at any resolution: the 2000 ppi crop descriptor with
 * only the decoded fields replaced (session 6):
 *   @10 ppi x 10000, @14/@18 origin and @22/@26 size in output pixels at
 *   that ppi (square pixels; origin fixed on the holder: both captured
 *   crops centre the frame at 16.8 mm across), @51 bin = the largest divisor
 *   of 12 with ppi x bin <= 6300 (preview 12, 1000 -> 6, 2000 -> 3,
 *   1200 -> 4), @55 = 12 / bin.
 * CCD/lens @56/@60: the in-focus pair for the optical scale ppi x bin
 * (ft_optics_for on the focus curve, see load_lens), or -p.
 * mm in, clamped to the area the captured scans covered. */
static BYTE CROP_SCAN[84], CROP_END[84];

/* Focus: thin lens on one rail (libft/ft_focus.h), fitted to the sharpest
 * point of eight CCD focus brackets at 3807..6230 px/inch (2026-10-12).
 * FlexColor's own crop/preview pairs sit 9000-10000 CCD steps short of
 * that peak on this scanner, and are visibly softer, so crop does not use
 * them. The curve: focus.txt in the current directory if present (written
 * by calibrate-focus), else this scanner's fit compiled into libft. */
static ft_lens g_lens;

static void load_lens(void) {
  FILE *f = fopen("focus.txt", "r");
  ft_err e;
  g_lens = ft_lens_default;
  if (!f)
    return;
  if (ft_lens_read(&g_lens, f, &e)) {
    fprintf(stderr, "focus.txt: %s; using the built-in curve\n", e.msg);
    g_lens = ft_lens_default;
  } else {
    printf("focus.txt: film=%.0f w=%.0f fl=%.0f p=%.4e\n", g_lens.film,
           g_lens.w, g_lens.fl, g_lens.p);
  }
  fclose(f);
}

/* One synthesized crop. ccd/optic >= 0: use them (after the safety check);
 * otherwise the focus curve for ppi x bin with focus_offset added to the
 * CCD and the lens (tandem), or to the CCD alone with ccd_only. Returns 0 ok,
 * CROP_REFUSED when a check refused the request before anything was sent, 1
 * when the scan failed. */
#define CROP_REFUSED 2
#define CROP_X_MAX 32.0 /* mm across: the holder's crop limit */
struct crop_req {
  double ppi, x, y, w, h; /* mm */
  int bin;                /* 0: the fastest (ft_bin_for_ppi) */
  long ccd, optic, focus_offset;
  int ccd_only; /* offset on the CCD alone (calibration) */
  long keep;
  DWORD hold;
  const char *out;
};

static long crop_bin(const struct crop_req *r) {
  double s;
  if (!r->bin)
    return ft_bin_for_ppi(r->ppi);
  if (ft_bin_ok(r->ppi, r->bin))
    return r->bin;
  /* explicit positions (-p): any optics the scanner reaches, down to
   * FT_SCALE_REACH_MIN (the curve stops at FT_SCALE_MIN) */
  s = r->ppi * r->bin;
  if (r->ccd >= 0 && r->bin >= 1 && 12 % r->bin == 0 &&
      s >= FT_SCALE_REACH_MIN - 1e-6 && s <= FT_SCALE_MAX + 1e-6)
    return r->bin;
  return -1;
}

/* Every check crop_scan makes before it sends anything: 0 with the
 * CCD/lens pair it would use in *ccd / *optic, or CROP_REFUSED (reason
 * printed). */
static int crop_check(const struct crop_req *r, long *ccd_out,
                      long *optic_out) {
  long bin, ccd = r->ccd, optic = r->optic;
  double scale, cx;

  /* every range check is written so that NaN fails it */
  if (!isfinite(r->ppi) || !isfinite(r->x) || !isfinite(r->y) ||
      !isfinite(r->w) || !isfinite(r->h)) {
    fprintf(stderr, "crop: non-finite window\n");
    return CROP_REFUSED;
  }
  if (!(r->ppi >= 525 && r->ppi <= 6300)) {
    fprintf(stderr, "crop: ppi 525..6300\n");
    return CROP_REFUSED;
  }
  /* the preview covers 0.92..33.14 mm across and ~232 mm along; crops
   * stay within the holder's crop limits, 32 x 231 mm */
  if (!(r->x >= 0 && r->w > 0 && r->x + r->w <= CROP_X_MAX && r->y >= 0 &&
        r->h > 0 && r->y + r->h <= 231.0)) {
    fprintf(stderr, "crop: window outside the holder area FlexColor "
                    "scanned (x+w <= 32 mm, y+h <= 231 mm)\n");
    return CROP_REFUSED;
  }
  /* bin divides 12 (byte 55 = 12 / bin in every capture); by default
   * the largest that keeps the optical scale ppi x bin within 6300 (the
   * fastest); a smaller one is a lower magnification (ft_plan.h) */
  bin = crop_bin(r);
  if (bin < 0) {
    fprintf(stderr,
            "crop: bin %d not usable at %g ppi (the optics ppi x "
            "bin must be %.0f..%.0f)\n",
            r->bin, r->ppi, FT_SCALE_MIN, FT_SCALE_MAX);
    return CROP_REFUSED;
  }
  scale = r->ppi * bin;
  if ((long)(r->w / 25.4 * r->ppi + 0.5) * 6 > 60000) {
    fprintf(stderr, "crop: line too long for one ReadExt\n");
    return CROP_REFUSED;
  }
  if (ccd < 0) {
    if (ft_optics_for(&g_lens, scale, &ccd, &optic)) {
      fprintf(stderr,
              "crop: %g ppi x bin %ld = optical scale %.0f; "
              "focus is known for %.0f..%.0f (or give -p)\n",
              r->ppi, bin, scale, FT_SCALE_MIN, FT_SCALE_MAX);
      return CROP_REFUSED;
    }
    ccd += r->focus_offset;
    if (!r->ccd_only)
      optic += r->focus_offset;
  }
  if (!pos_ok(ccd, optic))
    return CROP_REFUSED;
  /* mm across are measured at the 6000 scale; other scales magnify
   * about the optical axis */
  cx = ft_mm_to_ccd(r->x, scale);
  if (!(cx >= FT_CCD_FIRST &&
        cx + r->w / 25.4 * scale <= FT_CCD_FIRST + FT_CCD_PIXELS)) {
    fprintf(stderr, "crop: window runs off the CCD at this scale\n");
    return CROP_REFUSED;
  }
  *ccd_out = ccd;
  *optic_out = optic;
  return 0;
}

/* One scan from the captured 2000 ppi crop descriptor (SCAN_2000) with
 * only the decoded fields replaced: @10 ppi, @14/@18 origin and @22/@26
 * size in output pixels/lines, @51 bin, @55 = 12 / bin and the CCD/lens
 * pair. Every synthesized scan (crops, 1000/1500 ppi previews) is built
 * here, so all of them follow the same proven construction. */
static int synth_scan(const char *name, const char *what, double ppi, long bin,
                      long x_out, long y_line, long px, long lines, long ccd,
                      long optic, long keep, DWORD hold, const char *out) {
  static struct preset sp;
  int i, rc;

  memcpy(CROP_SCAN, SCAN_2000, 84);
  write_big_endian32(CROP_SCAN, POS_OFF, (long)(ppi * 10000 + 0.5));
  write_big_endian32(CROP_SCAN, F_X, x_out);
  write_big_endian32(CROP_SCAN, F_Y, y_line);
  write_big_endian32(CROP_SCAN, F_PX, px);
  write_big_endian32(CROP_SCAN, F_LINES, lines);
  write_big_endian32(CROP_SCAN, F_CCD, ccd);
  write_big_endian32(CROP_SCAN, F_OPTIC, optic);
  CROP_SCAN[F_BIN] = (BYTE)bin;
  CROP_SCAN[55] = (BYTE)(12 / bin);
  memcpy(CROP_END, CROP_SCAN, 84);
  memset(CROP_END + 8, 0, 53 - 8);
  printf("%s descriptor:", name);
  for (i = 0; i < 84; i++)
    printf("%s%02x", i % 28 ? " " : "\n  ", CROP_SCAN[i]);
  printf("\n");
  sp.name = name;
  sp.what = what;
  sp.scan = CROP_SCAN;
  sp.end = CROP_END;
  g_ccd = ccd; /* cmd_scan applies these to scan + end */
  g_optic = optic;
  rc = cmd_scan(&sp, -1, keep, hold, out);
  g_ccd = g_optic = -1;
  return rc ? 1 : 0;
}

static int crop_scan(const struct crop_req *r) {
  long bin, ccd, optic;
  double scale, cx;

  if (crop_check(r, &ccd, &optic))
    return CROP_REFUSED;
  bin = crop_bin(r); /* crop_check: valid */
  scale = r->ppi * bin;
  cx = ft_mm_to_ccd(r->x, scale);
  return synth_scan(
      "crop", "synthesized crop", r->ppi, bin,
      (long)((cx - FT_CCD_FIRST) / bin + 0.5),
      (long)(r->y / 25.4 * r->ppi + 0.5), (long)(r->w / 25.4 * r->ppi + 0.5),
      (long)(r->h / 25.4 * r->ppi + 0.5), ccd, optic, r->keep, r->hold, r->out);
}

/* A whole-holder preview (libft/ft_preview.h). standard: FlexColor's
 * captured 525 ppi descriptor with only the CCD/lens moved to the focus
 * curve at 6300 (FlexColor's own pair is ~10000 CCD steps short of the
 * peak and soft). o3150/1000/1500: synthesized over the same window at
 * their optics (bin 6 / 6 / 4), on the focus curve: the fastest way to
 * those resolutions. */
static int preview_scan(const ft_preview *m, long keep, DWORD hold,
                        const char *out) {
  static char what[80];
  double x0, w, len, scale = m->ppi * m->bin;
  long ccd, optic, x_out, px;

  if (m->bin == 12) {
    static BYTE pv[84];
    static struct preset pp;
    int rc;
    if (ft_preview_std_desc(SCAN_WIN, &g_lens, pv, &ccd, &optic)) {
      fprintf(stderr, "preview %s: no focus for optical scale %.0f\n", m->name,
              scale);
      return CROP_REFUSED;
    }
    if (!pos_ok(ccd, optic))
      return CROP_REFUSED;
    pp = PRESETS[0];
    pp.scan = pv;
    g_ccd = ccd; /* cmd_scan applies these to scan + end */
    g_optic = optic;
    rc = cmd_scan(&pp, -1, keep, hold, out);
    g_ccd = g_optic = -1;
    return rc ? 1 : 0;
  }
  if (ft_optics_for(&g_lens, scale, &ccd, &optic)) {
    fprintf(stderr, "preview %s: no focus for optical scale %.0f\n", m->name,
            scale);
    return CROP_REFUSED;
  }
  if (!pos_ok(ccd, optic))
    return CROP_REFUSED;
  ft_preview_window(&x0, &w, &len);
  x_out = (long)((ft_mm_to_ccd(x0, scale) - FT_CCD_FIRST) / m->bin + 0.5);
  px = (long)(w / 25.4 * m->ppi + 0.5);
  if (x_out < 0 || (x_out + px) * m->bin > FT_CCD_PIXELS || px * 6 > 60000) {
    fprintf(stderr, "preview %s: window runs off the CCD\n", m->name);
    return CROP_REFUSED;
  }
  snprintf(what, sizeof what,
           "%g ppi preview, whole holder (optics %.0f, "
           "bin %d)",
           m->ppi, scale, m->bin);
  return synth_scan(m->name, what, m->ppi, m->bin, x_out, 0, px,
                    ft_preview_lines(m), ccd, optic, keep, hold, out);
}

static int cmd_crop(int argc, char **argv) {
  struct crop_req r;
  int i;

  if (argc < 5) {
    fprintf(stderr, "crop <ppi> <x_mm> <y_mm> <w_mm> <h_mm> "
                    "[-b bin] [-n lines] [-h s] [-p ccd,lens] [-l lamp] "
                    "[-o file]\n");
    return 2;
  }
  memset(&r, 0, sizeof r);
  r.ppi = atof(argv[0]);
  r.x = atof(argv[1]);
  r.y = atof(argv[2]);
  r.w = atof(argv[3]);
  r.h = atof(argv[4]);
  r.ccd = r.optic = -1;
  r.hold = 10000;
  r.out = "crop.raw";
  for (i = 5; i + 1 < argc; i += 2) {
    if (strcmp(argv[i], "-n") == 0)
      r.keep = atol(argv[i + 1]);
    else if (strcmp(argv[i], "-b") == 0)
      r.bin = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "-h") == 0) {
      if (parse_hold(argv[i + 1], &r.hold))
        return 2;
    } else if (strcmp(argv[i], "-o") == 0)
      r.out = argv[i + 1];
    else if (strcmp(argv[i], "-p") == 0) {
      if (parse_pos(argv[i + 1]))
        return 2;
      r.ccd = g_ccd;
      r.optic = g_optic;
      g_ccd = g_optic = -1;
    } else if (strcmp(argv[i], "-l") == 0) {
      if (parse_lamp(argv[i + 1]))
        return 2;
    } else {
      fprintf(stderr, "crop: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  return crop_scan(&r) ? 1 : 0;
}

/* ------------------------------------------------------------ focus, frames */

/* Focus bracket on a 1 mm band centred at (xc, yc), w mm across, at ppi:
 * n (3..9) crops at curve + centre + (i - n/2) * step on the CCD and the
 * lens together, or on the CCD alone (lens on the curve) with ccd_only. Samples
 * are kept by offset; on a peak at an end the bracket moves there (up to
 * twice), scanning only offsets not yet sampled, at most max_samples
 * (n..FB_SAMPLES) in all, and the peak is fitted over every sample. The bracket
 * is clamped to +-FT_BRACKET_MAX of the curve (FT_CAL_BRACKET_MAX with
 * ccd_only). Returns 0 with *best = peak offset from the curve; FB_NO_PEAK when
 * the point is unusable (crop refused before scanning, flat response, peak
 * still at an edge); -1 on a scan or file failure. */
#define FB_SAMPLES 17      /* array size; callers pass their own cap */
#define FB_FRAME_SAMPLES 9 /* frames and scan --job per-frame focus */
#define FB_NO_PEAK 2
#define FB_MIN_CONTRAST 0.03 /* (max - min) / max below this: flat */

/* the bin focus crops use; 0: the fastest for the ppi */
static int g_focus_bin;

static int focus_bracket(double ppi, double xc, double yc, double w,
                         long centre, long step, int n, int max_samples,
                         int ccd_only, long *best) {
  /* a wide bracket (low magnification: share_focus) may reach as far
   * as it spans; every crop still goes through crop_check */
  const long lim = ccd_only ? FT_CAL_BRACKET_MAX
                   : (long)(n - 1) * step > FT_BRACKET_MAX
                       ? (long)(n - 1) * step
                       : FT_BRACKET_MAX;
  long off[FB_SAMPLES];
  double sh[FB_SAMPLES];
  int ns = 0, tries, i, k, m, dir = 0;
  int bin = g_focus_bin ? g_focus_bin : ft_bin_for_ppi(ppi);
  long half, refused_lo = LONG_MAX, refused_hi = LONG_MIN;

  if (n < 3 || n > max_samples || max_samples > FB_SAMPLES || step <= 0 ||
      bin < 1 || (long)(n - 1) * step > 2 * lim) {
    fprintf(stderr, "focus: bad bracket (n %d, step %ld, ppi %g)\n", n, step,
            ppi);
    return FB_NO_PEAK;
  }
  half = (long)(n / 2) * step;
  for (tries = 0; tries < 3; tries++) {
    long lo, mn_i;
    double mx, mn, pk;
    int r;

    /* keep the whole bracket inside +-lim */
    lo = centre - half;
    if (lo < -lim)
      lo = -lim;
    if (lo + (long)(n - 1) * step > lim)
      lo = lim - (long)(n - 1) * step;
    for (i = 0; i < n && ns < max_samples; i++) {
      struct crop_req q;
      int lines = 0, px, rc;
      long o = lo + i * step, x;
      float *t;
      double s;
      for (k = 0; k < ns && off[k] != o; k++)
        ;
      if (k < ns)
        continue;          /* sampled on an earlier bracket */
      if (labs(o) > lim) { /* cannot happen; belt and braces */
        fprintf(stderr, "focus: offset %ld beyond +-%ld\n", o, lim);
        return FB_NO_PEAK;
      }
      memset(&q, 0, sizeof q);
      q.ppi = ppi;
      q.bin = g_focus_bin;
      q.x = xc - w / 2;
      q.y = yc - 0.5;
      q.w = w;
      q.h = 1.0;
      q.ccd = q.optic = -1;
      q.focus_offset = o;
      q.ccd_only = ccd_only;
      q.hold = 3000;
      q.out = "fb.raw";
      pg_focus(++g_pg_crops, o);
      rc = crop_scan(&q);
      if (rc == CROP_REFUSED) {
        /* outside the safe CCD/lens range: unreachable, not fatal
         * (at 3150 ppi the focus sits ~1700 steps above the CCD
         * floor, hardware 2026-10-12) */
        printf("  focus %+6ld: outside the safe range; skipped\n", o);
        if (o < refused_lo)
          refused_lo = o;
        if (o > refused_hi)
          refused_hi = o;
        continue;
      }
      if (rc)
        return -1;
      px = (int)meta_long("fb.raw", "px");
      x = meta_long("fb.raw", "x");
      t = load_corrected("fb.raw", px, bin, x >= 0 ? (int)x : -1, &lines);
      if (!t)
        return -1;
      s = green_sharpness(t, lines, px);
      free(t);
      printf("  focus %+6ld: sharpness %.4f\n", o, s);
      if (isfinite(s))
        pg_sharp(o, s);
      /* insert sorted by offset */
      for (k = ns; k > 0 && off[k - 1] > o; k--) {
        off[k] = off[k - 1];
        sh[k] = sh[k - 1];
      }
      off[k] = o;
      sh[k] = s;
      ns++;
    }

    m = 0;
    mn_i = 0;
    for (k = 1; k < ns; k++) {
      if (sh[k] > sh[m])
        m = k;
      if (sh[k] < sh[mn_i])
        mn_i = k;
    }
    mx = sh[m];
    mn = sh[mn_i];
    if (ns < 3 || !isfinite(mx) || !isfinite(mn) || !(mx > 0) ||
        (mx - mn) / mx < FB_MIN_CONTRAST) {
      printf("  focus: flat response (%.4f..%.4f); no usable peak\n", mn, mx);
      return FB_NO_PEAK;
    }
    r = ft_peak(off, sh, ns, &pk);
    if (r < 0) {
      printf("  focus: no usable sharpness peak\n");
      return FB_NO_PEAK;
    }
    *best = (long)(pk < 0 ? pk - 0.5 : pk + 0.5);
    if (r == 0)
      return 0;
    /* maximum against the safe limit: nothing sharper is reachable */
    if ((m == 0 && refused_lo < off[0]) ||
        (m == ns - 1 && refused_hi > off[ns - 1])) {
      printf("  focus: sharpest at the safe limit (%+ld)\n", off[m]);
      *best = off[m];
      return 0;
    }
    /* maximum at an end of everything sampled */
    if (dir != 0 && (m == 0 ? -1 : 1) != dir) {
      printf("  focus: maximum went back to the other end; no usable "
             "peak\n");
      return FB_NO_PEAK;
    }
    dir = m == 0 ? -1 : 1;
    if (tries < 2)
      printf("  focus: still rising at %+ld; moving the bracket\n", off[m]);
    centre = off[m];
  }
  printf("  focus: peak still at the bracket edge (%+ld); no usable peak\n",
         *best);
  return FB_NO_PEAK;
}

/* 3x5 digits, rows top to bottom, 3 bits each (MSB = left) */
static const unsigned char DIGITS[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

static void put_px(unsigned char *img, int w, int h, long x, long y,
                   unsigned char r, unsigned char g, unsigned char b) {
  size_t o;
  if (x < 0 || y < 0 || x >= w || y >= h)
    return;
  o = ((size_t)y * w + (size_t)x) * 3;
  img[o] = r;
  img[o + 1] = g;
  img[o + 2] = b;
}

static void draw_number(unsigned char *img, int w, int h, long x, long y,
                        int v) {
  char s[16];
  int i, r, c, k, scale = 4;
  snprintf(s, sizeof s, "%d", v);
  for (i = 0; s[i]; i++) {
    if (s[i] < '0' || s[i] > '9')
      continue;
    for (r = 0; r < 5; r++)
      for (c = 0; c < 3; c++)
        if (DIGITS[s[i] - '0'][r] & (4 >> c))
          for (k = 0; k < scale * scale; k++)
            put_px(img, w, h, x + (i * 4 + c) * scale + k % scale,
                   y + r * scale + k / scale, 255, 255, 0);
  }
}

/* mm -> pixel index, clamped to -1..n+1 (NaN -> -1) so a bad window
 * cannot run away; callers treat <0 and >n as outside */
static long mm_to_idx(double mm, double mm0, double per, int n) {
  double v = floor((mm - mm0) / per + 0.5);
  if (!(v >= 0))
    return -1;
  if (v > n)
    return (long)n + 1;
  return (long)v;
}

/* strip horizontal: image x = preview line, image y = preview column.
 * Included frames yellow (dimmer when excluded), low confidence orange. */
static int write_strip_png(const char *path, const float *t, int lines, int px,
                           const ft_axes *ax, const ft_job *j) {
  unsigned char *img;
  long l, c;
  int i, k, rc;
  ft_err e;
  if (lines < 1 || px < 1)
    return -1;
  img = malloc((size_t)lines * px * 3);
  if (!img) {
    fprintf(stderr, "%s: out of memory\n", path);
    return -1;
  }
  for (l = 0; l < lines; l++)
    for (c = 0; c < px; c++)
      for (k = 0; k < 3; k++) {
        double v = t[((size_t)l * px + c) * 3 + k];
        v = v > 0 ? pow(v < 1 ? v : 1, 1 / 2.2) : 0;
        img[((size_t)c * lines + l) * 3 + k] = (unsigned char)(v * 255 + 0.5);
      }
  for (i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    long x0 = mm_to_idx(f->y0, ax->y_mm0, ax->y_mm_per_line, lines);
    long x1 = mm_to_idx(f->y0 + f->len, ax->y_mm0, ax->y_mm_per_line, lines);
    long y0 = mm_to_idx(f->x0, ax->x_mm0, ax->x_mm_per_px, px);
    long y1 = mm_to_idx(f->x0 + f->width, ax->x_mm0, ax->x_mm_per_px, px);
    unsigned char r = f->include ? 255 : 128, g = f->confidence_low ? 64 : 255;
    for (l = x0; l <= x1; l++) {
      put_px(img, lines, px, l, y0, r, g, 0);
      put_px(img, lines, px, l, y1, r, g, 0);
    }
    for (c = y0; c <= y1; c++) {
      put_px(img, lines, px, x0, c, r, g, 0);
      put_px(img, lines, px, x1, c, r, g, 0);
    }
    draw_number(img, lines, px, x0 + 6, y0 + 6, f->index);
  }
  rc = ft_png_write_rgb8(path, img, lines, px, &e);
  if (rc)
    fprintf(stderr, "%s\n", e.msg);
  free(img);
  return rc;
}

/* Per included frame: every usable bin at its ppi with the scan time,
 * light per sample and per-read Dmax estimate (ft_plan.h); totals for
 * the fastest and the most-light choice. The job's bin key picks one. */
static void print_options(const ft_job *j) {
  double t_fast = 0, t_light = 0;
  int i, k, n, b[6], any = 0;
  printf("\n optics per frame: bin = time, light per sample, Dmax~ "
         "(job key bin; 0 = fastest)\n");
  for (i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    if (!f->include)
      continue;
    n = ft_bins_for_ppi(f->ppi, b);
    if (n < 1)
      continue;
    any = 1;
    printf(" %3d %5g ppi ", f->index, f->ppi);
    for (k = 0; k < n; k++) {
      double l = ft_light_rel(&g_lens, f->ppi * b[k]);
      printf(" %s%d: %3.0f s %.1fx %.2f", k ? "| " : "", b[k],
             ft_scan_seconds(f->ppi, b[k], f->len), l, ft_dmax_est(l));
    }
    printf("\n");
    t_fast += ft_scan_seconds(f->ppi, b[0], f->len);
    t_light += ft_scan_seconds(f->ppi, b[n - 1], f->len);
  }
  if (any)
    printf(" all included frames: ~%.1f min fastest, ~%.1f min most "
           "light (plus focus crops)\n",
           t_fast / 60, t_light / 60);
}

static const char *type_name(ft_frame_type t) {
  static const char *const names[] = {"full", "half", "panorama", "partial"};
  return (unsigned)t < sizeof names / sizeof names[0] ? names[t] : "?";
}

static void print_job(const ft_job *j) {
  int i;
  printf("\n frame  type      y0     len    x0   width  tilt  ppi   conf\n");
  for (i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    printf(" %3d%s  %-8s %6.1f %6.1f %5.1f %6.1f %5.2f %5g  %s\n", f->index,
           f->include ? " " : "-", type_name(f->type), f->y0, f->len, f->x0,
           f->width, f->tilt_deg, f->ppi, f->confidence_low ? "LOW" : "ok");
  }
  printf(" focus: %s, offset %ld CCD steps\n",
         j->focus == FT_FOCUS_PER_FRAME ? "per frame" : "shifted curve",
         j->focus_offset);
}

/* most textured 1 mm band of a frame, by preview sharpness: centre (mm
 * along) returned, *score = its sharpness (0 if no band fits) */
static double textured_band(const float *t, int lines, int px,
                            const ft_axes *ax, const ft_job_frame *f,
                            double *score) {
  double best_y = f->y0 + f->len / 2;
  int k;
  *score = 0;
  for (k = 1; k <= 9; k++) {
    double yc = f->y0 + 2 + (f->len - 4) * k / 10.0;
    long l0 = mm_to_idx(yc - 0.5, ax->y_mm0, ax->y_mm_per_line, lines);
    long l1 = mm_to_idx(yc + 0.5, ax->y_mm0, ax->y_mm_per_line, lines);
    long c0 = mm_to_idx(f->x0, ax->x_mm0, ax->x_mm_per_px, px);
    long c1 = mm_to_idx(f->x0 + f->width, ax->x_mm0, ax->x_mm_per_px, px);
    double s;
    float *band;
    long l, c;
    if (c0 < 0) /* across: the part inside the preview */
      c0 = 0;
    if (c1 > px)
      c1 = px;
    if (l0 < 0 || l1 > lines || l1 - l0 < 3 || c1 - c0 < 3)
      continue;
    band = malloc((size_t)(l1 - l0) * (c1 - c0) * sizeof *band);
    if (!band)
      continue;
    for (l = l0; l < l1; l++)
      for (c = c0; c < c1; c++)
        band[(size_t)(l - l0) * (c1 - c0) + (c - c0)] =
            t[((size_t)l * px + c) * 3 + 1];
    s = ft_sharpness(band, (int)(c1 - c0), (int)(l1 - l0));
    free(band);
    if (s > *score) {
      *score = s;
      best_y = yc;
    }
  }
  return best_y;
}

#define TEXTURE_MIN 0.01 /* preview sharpness below this: featureless */

/* Across reach (mm) of a crop at ppi: crop_scan's holder limit
 * (0..CROP_X_MAX) intersected with the CCD at the optical scale ppi x bin
 * (crop_scan refuses a window that runs off it). -1 for an unusable ppi. */
static int crop_reach(double ppi, double *lo, double *hi) {
  int bin;
  double scale;
  if (!(ppi >= 525 && ppi <= 6300) || (bin = ft_bin_for_ppi(ppi)) < 1)
    return -1;
  scale = ppi * bin;
  *lo = ft_ccd_to_mm(FT_CCD_FIRST, scale);
  *hi = ft_ccd_to_mm(FT_CCD_FIRST + FT_CCD_PIXELS, scale);
  if (!(*lo > 0))
    *lo = 0;
  if (!(*hi < CROP_X_MAX))
    *hi = CROP_X_MAX;
  return 0;
}

#define FOCUS_X_MARGIN 0.3 /* mm kept clear of the reach on each side */
#define FOCUS_W_MIN 2.0    /* narrower band: not worth a bracket */

/* The part of [x0, x1] mm across a focus crop at ppi can use: the crop
 * reach less FOCUS_X_MARGIN each side. 0 with *lo and *hi set,
 * FB_NO_PEAK when less than FOCUS_W_MIN remains. */
static int focus_span(double ppi, double x0, double x1, double *lo,
                      double *hi) {
  double a, b;
  if (crop_reach(ppi, &a, &b))
    return FB_NO_PEAK;
  a += FOCUS_X_MARGIN;
  b -= FOCUS_X_MARGIN;
  if (x0 > a)
    a = x0;
  if (x1 < b)
    b = x1;
  if (!(b - a >= FOCUS_W_MIN))
    return FB_NO_PEAK;
  *lo = a;
  *hi = b;
  return 0;
}

#define FOCUS_N 5 /* crops in the first bracket */
#define CROP_S 12 /* rough seconds per focus crop */

/* One check point on frame fr: 0 with *off, FB_NO_PEAK when the frame
 * cannot give one (no texture, outside the crop area, no peak), -1 on a
 * scan or file failure. */
static int focus_point(const float *t, int lines, int px, const ft_axes *ax,
                       double ppi, const ft_job_frame *fr, long *off) {
  double score, x0, x1, yc = textured_band(t, lines, px, ax, fr, &score);
  int rc;
  if (!(score >= TEXTURE_MIN)) {
    printf("frame %d: no texture for focus\n", fr->index);
    return FB_NO_PEAK;
  }
  if (focus_span(ppi, fr->x0, fr->x0 + fr->width, &x0, &x1)) {
    printf("frame %d: outside the area a crop can scan\n", fr->index);
    return FB_NO_PEAK;
  }
  printf("focus check, frame %d at %.1f mm (%.1f..%.1f mm across)\n", fr->index,
         yc, x0, x1);
  g_pg_frame = fr->index;
  rc = focus_bracket(ppi, (x0 + x1) / 2, yc, x1 - x0, 0, 500, FOCUS_N,
                     FB_FRAME_SAMPLES, 0, off);
  g_pg_frame = 0;
  return rc;
}

/* 3-point focus check on the first, middle and last whole frame; a point
 * that gives no peak moves to the next whole frame once, then is dropped.
 * Sets j->focus / focus_offset. Returns -1 on a scan or file failure
 * (j untouched), else 0. */
static int focus_check(const float *t, int lines, int px, const ft_axes *ax,
                       double ppi, ft_job *j) {
  int cand[FT_MAX_FRAMES], used[FT_MAX_FRAMES], nc = 0, np = 0, npt = 0;
  int pos[3], dir[3], p, i, k, rc;
  long off[3];

  for (i = 0; i < j->nframes && i < FT_MAX_FRAMES; i++)
    if (j->f[i].type != FT_FRAME_PARTIAL) {
      used[nc] = 0;
      cand[nc++] = i;
    }
  if (nc > 0) {
    pos[npt] = 0;
    dir[npt++] = 1;
  }
  if (nc > 2) {
    pos[npt] = nc / 2;
    dir[npt++] = 1;
  }
  if (nc > 1) {
    pos[npt] = nc - 1;
    dir[npt++] = -1;
  }
  if (npt)
    printf("focus check: %d points, ~%d crops (up to %d), ~%d min\n", npt,
           npt * FOCUS_N, npt * 2 * FB_FRAME_SAMPLES,
           (npt * FOCUS_N * CROP_S + 59) / 60);
  for (p = 0; p < npt; p++) {
    int tried = 0, ok = 0;
    for (i = pos[p]; tried < 2 && i >= 0 && i < nc; i += dir[p]) {
      if (used[i])
        continue;
      used[i] = 1;
      tried++;
      rc = focus_point(t, lines, px, ax, ppi, &j->f[cand[i]], &off[np]);
      if (rc < 0)
        return -1;
      if (rc == 0) {
        np++;
        ok = 1;
        break;
      }
      if (tried < 2)
        printf("  trying the next frame for this point\n");
    }
    if (!ok)
      printf("focus point %d of %d dropped\n", p + 1, npt);
  }
  if (np == 0) {
    printf("WARNING: no usable focus point; using the focus curve as is\n");
    j->focus = FT_FOCUS_SHIFTED;
    j->focus_offset = 0;
    return 0;
  }
  {
    double s = 0, m;
    long mean;
    for (k = 0; k < np; k++)
      s += off[k];
    m = s / np;
    mean = (long)(m < 0 ? m - 0.5 : m + 0.5);
    while (np < 3) /* fewer points: repeat the mean */
      off[np++] = mean;
  }
  j->focus = ft_focus_decide(off, &j->focus_offset);
  printf("focus offsets %ld %ld %ld -> %s\n", off[0], off[1], off[2],
         j->focus == FT_FOCUS_PER_FRAME ? "per-frame focus" : "shifted curve");
  if (j->focus == FT_FOCUS_PER_FRAME) {
    int n = 0;
    for (i = 0; i < j->nframes; i++)
      n += j->f[i].include != 0;
    printf("per-frame focus adds ~%d crops (~%d min) to scan --job\n",
           n * FOCUS_N, (n * FOCUS_N * CROP_S + 59) / 60);
  }
  return 0;
}

/* Warn (per included frame) about anything ft_job_validate would refuse
 * in scan --job; the job is written regardless, to be fixed by hand. */
static void warn_job(const ft_job *j, const char *path) {
  static ft_job one;
  ft_err e, all;
  int i, n = 0;

  if (ft_job_validate(j, &g_lens, &all) == 0)
    return;
  printf("WARNING: scan --job will refuse %s as written (fix it there):\n",
         path);
  for (i = 0; i < j->nframes; i++) {
    if (!j->f[i].include)
      continue;
    one = *j;
    one.nframes = 1;
    one.f[0] = j->f[i];
    if (ft_job_validate(&one, &g_lens, &e)) {
      printf("  %s\n", e.msg);
      n++;
    }
  }
  if (!n)
    printf("  %s\n", all.msg);
}

static int detail_plan(const float *t, int lines, int px, const ft_axes *ax,
                       double ppi, const char *select, ft_job *j);

/* frames --share: pick the frames, price every bin (ft_share.h), keep
 * the most light, write the plan into the job. Focus: per-frame, which a
 * share job's scan --job takes as one bracket on the first window, at
 * its own optics, carried to the rest (no 3-point check here: it would bracket
 * at another magnification, the fastest bin at the job's ppi). -1 on a bad
 * selection (message printed). */
static int plan_share(ft_job *j, const ft_holder *h, double share, int hf,
                      const char *select) {
  static ft_share_frame sel[FT_MAX_FRAMES];
  static ft_share_est c[6];
  ft_share_opts o;
  ft_err e;
  int n, nc, k, i, w;

  ft_share_opts_default(&o, h->scan_len_mm);
  o.target_px = share;
  o.hf_half_res = hf;
  if (ft_share_select(j, select, sel, &n, &e)) {
    fprintf(stderr, "frames: %s\n", e.msg);
    return -1;
  }
  nc = ft_share_candidates(sel, n, &o, &g_lens, c, &e);
  if (nc < 1) {
    fprintf(stderr, "frames: %s\n", e.msg);
    return -1;
  }
  k = ft_share_pick_max_dmax(c, nc);
  ft_share_apply(j, sel, n, &o, &c[k]);
  j->focus = FT_FOCUS_PER_FRAME;
  j->focus_offset = 0;
  printf("\n share: %g px on each frame's long side%s, %d frame(s)\n"
         "   bin   ppi  optics win    scan   focus   total  light  Dmax~\n",
         share, hf ? " (half frames at the FF ppi)" : "", n);
  for (i = 0; i < nc; i++)
    printf(" %s %3d %5.0f %7.0f %3d %5.1f m %5.1f m %5.1f m %5.1fx  %.2f\n",
           i == k ? "*" : " ", c[i].bin, c[i].ppi, c[i].scale, c[i].nwin,
           c[i].scan_s / 60, c[i].focus_s / 60, c[i].seconds / 60, c[i].light,
           c[i].dmax);
  printf(" * chosen (most light); windows (job key group):\n");
  for (w = 0; w < c[k].nwin; w++) {
    const ft_share_window *x = &c[k].win[w];
    printf("   %d: frame %d", w + 1, sel[x->first].index);
    if (x->last > x->first)
      printf(" to %d (%d selected)", sel[x->last].index,
             x->last - x->first + 1);
    printf(", %.1f..%.1f mm\n", x->y0, x->y0 + x->len);
  }
  printf(" focus: scan --job brackets the first window, the rest carry "
         "its offset (focus = shifted: none)\n");
  return 0;
}

static int cmd_frames(int argc, char **argv) {
  const char *base = "strip";
  char raw[260], path[260];
  ft_film film = FT_FILM_NEGATIVE;
  double ppi = 2000, share = 0;
  int sprockets = 0, i, lines = 0, px = 0, rc, focus_failed = 0;
  int hf_half_res = -1;
  const char *select = NULL;
  const ft_holder *h = ft_holder_find("24x217");
  const ft_preview *pm = ft_preview_find("standard");
  static ft_layout L;
  static ft_job j;
  ft_axes ax;
  ft_err e;
  float *t;
  FILE *f;
  /* detailed scans: --detail neg|pos and its options */
  int detail = FT_DETAIL_NONE, dfmt = FT_DFMT_DNG, dbw = FT_BW_OFF;
  int keep_raw = 0, ring_mb = FT_RING_MB_DEFAULT, preview_set = 0;

  for (i = 0; i < argc; i += 2) {
    if (i + 1 >= argc) {
      fprintf(stderr, "frames: %s needs a value\n", argv[i]);
      return 2;
    }
    if (strcmp(argv[i], "--film") == 0) {
      if (strcmp(argv[i + 1], "slide") == 0)
        film = FT_FILM_SLIDE;
      else if (strcmp(argv[i + 1], "neg") == 0)
        film = FT_FILM_NEGATIVE;
      else {
        fprintf(stderr, "frames: --film neg|slide\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--ppi") == 0)
      ppi = atof(argv[i + 1]);
    else if (strcmp(argv[i], "--sprockets") == 0) {
      if (strcmp(argv[i + 1], "yes") == 0)
        sprockets = 1;
      else if (strcmp(argv[i + 1], "no") == 0)
        sprockets = 0;
      else {
        fprintf(stderr, "frames: --sprockets yes|no\n");
        return 2;
      }
    } else if (strcmp(argv[i], "-h") == 0) {
      if (parse_hold(argv[i + 1], &g_first_hold))
        return 2;
    } else if (strcmp(argv[i], "--preview") == 0) {
      pm = ft_preview_find(argv[i + 1]);
      preview_set = 1;
      if (!pm) {
        fprintf(stderr, "frames: --preview standard|1000|1500\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--detail") == 0) {
      if (strcmp(argv[i + 1], "neg") == 0)
        detail = FT_DETAIL_NEG;
      else if (strcmp(argv[i + 1], "pos") == 0)
        detail = FT_DETAIL_POS;
      else {
        fprintf(stderr, "frames: --detail neg|pos\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--fmt") == 0) {
      if (strcmp(argv[i + 1], "dng") == 0)
        dfmt = FT_DFMT_DNG;
      else if (strcmp(argv[i + 1], "tiff") == 0)
        dfmt = FT_DFMT_TIFF;
      else {
        fprintf(stderr, "frames: --fmt dng|tiff\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--bw") == 0) {
      if (strcmp(argv[i + 1], "off") == 0)
        dbw = FT_BW_OFF;
      else if (strcmp(argv[i + 1], "mono") == 0)
        dbw = FT_BW_MONO;
      else if (strcmp(argv[i + 1], "rgb") == 0)
        dbw = FT_BW_RGB;
      else {
        fprintf(stderr, "frames: --bw off|mono|rgb\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--keep-raw") == 0) {
      if (strcmp(argv[i + 1], "yes") == 0)
        keep_raw = 1;
      else if (strcmp(argv[i + 1], "no") == 0)
        keep_raw = 0;
      else {
        fprintf(stderr, "frames: --keep-raw yes|no\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--ring-mb") == 0) {
      ring_mb = atoi(argv[i + 1]);
      if (ring_mb < FT_RING_MB_MIN || ring_mb > FT_RING_MB_MAX) {
        fprintf(stderr, "frames: --ring-mb %d..%d\n", FT_RING_MB_MIN,
                FT_RING_MB_MAX);
        return 2;
      }
    } else if (strcmp(argv[i], "--select") == 0)
      select = argv[i + 1];
    else if (strcmp(argv[i], "-o") == 0)
      base = argv[i + 1];
    else if (strcmp(argv[i], "--share") == 0) {
      share = atof(argv[i + 1]);
      if (!(share == 0 || (share >= 100 && share <= 20000))) {
        fprintf(stderr, "frames: --share px on the frame's long side "
                        "(100..20000; 0 = off)\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--hf-half-res") == 0) {
      if (strcmp(argv[i + 1], "on") == 0)
        hf_half_res = 1;
      else if (strcmp(argv[i + 1], "off") == 0)
        hf_half_res = 0;
      else {
        fprintf(stderr, "frames: --hf-half-res on|off\n");
        return 2;
      }
    } else {
      fprintf(stderr, "frames: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (hf_half_res >= 0 && !(share > 0)) {
    fprintf(stderr, "frames: --hf-half-res needs --share\n");
    return 2;
  }
  if (detail == FT_DETAIL_NONE && !(share > 0) && select) {
    fprintf(stderr, "frames: --select needs --detail neg|pos or --share\n");
    return 2;
  }
  if (!ft_ppi_supported(ppi)) {
    fprintf(stderr, "frames: ppi %g not supported\n", ppi);
    return 2;
  }
  if (detail == FT_DETAIL_NONE &&
      (dbw != FT_BW_OFF || keep_raw || dfmt != FT_DFMT_DNG)) {
    fprintf(stderr, "frames: --fmt/--bw/--keep-raw need "
                    "--detail neg|pos\n");
    return 2;
  }
  if (detail != FT_DETAIL_NONE) {
    /* the preview at the scan's own optics: same light, same Dmax */
    if (ppi != 3150 && ppi != 6300) {
      fprintf(stderr, "frames: --detail scans at --ppi 3150 or 6300\n");
      return 2;
    }
    if (preview_set) {
      fprintf(stderr, "frames: --detail previews at the scan's optics "
                      "(no --preview)\n");
      return 2;
    }
    if (detail == FT_DETAIL_POS &&
        (film != FT_FILM_NEGATIVE || dfmt != FT_DFMT_DNG)) {
      fprintf(stderr, "frames: --detail pos is for negatives and "
                      "always TIFF (no --fmt)\n");
      return 2;
    }
    if (detail == FT_DETAIL_NEG && dbw != FT_BW_OFF) {
      fprintf(stderr, "frames: --bw is for --detail pos\n");
      return 2;
    }
    if (detail == FT_DETAIL_POS)
      dfmt = FT_DFMT_TIFF;
    pm = ft_preview_find(ppi == 6300 ? "standard" : "o3150");
  }
  if (!h) {
    fprintf(stderr, "frames: no 24x217 holder geometry\n");
    return 1;
  }
  if (snprintf(raw, sizeof raw, "%s.raw", base) >= (int)sizeof raw ||
      snprintf(path, sizeof path, "%s.job", base) >= (int)sizeof path) {
    fprintf(stderr, "frames: -o name too long\n");
    return 2;
  }

  g_lamp = 255; /* the lamp level persists: set it */
  /* the preview is the run's first scan: it holds g_first_hold (-h) */
  pg_begin(FT_PG_PREVIEW, 1);
  pg_unit(FT_PG_PREVIEW, 1, 1);
  g_pg_track = 1;
  rc = preview_scan(pm, 0, g_first_hold, raw);
  g_pg_track = 0;
  if (rc)
    return 1;
  if (pm->bin == 12) {
    /* the axes below assume SCAN_WIN's geometry: 666 px from pixel 2 */
    if (meta_long(raw, "px") != FT_PREVIEW_PX ||
        meta_long(raw, "x") != FT_PREVIEW_X_OUT) {
      fprintf(stderr,
              "frames: %s.txt: expected px=666 x=2 (got %ld, "
              "%ld)\n",
              raw, meta_long(raw, "px"), meta_long(raw, "x"));
      return 1;
    }
    px = FT_PREVIEW_PX;
    t = load_corrected(raw, px, 12, FT_PREVIEW_X_OUT, &lines);
  } else {
    long mpx = meta_long(raw, "px"), mx = meta_long(raw, "x");
    if (meta_long(raw, "bin") != pm->bin || mpx < 1 || mpx > 20000 || mx < 0) {
      fprintf(stderr,
              "frames: %s.txt: unexpected geometry (px %ld, "
              "x %ld, bin %ld)\n",
              raw, mpx, mx, meta_long(raw, "bin"));
      return 1;
    }
    t = load_preview_ds(raw, (int)mpx, pm->bin, (int)mx, pm->down, &lines, &px);
  }
  if (!t)
    return 1;

  ft_preview_axes(pm, meta_long(raw, "x"), &ax);
  if (ft_detect(t, lines, px, &ax, film, h, &L, &e)) {
    fprintf(stderr, "frames: %s\n", e.msg);
    free(t);
    return 1;
  }
  if (L.fallback)
    printf("WARNING: no trusted frame edge on part of the strip; "
           "gridded at %g mm there (frames flagged LOW)\n",
           h->grid_pitch_mm);
  if (L.nblank)
    printf("note: %d blank frame(s) dropped\n", L.nblank);
  if (L.nblank_end)
    printf("note: %d blank frame(s) of unexposed film at the strip ends\n",
           L.nblank_end);
  ft_layout_to_job(&L, h, film, ppi, sprockets, &j);
  printf("film %s: D %.3f %.3f %.3f (%s)\n",
         film == FT_FILM_SLIDE ? "black" : "base", j.film_base[0],
         j.film_base[1], j.film_base[2],
         L.has_holes ? "rebate between the holes"
                     : "estimated: no hole rows, extreme film pixels");

  if (detail != FT_DETAIL_NONE) {
    j.detail = (ft_detail_out)detail;
    j.detail_fmt = (ft_detail_fmt)dfmt;
    j.detail_bw = (ft_detail_bw)dbw;
    j.keep_raw = keep_raw;
    j.ring_mb = ring_mb;
    if (detail_plan(t, lines, px, &ax, ppi, select, &j)) {
      free(t);
      return 1;
    }
    focus_failed = 0;
  } else if (share > 0) {
    if (plan_share(&j, h, share, hf_half_res > 0, select)) {
      free(t);
      return 2;
    }
  } else {
    focus_failed = focus_check(t, lines, px, &ax, ppi, &j) != 0;
    if (focus_failed) {
      j.focus = FT_FOCUS_SHIFTED;
      j.focus_offset = 0;
    }
  }
  warn_job(&j, path);

  f = fopen(path, "w");
  rc = !f;
  if (f) {
    rc = ft_job_write(&j, f) != 0;
    rc |= fclose(f) != 0;
  }
  if (rc) {
    fprintf(stderr, "frames: cannot write %s\n", path);
    free(t);
    return 1;
  }
  pg_file(path);
  snprintf(path, sizeof path, "%s.png", base);
  rc = write_strip_png(path, t, lines, px, &ax, &j);
  free(t);
  if (!rc)
    pg_file(path);
  print_job(&j);
  if (!(share > 0))
    print_options(&j);
  printf("\nwrote %s.job and %s.png; edit the job, then: ftpoc scan --job "
         "%s.job\n",
         base, base, base);
  if (detail != FT_DETAIL_NONE)
    printf("detailed scan: %s, %g ppi; scan --job writes %s_frameNN%s\n",
           detail == FT_DETAIL_NEG ? "digital negatives" : "archive positives",
           ppi, base,
           detail == FT_DETAIL_NEG && dfmt == FT_DFMT_DNG ? ".dng" : ".tif");
  if (focus_failed)
    printf("\n*** FOCUS NOT CHECKED: a focus crop %s. The job uses the\n"
           "*** focus curve as is (shifted, offset 0); check the scanner and\n"
           "*** re-run frames, or edit focus/focus_offset by hand.\n",
           g_abort ? "was interrupted" : "failed");
  return rc || focus_failed ? 1 : 0;
}

/* ------------------------------------------------- scan --job, calibrate */

/* Atomic rewrite: <path>.tmp, then renamed over path, so an interrupted
 * write never leaves a truncated job. */
static int save_job(const char *path, const ft_job *j) {
  char tmp[300];
  FILE *f;
  int rc;
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
    fprintf(stderr, "%s: name too long\n", path);
    return 1;
  }
  f = fopen(tmp, "w");
  rc = !f;
  if (f) {
    rc = ft_job_write(j, f) != 0;
    rc |= fclose(f) != 0;
  }
  if (!rc && io_replace(tmp, path)) {
    fprintf(stderr, "cannot replace %s (%s)\n", path, io_syserr());
    rc = 1;
  }
  if (rc) {
    fprintf(stderr, "cannot rewrite %s\n", path);
    remove(tmp);
  }
  return rc;
}

static long round_l(double v) { return (long)floor(v + 0.5); }

#define JOB_EDGE 0.01 /* mm kept inside the crop reach (rounding) */

/* Per-frame focus centre: the frame's focus_offset, the strip's when 0. */
static long frame_focus_base(const ft_job *j, const ft_job_frame *fr) {
  return fr->focus_offset ? fr->focus_offset : j->focus_offset;
}

/* The scan request for a frame: its window plus a margin (measured tilt
 * over the width + 0.3 mm), clipped to the holder and the CCD reach at
 * the frame's ppi but never into the window (crop_check then refuses). */
static void job_window(const ft_job_frame *fr, const ft_holder *h, long off,
                       struct crop_req *q) {
  double margin, rlo, rhi, x0, x1, y0, y1;
  margin = fabs(tan(fr->tilt_deg * 3.14159265358979 / 180)) * fr->width + 0.3;
  if (crop_reach(fr->ppi, &rlo, &rhi)) {
    rlo = 0; /* crop_check refuses the ppi */
    rhi = CROP_X_MAX;
  }
  rlo += JOB_EDGE;
  rhi -= JOB_EDGE;
  if (rhi > h->scan_width_mm)
    rhi = h->scan_width_mm;
  x0 = fr->x0 - margin;
  if (x0 < rlo)
    x0 = rlo < fr->x0 ? rlo : fr->x0;
  x1 = fr->x0 + fr->width + margin;
  if (x1 > rhi)
    x1 = rhi > fr->x0 + fr->width ? rhi : fr->x0 + fr->width;
  y0 = fr->y0 - margin;
  if (y0 < 0)
    y0 = 0;
  y1 = fr->y0 + fr->len + margin;
  if (y1 > h->scan_len_mm)
    y1 = h->scan_len_mm;
  memset(q, 0, sizeof *q);
  q->ppi = fr->ppi;
  q->bin = fr->bin;
  q->x = x0;
  q->y = y0;
  q->w = x1 - x0;
  q->h = y1 - y0;
  q->ccd = q->optic = -1;
  q->focus_offset = off;
  q->hold = 3000; /* cmd_scan: >= g_first_hold first */
}

/* Append the frame's tight crop, in output pixels of the scan as sent
 * (origin and size from <out>.txt), to <out>.txt. Rows count in the
 * RGB-aligned image (ftraw.py, load_corrected): row r is green row
 * r + k of the scan, k = 12 / bin lines dropped at each end. */
static void write_crop(const char *out, const ft_job *j, const ft_job_frame *fr,
                       long off) {
  char meta[300];
  long xs = meta_long(out, "x"), ys = meta_long(out, "y");
  long pxs = meta_long(out, "px"), ls = meta_long(out, "lines");
  long cx, cy, cw, ch, k;
  int bin = ft_job_bin(fr);
  double px_mm = fr->ppi / 25.4;
  FILE *m;

  if (snprintf(meta, sizeof meta, "%s.txt", out) >= (int)sizeof meta) {
    fprintf(stderr, "warning: %s: name too long; crop not recorded\n", out);
    return;
  }
  k = bin >= 1 ? 12 / bin : 0;
  ls -= 2 * k; /* aligned image height */
  if (xs < 0 || ys < 0 || pxs < 1 || ls < 1 || bin < 1) {
    fprintf(stderr,
            "warning: %s lacks the scan geometry; crop not "
            "recorded\n",
            meta);
    return;
  }
  /* output pixel j covers CCD pixels (x + j) * bin + FT_CCD_FIRST ... */
  cx = round_l((ft_mm_to_ccd(fr->x0, fr->ppi * bin) - FT_CCD_FIRST) / bin) - xs;
  cy = round_l(fr->y0 * px_mm) - ys - k;
  cw = round_l(fr->width * px_mm);
  ch = round_l(fr->len * px_mm);
  if (cx < 0) {
    cw += cx;
    cx = 0;
  }
  if (cy < 0) {
    ch += cy;
    cy = 0;
  }
  if (cw > pxs - cx)
    cw = pxs - cx;
  if (ch > ls - cy)
    ch = ls - cy;
  m = fopen(meta, "a");
  if (m) {
    fprintf(m,
            "crop_x=%ld\ncrop_y=%ld\ncrop_w=%ld\ncrop_h=%ld\n"
            "tilt=%.3f\nstraighten=%d\nfocus_offset=%ld\n",
            cx, cy, cw, ch, fr->tilt_deg, fr->straighten, off);
    if (j->has_film_base) /* the render's base (ftdev) */
      fprintf(m, "film_base=%.4f %.4f %.4f\n", j->film_base[0], j->film_base[1],
              j->film_base[2]);
    if (fclose(m) != 0)
      m = NULL;
  }
  if (!m)
    fprintf(stderr, "warning: cannot append the crop to %s\n", meta);
}

/* ------------------------------------------------ detailed scans (2b) */

/* flood bytes of a detailed pass: calibration lines to RAM, image lines
 * to the ring (never blocking: a full ring stops the scan) */
static int detail_put(const BYTE *p, DWORD n) {
  struct detail_run *r = g_detail;
  if (!r)
    return 0;
  if (r->phase == 0) {
    size_t take = n;
    if (take > r->cal_cap - r->cal_n)
      take = r->cal_cap - r->cal_n;
    memcpy((BYTE *)r->cal + r->cal_n, p, take);
    r->cal_n += take;
    return 0;
  }
  if (ring_write(r->r, p, n) == 0)
    return 0;
  printf("\n%s; stopping the scan\n",
         ring_failed(r->r)
             ? "the pass worker failed"
             : "RING FULL: the disk fell behind the scanner (a larger "
               "ring_mb, a faster card, or a two-stage scan)");
  return -1;
}

/* after the calibration lines: open the pass (its files are created
 * here) and the worker thread behind the ring scan_detail allocated */
static int detail_start(const char *out) {
  struct detail_run *r = g_detail;
  char fp[300];
  char *faul;
  long nfaul = 0;
  ft_err e;

  if (r->cal_n < r->cal_cap) {
    printf("detail: %lu of %lu calibration bytes\n", (unsigned long)r->cal_n,
           (unsigned long)r->cal_cap);
    return 1;
  }
  snprintf(fp, sizeof fp, "%s.faul", out);
  faul = load_file(fp, &nfaul);
  r->d = ft_detail_open(&r->o, r->cal, FT_CAL_LINES, faul,
                        (size_t)(faul ? nfaul : 0), &e);
  free(faul);
  if (!r->d) {
    printf("detail: %s\n", e.msg);
    return 1;
  }
  memset(&r->w, 0, sizeof r->w);
  r->w.r = r->r;
  r->w.d = r->d;
  if (pass_worker_start(&r->w)) {
    printf("detail: cannot start the worker thread\n");
    ft_detail_abort(r->d);
    r->d = NULL;
    return 1;
  }
  r->phase = 1;
  return 0;
}

/* the pass's scan window: its frames' boxes + FT_DETAIL_MARGIN_MM along,
 * the whole reach of the bin-1 optics across (detection sees the hole
 * rows), and the middle of their focus offsets. -1 if no frame. */
static int detail_window(const ft_job *j, int pass, const ft_holder *h,
                         struct crop_req *q, ft_dscan *ds) {
  double y0 = 1e9, y1 = -1e9, lo, hi, ppi = 0;
  long flo = LONG_MAX, fhi = LONG_MIN;
  int i, n = 0;
  for (i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    if (!f->include || f->pass != pass)
      continue;
    if (f->y0 < y0)
      y0 = f->y0;
    if (f->y0 + f->len > y1)
      y1 = f->y0 + f->len;
    if (f->focus_offset < flo)
      flo = f->focus_offset;
    if (f->focus_offset > fhi)
      fhi = f->focus_offset;
    ppi = f->ppi;
    n++;
  }
  if (!n)
    return -1;
  y0 -= FT_DETAIL_MARGIN_MM;
  y1 += FT_DETAIL_MARGIN_MM;
  if (y0 < 0)
    y0 = 0;
  if (y1 > h->scan_len_mm)
    y1 = h->scan_len_mm;
  ft_dscan_reach(ppi, FT_DETAIL_BIN, &lo, &hi);
  lo += JOB_EDGE;
  hi -= JOB_EDGE;
  if (lo < JOB_EDGE)
    lo = JOB_EDGE;
  if (hi > CROP_X_MAX - JOB_EDGE)
    hi = CROP_X_MAX - JOB_EDGE;
  if (hi > h->scan_width_mm)
    hi = h->scan_width_mm;
  memset(q, 0, sizeof *q);
  q->ppi = ppi;
  q->bin = FT_DETAIL_BIN;
  q->x = lo;
  q->w = hi - lo;
  q->y = y0;
  q->h = y1 - y0;
  q->ccd = q->optic = -1;
  q->focus_offset = ft_dplan_mid(flo, fhi);
  q->hold = 3000;
  ft_dscan_for(ppi, FT_DETAIL_BIN, q->x, q->w, q->y, q->h, ds);
  return 0;
}

/* bytes the pass's frame files take */
static double detail_bytes(const ft_job *j, int pass, const ft_dscan *ds) {
  ft_tiff_kind k = ft_detail_kind(j->detail, j->detail_fmt, j->detail_bw);
  double sum = 0;
  int i, r[4];
  for (i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    if (!f->include || f->pass != pass)
      continue;
    if (ft_dscan_ranges(ds, f->y0 - FT_DETAIL_MARGIN_MM,
                        f->y0 + f->len + FT_DETAIL_MARGIN_MM,
                        f->x0 - FT_DETAIL_MARGIN_MM,
                        f->x0 + f->width + FT_DETAIL_MARGIN_MM, r) == 0)
      sum += ft_tiff_bytes(k, r[1] - r[0], r[3] - r[2]);
  }
  return sum;
}

/* frames --detail: a focus point on every selected frame at the scan's
 * optics (bin 1), then the passes (ft_dplan) written into the job */
static int detail_plan(const float *t, int lines, int px, const ft_axes *ax,
                       double ppi, const char *select, ft_job *j) {
  static ft_dframe df[FT_MAX_FRAMES];
  static ft_dplan plan;
  const ft_holder *h = ft_holder_find(j->holder);
  int idx[FT_MAX_FRAMES], ok[FT_MAX_FRAMES], n = 0, i, k, p, nok = 0;
  long meas[FT_MAX_FRAMES], med = 0;
  double total_s = 0, total_b = 0;
  ft_err e;

  for (i = 0; i < j->nframes; i++) {
    ft_job_frame *f = &j->f[i];
    int sel = f->type != FT_FRAME_PARTIAL;
    if (select && (sel = ft_dplan_selected(select, f->index, &e)) < 0) {
      fprintf(stderr, "frames: %s\n", e.msg);
      return -1;
    }
    f->include = sel;
    f->pass = 0;
    f->ppi = ppi;
    f->bin = FT_DETAIL_BIN;
    f->focus_offset = 0;
    if (sel)
      idx[n++] = i;
  }
  if (!n) {
    fprintf(stderr, "frames: no frame selected\n");
    return -1;
  }
  printf("detail focus: %d frame(s) at %g ppi, bin 1, ~%d min\n", n, ppi,
         (n * FOCUS_N * CROP_S + 59) / 60);
  g_focus_bin = FT_DETAIL_BIN;
  for (k = 0; k < n; k++) {
    int rc = focus_point(t, lines, px, ax, ppi, &j->f[idx[k]], &meas[k]);
    if (rc < 0) {
      g_focus_bin = 0;
      return -1;
    }
    ok[k] = rc == 0;
    nok += ok[k];
  }
  g_focus_bin = 0;
  if (nok) { /* frames without a peak: the median */
    long v[FT_MAX_FRAMES];
    int m = 0, a, b;
    for (k = 0; k < n; k++)
      if (ok[k])
        v[m++] = meas[k];
    for (a = 1; a < m; a++)
      for (b = a; b > 0 && v[b - 1] > v[b]; b--) {
        long tmp = v[b];
        v[b] = v[b - 1];
        v[b - 1] = tmp;
      }
    med = m % 2 ? v[m / 2] : ft_dplan_mid(v[m / 2 - 1], v[m / 2]);
  }
  for (k = 0; k < n; k++) {
    const ft_job_frame *f = &j->f[idx[k]];
    if (!ok[k])
      printf("WARNING: frame %d: no focus peak; using %ld (the median "
             "of the others)\n",
             f->index, med);
    df[k].index = f->index;
    df[k].y0 = f->y0;
    df[k].len = f->len;
    df[k].focus = ok[k] ? meas[k] : med;
  }
  if (ft_dplan_make(df, n, ppi, h->scan_len_mm, FT_DETAIL_DOF_SPAN, &plan,
                    &e)) {
    fprintf(stderr, "frames: %s\n", e.msg);
    return -1;
  }
  j->focus = FT_FOCUS_PER_FRAME;
  j->focus_offset = 0;
  printf("\ndetailed scan: %d pass(es)\n", plan.npass);
  for (p = 0; p < plan.npass; p++) {
    const ft_dpass *ps = &plan.p[p];
    struct crop_req q;
    ft_dscan ds;
    double b;
    for (k = ps->first; k <= ps->last; k++) {
      j->f[idx[k]].pass = p + 1;
      j->f[idx[k]].focus_offset = df[k].focus;
    }
    detail_window(j, p + 1, h, &q, &ds);
    b = detail_bytes(j, p + 1, &ds);
    printf("  pass %d: frames %d..%d, %.1f..%.1f mm, focus %+ld, ~%.0f s, "
           "%.0f MB\n",
           p + 1, df[ps->first].index, df[ps->last].index, ps->y0,
           ps->y0 + ps->len, ps->focus, ps->seconds, b / 1048576);
    total_s += ps->seconds;
    total_b += b;
  }
  printf("  total ~%.0f min, %.0f MB\n", total_s / 60, total_b / 1048576);
  return 0;
}

/* scan --job for a detailed scan: one scan per pass, developed and
 * written while it arrives. Every pass is checked and the disk's free
 * space with it before the scanner moves. */
static int scan_detail(ft_job *j, const char *path, const char *jobbase,
                       const ft_holder *h) {
  static struct detail_run run;
  static ft_detail_result res;
  static ft_image meta;
  int maxp = 0, npass = 0, p, i, k;
  double need = 0, secs = 0;
  double freeb;
  char dir[260];

  for (i = 0; i < j->nframes; i++)
    if (j->f[i].include && !j->f[i].done && j->f[i].pass > maxp)
      maxp = j->f[i].pass;
  for (p = 1; p <= maxp; p++) {
    struct crop_req q;
    ft_dscan ds;
    long ccd, optic;
    if (!ft_job_pass_todo(j, p) || detail_window(j, p, h, &q, &ds))
      continue;
    npass++;
    if (crop_check(&q, &ccd, &optic)) {
      printf("pass %d: REFUSED (reason above); nothing scanned\n", p);
      g_pg_refused = 1;
      return 1;
    }
    need += detail_bytes(j, p, &ds);
    secs += ft_scan_seconds(q.ppi, q.bin, q.h);
  }
  if (!maxp) {
    printf("\nnothing to scan (every included frame is done)\n");
    return 0;
  }
  snprintf(dir, sizeof dir, "%s", jobbase);
  for (k = (int)strlen(dir); k > 0 && dir[k - 1] != '/' && dir[k - 1] != '\\';
       k--)
    ;
  dir[k] = 0;
  if (io_free_bytes(k ? dir : NULL, &freeb) == 0) {
    printf("%s: %d pass(es), ~%.0f min, %.0f MB of files, %.0f MB free\n", path,
           npass, secs / 60, need / 1048576, freeb / 1048576);
    if (freeb < need * 1.05) {
      printf("not enough free space (outputs + 5 %%); nothing "
             "scanned\n");
      return 1;
    }
  } else {
    printf("WARNING: free space unknown (%s)\n", io_syserr());
  }

  ft_image_init(&meta);
  g_lamp = 255;
  pg_begin(FT_PG_PASS, npass);
  npass = 0; /* now: passes started */
  for (p = 1; p <= maxp && !g_abort; p++) {
    struct crop_req q;
    ft_dscan ds;
    long ccd, optic;
    char out[300], report[256];
    double t0;
    int rc, n = 0;
    ft_err e;

    if (!ft_job_pass_todo(j, p) || detail_window(j, p, h, &q, &ds))
      continue;
    memset(&run, 0, sizeof run);
    run.o.scan = ds;
    run.o.film = j->film;
    run.o.out = j->detail;
    run.o.fmt = j->detail_fmt;
    run.o.bw = j->detail_bw;
    memcpy(run.o.film_base, j->film_base, sizeof run.o.film_base);
    run.o.margin_mm = FT_DETAIL_MARGIN_MM;
    run.o.holder = h;
    run.o.meta = &meta;
    run.o.base = jobbase;
    run.o.pass = p;
    run.o.focus = q.focus_offset;
    run.o.keep_raw = j->keep_raw;
    for (i = 0; i < j->nframes; i++) {
      const ft_job_frame *f = &j->f[i];
      if (!f->include || f->pass != p)
        continue;
      run.o.f[n].index = f->index;
      run.o.f[n].y0 = f->y0;
      run.o.f[n].y1 = f->y0 + f->len;
      run.o.f[n].x0 = f->x0;
      run.o.f[n].x1 = f->x0 + f->width;
      n++;
    }
    run.o.nframes = n;
    run.ring_mb = j->ring_mb;
    run.cal_cap = (size_t)FT_CAL_LINES * ds.px * 6;
    run.cal = malloc(run.cal_cap);
    if (!run.cal || crop_check(&q, &ccd, &optic)) {
      free(run.cal);
      printf("pass %d: out of memory or refused; stopped\n", p);
      return 1;
    }
    /* the ring before the scanner moves: a failed allocation now
     * stops the job, not the scan halfway through calibration */
    run.r = ring_open((size_t)ds.px * 6, (size_t)run.ring_mb << 20);
    if (!run.r) {
      free(run.cal);
      printf("pass %d: no room for a %d MB ring (a smaller ring_mb); "
             "stopped\n",
             p, run.ring_mb);
      return 1;
    }
    snprintf(out, sizeof out, "%s_pass%02d.raw", jobbase, p);
    printf("\n=== pass %d: %d frame(s), %g ppi, bin 1, %.1f..%.1f mm "
           "(~%.0f s), focus offset %+ld\n",
           p, n, q.ppi, q.y, q.y + q.h, ft_scan_seconds(q.ppi, 1, q.h),
           q.focus_offset);
    g_detail = &run;
    t0 = ring_ms();
    pg_unit(FT_PG_PASS, ++npass, p);
    g_pg_track = 1;
    rc = synth_scan("detail", "detailed-scan pass", q.ppi, 1, ds.x_out,
                    ds.y_line, ds.px, ds.lines, ccd, optic, 0, q.hold, out);
    g_pg_track = 0;
    g_detail = NULL;
    if (run.d) {
      ring_end(run.r);
      pass_worker_join(&run.w);
      pass_worker_report(&run.w, (ring_ms() - t0) / 1000, report,
                         sizeof report);
      printf("pass %d: %s\n", p, report);
    }
    if (!j->keep_raw) {
      char fp[320];
      snprintf(fp, sizeof fp, "%s.faul", out);
      remove(fp);
    }
    if (rc || !run.d || run.w.failed) {
      if (run.w.failed)
        printf("pass %d: %s\n", p, run.w.e.msg);
      if (run.d)
        ft_detail_abort(run.d);
      ring_close(run.r);
      free(run.cal);
      save_job(path, j);
      printf("pass %d: %s; its frames stay .partial, finished passes "
             "are marked done in %s\n",
             p, g_abort ? "interrupted" : "failed", path);
      return 1;
    }
    rc = ft_detail_close(run.d, &res, &e);
    ring_close(run.r);
    free(run.cal);
    for (i = 0; i < res.nframes; i++) {
      const ft_detail_frame_result *f = &res.f[i];
      printf("  %s: frame %d at %d,%d %dx%d px, shift %+.2f mm%s%s\n", f->path,
             f->index, f->box[0], f->box[1], f->box[2], f->box[3], f->shift_mm,
             f->estimated ? "  ESTIMATED: not found on the scan" : "",
             f->edge ? "  WARNING: at the file edge, frame may be "
                       "cut (margin too small)"
                     : "");
    }
    if (rc) {
      printf("pass %d: %s\n", p, e.msg);
      save_job(path, j);
      return 1;
    }
    for (i = 0; i < j->nframes; i++)
      if (j->f[i].include && j->f[i].pass == p)
        j->f[i].done = 1;
    if (save_job(path, j))
      return 1;
    for (i = 0; i < res.nframes; i++)
      pg_file(res.f[i].path);
  }
  if (g_abort) {
    save_job(path, j);
    printf("interrupted; finished passes are marked done in %s\n", path);
    return 1;
  }
  printf("\nall passes scanned\n");
  return 0;
}

/* A share job's window for group g: from its first frame - margin to its
 * last + margin along (ft_share_group_window, as the planner priced it)
 * and the whole reach across at the group's optics (detection in ftdev
 * cut needs the hole rows; the line rate does not depend on the width).
 * -1 if the group has no included frame. */
static int share_request(const ft_job *j, const ft_holder *h, int g,
                         struct crop_req *q) {
  const ft_job_frame *fr = NULL;
  ft_share_opts o;
  double y0, len, scale, lo, hi;
  int i;
  for (i = 0; i < j->nframes && !fr; i++)
    if (j->f[i].include && j->f[i].group == g)
      fr = &j->f[i];
  ft_share_opts_default(&o, h->scan_len_mm);
  if (!fr || ft_share_group_window(j, g, &o, &y0, &len))
    return -1;
  scale = fr->ppi * ft_job_bin(fr);
  lo = ft_ccd_to_mm(FT_CCD_FIRST, scale);
  hi = ft_ccd_to_mm(FT_CCD_FIRST + FT_CCD_PIXELS, scale);
  if (!(lo > 0))
    lo = 0;
  if (!(hi < CROP_X_MAX))
    hi = CROP_X_MAX;
  if (hi > h->scan_width_mm)
    hi = h->scan_width_mm;
  memset(q, 0, sizeof *q);
  q->ppi = fr->ppi;
  q->bin = ft_job_bin(fr);
  q->x = lo + JOB_EDGE;
  q->w = hi - JOB_EDGE - q->x;
  q->y = y0;
  q->h = len;
  q->ccd = q->optic = -1;
  q->focus_offset = j->focus_offset;
  q->hold = 3000; /* cmd_scan: >= g_first_hold first */
  return 0;
}

#define SHARE_FOCUS_TRIES 2 /* frames of a window tried for a peak */

/* The 95 % depth of field in tandem steps at an optical scale and bin
 * (ft_lens_dof95: ~980 (1+m)/m^2 x bin, focus survey 2026-10-15/16,
 * RESULTS.md 4: 636 at 6300, 3651 at 2000, 5653 at 1500, bin 1). */
static double focus_dof95(double scale, int bin) {
  return ft_lens_dof95(&g_lens, scale, bin);
}

/* A share bracket's step: half the depth of field (5 crops span +-1
 * DOF95, so the peak stands out of the 3 % flat cut-off), never under
 * the 500 the high-magnification brackets use. ~2200 at 1705 bin 1. */
static long share_focus_step(double scale, int bin) {
  long st = (long)(focus_dof95(scale, bin) / 2 / 50 + 0.5) * 50;
  return st > 500 ? st : 500;
}

/* A share window's focus: a bracket of n crops at the window's own
 * optics (q: its ppi, bin and reach across), share_focus_step apart, on
 * the centre band of its first included frame, the next one if that
 * gives no peak, centred on centre. 0 with *off, FB_NO_PEAK (no frame
 * gave one), -1 on a scan or file failure. */
static int share_focus(const ft_job *j, int g, const struct crop_req *q,
                       long centre, int n, long *off) {
  double a = q->x + FOCUS_X_MARGIN, b = q->x + q->w - FOCUS_X_MARGIN;
  long step = share_focus_step(q->ppi * q->bin, q->bin);
  int i, tried = 0, rc = FB_NO_PEAK, keep = g_focus_bin;
  g_focus_bin = q->bin;
  for (i = 0; i < j->nframes && tried < SHARE_FOCUS_TRIES; i++) {
    const ft_job_frame *fr = &j->f[i];
    double lo, hi;
    if (!fr->include || fr->group != g)
      continue;
    lo = fr->x0 > a ? fr->x0 : a;
    hi = fr->x0 + fr->width < b ? fr->x0 + fr->width : b;
    if (!(hi - lo >= FOCUS_W_MIN)) {
      printf("frame %d: outside the area a focus crop can scan\n", fr->index);
      continue;
    }
    tried++;
    printf("focus, window %d: frame %d at %.1f mm (%.1f..%.1f mm "
           "across), %g ppi, bin %d, %d crops %ld apart\n",
           g, fr->index, fr->y0 + fr->len / 2, lo, hi, q->ppi, q->bin, n, step);
    g_pg_frame = fr->index;
    rc = focus_bracket(q->ppi, (lo + hi) / 2, fr->y0 + fr->len / 2, hi - lo,
                       centre, step, n, FB_FRAME_SAMPLES, 0, off);
    g_pg_frame = 0;
    if (rc != FB_NO_PEAK)
      break;
  }
  g_focus_bin = keep;
  return rc;
}

static int group_todo(const ft_job *j, int g) {
  int i;
  for (i = 0; i < j->nframes; i++)
    if (j->f[i].include && j->f[i].group == g && !j->f[i].done)
      return 1;
  return 0;
}

/* scan --job of a share job: one scan per group not yet done, every
 * window checked (crop_check) before the scanner does anything. Output
 * <jobbase>_winNN.raw; its .txt gets the share keys (ft_share_cut.h) for
 * ftdev cut. A window's frames are marked done together.
 * Focus (focus = per-frame): a tandem offset moves the film-side focus
 * plane, the same at any scale, so one measured window serves the rest:
 * a full bracket until one finds a peak (centred on any offset the job
 * kept, e.g. from a stopped run), then the curve with that offset and no
 * crops, except a 3-crop check where the window's scale is above
 * FT_SHARE_FOCUS_RECHECK x the measured one (less depth of field). */
static int scan_share(ft_job *j, const char *path, const char *jobbase,
                      const ft_holder *h) {
  char ref[FT_MAX_FRAMES + 1];
  int g, ng = 0, i, nref = 0, ntodo = 0, nscan = 0;
  long centre = j->focus_offset; /* the next bracket's centre */
  double mscale = 0;             /* scale measured at; 0: none yet */

  for (i = 0; i < j->nframes; i++)
    if (j->f[i].include && j->f[i].group > ng)
      ng = j->f[i].group;
  memset(ref, 0, sizeof ref);
  for (g = 1; g <= ng; g++) {
    struct crop_req q;
    long ccd, optic;
    if (!group_todo(j, g))
      continue;
    if (share_request(j, h, g, &q) || crop_check(&q, &ccd, &optic)) {
      printf("window %d: REFUSED (reason above); it will be skipped\n", g);
      ref[g] = 1;
      nref++;
      g_pg_refused = 1;
    } else {
      ntodo++;
    }
  }
  for (i = 0; i < j->nframes; i++) /* an offset a stopped run kept */
    if (j->f[i].include && j->f[i].focus_offset)
      centre = j->f[i].focus_offset;
  printf("%s: share job, %d window(s) to scan\n", path, ntodo);
  pg_begin(FT_PG_WINDOW, ntodo);
  g_lamp = 255; /* the lamp level persists: set it */
  for (g = 1; g <= ng && !g_abort; g++) {
    struct crop_req q;
    char out[300], meta[310];
    FILE *m;
    int rc;
    if (!group_todo(j, g) || ref[g])
      continue;
    share_request(j, h, g, &q);
    snprintf(out, sizeof out, "%s_win%02d.raw", jobbase, g);
    q.out = out;
    pg_unit(FT_PG_WINDOW, nscan + 1, g);
    if (j->focus == FT_FOCUS_PER_FRAME) {
      double scale = q.ppi * q.bin;
      long off = 0;
      ft_pg_how how;
      if (mscale > 0 && scale <= mscale * FT_SHARE_FOCUS_RECHECK) {
        printf("\n=== window %d: focus offset %ld carried (measured "
               "at scale %g, this is %g): no crops\n",
               g, centre, mscale, scale);
        rc = 0;
        off = centre;
        how = FT_PG_CARRIED;
      } else {
        int check = mscale > 0;
        if (check)
          printf("\n=== window %d: focus check, scale %g above "
                 "%g x the measured %g\n",
                 g, scale, FT_SHARE_FOCUS_RECHECK, mscale);
        else
          printf("\n=== window %d: focus\n", g);
        rc = share_focus(j, g, &q, centre, check ? 3 : FOCUS_N, &off);
        if (rc < 0) {
          save_job(path, j);
          printf("%s; stopped (finished windows are marked done "
                 "in %s)\n",
                 g_abort ? "interrupted" : "focus crop failed", path);
          return 1;
        }
        how = check ? FT_PG_CHECKED : FT_PG_MEASURED;
        if (rc == FB_NO_PEAK && check) {
          printf("window %d: no peak in the check; offset %ld "
                 "carried\n",
                 g, centre);
          off = centre;
          how = FT_PG_CARRIED;
        } else if (rc == FB_NO_PEAK) {
          printf("WARNING: window %d: no usable focus peak; using "
                 "offset %ld\n",
                 g, centre);
          off = centre;
          how = FT_PG_NONE;
        } else {
          mscale = scale;
        }
      }
      centre = off;
      if (how != FT_PG_NONE)
        for (i = 0; i < j->nframes; i++)
          if (j->f[i].include && j->f[i].group == g)
            j->f[i].focus_offset = off;
      pg_focused(off, how);
      q.focus_offset = off;
    }
    printf("\n=== window %d: %.1f..%.1f mm, %g ppi, bin %d (~%.0f s), "
           "focus offset %ld\n",
           g, q.y, q.y + q.h, q.ppi, q.bin, ft_scan_seconds(q.ppi, q.bin, q.h),
           q.focus_offset);
    g_pg_track = 1;
    rc = crop_scan(&q);
    g_pg_track = 0;
    if (rc == CROP_REFUSED) {
      printf("window %d: REFUSED (reason above); not scanned\n", g);
      ref[g] = 1;
      nref++;
      g_pg_refused = 1;
      continue;
    }
    if (rc) {
      save_job(path, j);
      printf("window %d: %s; stopped (finished windows are marked done "
             "in %s)\n",
             g, g_abort ? "interrupted" : "scan failed", path);
      return 1;
    }
    nscan++;
    snprintf(meta, sizeof meta, "%s.txt", out);
    m = fopen(meta, "a");
    if (m) {
      int bad = ft_share_meta_write(m, j, g) != 0;
      if (fclose(m) != 0 || bad)
        m = NULL;
    }
    if (!m)
      fprintf(stderr,
              "warning: cannot append the share keys to %s; "
              "ftdev cut will refuse it\n",
              meta);
    for (i = 0; i < j->nframes; i++)
      if (j->f[i].include && j->f[i].group == g)
        j->f[i].done = 1;
    if (save_job(path, j))
      return 1;
    if (m) /* the window, its share keys too */
      pg_file(out);
    printf("next: ftdev cut %s\n", out);
  }
  for (g = 1, i = 0; g <= ng; g++)
    i += group_todo(j, g) && !ref[g];
  if (g_abort && i) {
    save_job(path, j);
    printf("interrupted; finished windows are marked done in %s\n", path);
    return 1;
  }
  if (nref) {
    printf("\n%d window(s) scanned; REFUSED:", nscan);
    for (g = 1; g <= ng; g++)
      if (ref[g])
        printf(" %d", g);
    printf(" (fix them in %s and run again)\n", path);
    return 1;
  }
  printf(nscan ? "\nall windows scanned\n"
               : "\nnothing to scan (every "
                 "window is done)\n");
  return 0;
}

/* scan --job <file> [-h s]: scan every included frame not yet done.
 * The job is validated and every frame's scan window checked (crop_check)
 * before the scanner does anything; refused frames are listed, skipped,
 * left not done, and make the exit status non-zero. Per-frame focus: a
 * bracket on the frame's centre band first, centred on its focus_offset
 * (the strip's when 0); without a usable peak that centre is used. A scan
 * failure or Ctrl+C saves the job and stops; finished frames are marked
 * done, so a re-run resumes. */
static int cmd_scanjob(int argc, char **argv) {
  static ft_job j;
  const ft_holder *h;
  const char *path, *dot, *sep;
  char jobbase[240]; /* + _frameNN.raw.faul < 260 */
  ft_err e;
  FILE *f;
  char ref[FT_MAX_FRAMES];
  int i, nscan = 0, nref = 0, ntodo = 0, left;

  if (argc < 1) {
    fprintf(stderr, "scan --job <file.job> [-h s]\n");
    return 2;
  }
  path = argv[0];
  for (i = 1; i < argc; i += 2) {
    if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
      if (parse_hold(argv[i + 1], &g_first_hold))
        return 2;
    } else {
      fprintf(stderr, "scan --job <file.job> [-h s]\n");
      return 2;
    }
  }
  /* outputs are <jobbase>_frameNN.raw: the job path without its
   * extension, so scans of different jobs never overwrite each other */
  dot = strrchr(path, '.');
  sep = strrchr(path, '/');
  if (!sep || (strrchr(path, '\\') && strrchr(path, '\\') > sep))
    sep = strrchr(path, '\\');
  if (!dot || (sep && dot < sep) || dot == (sep ? sep + 1 : path))
    dot = path + strlen(path);
  if ((size_t)(dot - path) + sizeof "_frame00.raw" > sizeof jobbase) {
    fprintf(stderr, "scan --job: job path too long\n");
    return 2;
  }
  memcpy(jobbase, path, (size_t)(dot - path));
  jobbase[dot - path] = 0;
  f = fopen(path, "r");
  if (!f) {
    perror(path);
    return 1;
  }
  if (ft_job_read(&j, f, &e)) {
    fclose(f);
    fprintf(stderr, "%s: %s\n", path, e.msg);
    return 1;
  }
  fclose(f);
  if (ft_job_validate(&j, &g_lens, &e)) {
    fprintf(stderr, "%s: %s\n", path, e.msg);
    return 1;
  }
  h = ft_holder_find(j.holder);
  if (!h)
    return 1; /* validate checked it */
  if (j.detail != FT_DETAIL_NONE)
    return scan_detail(&j, path, jobbase, h);
  if (j.share > 0)
    return scan_share(&j, path, jobbase, h);
  /* every window through crop_scan's checks before any scanner action */
  memset(ref, 0, sizeof ref);
  for (i = 0; i < j.nframes; i++) {
    const ft_job_frame *fr = &j.f[i];
    struct crop_req q;
    long ccd, optic;
    if (!fr->include || fr->done)
      continue;
    job_window(fr, h,
               j.focus == FT_FOCUS_PER_FRAME ? frame_focus_base(&j, fr)
                                             : j.focus_offset,
               &q);
    if (crop_check(&q, &ccd, &optic)) {
      printf("frame %d: REFUSED (reason above); it will be skipped\n",
             fr->index);
      ref[i] = 1;
      nref++;
      g_pg_refused = 1;
    } else {
      ntodo++;
    }
  }
  pg_begin(FT_PG_FRAME, ntodo);
  printf("%s: %d frame(s) to scan%s", path, ntodo, nref ? ", refused:" : "\n");
  for (i = 0; nref && i < j.nframes; i++)
    if (ref[i])
      printf(" %d", j.f[i].index);
  if (nref)
    printf("\n");

  g_lamp = 255; /* the lamp level persists: set it */
  for (i = 0; i < j.nframes && !g_abort; i++) {
    ft_job_frame *fr = &j.f[i];
    struct crop_req q;
    char out[300];
    long off = j.focus_offset;
    int rc;

    if (!fr->include || fr->done || ref[i])
      continue;
    pg_unit(FT_PG_FRAME, nscan + 1, fr->index);
    if (j.focus == FT_FOCUS_PER_FRAME) {
      long base = frame_focus_base(&j, fr);
      double lo, hi;
      printf("\n=== frame %d: focus\n", fr->index);
      if (focus_span(fr->ppi, fr->x0, fr->x0 + fr->width, &lo, &hi)) {
        printf("frame %d: outside the area a focus crop can scan\n", fr->index);
        rc = FB_NO_PEAK;
      } else {
        g_pg_frame = fr->index;
        rc =
            focus_bracket(fr->ppi, (lo + hi) / 2, fr->y0 + fr->len / 2, hi - lo,
                          base, 500, FOCUS_N, FB_FRAME_SAMPLES, 0, &off);
        g_pg_frame = 0;
      }
      if (rc < 0) {
        save_job(path, &j);
        printf("%s; stopped (finished frames are marked done in "
               "%s)\n",
               g_abort ? "interrupted" : "focus crop failed", path);
        return 1;
      }
      if (rc == FB_NO_PEAK) {
        printf("WARNING: frame %d: no usable focus peak; using "
               "offset %ld\n",
               fr->index, base);
        off = base;
        pg_focused(off, FT_PG_NONE);
      } else {
        fr->focus_offset = off;
        pg_focused(off, FT_PG_MEASURED);
      }
    }
    job_window(fr, h, off, &q);
    snprintf(out, sizeof out, "%s_frame%02d.raw", jobbase, fr->index);
    q.out = out;
    printf("\n=== frame %d: %s, %g ppi, bin %d (~%.0f s), focus offset "
           "%ld\n",
           fr->index, type_name(fr->type), fr->ppi, ft_job_bin(fr),
           ft_scan_seconds(fr->ppi, ft_job_bin(fr), q.h), off);
    g_pg_track = 1;
    rc = crop_scan(&q);
    g_pg_track = 0;
    if (rc == CROP_REFUSED) {
      printf("frame %d: REFUSED (reason above); not scanned, left "
             "not done\n",
             fr->index);
      ref[i] = 1;
      nref++;
      g_pg_refused = 1;
      continue;
    }
    if (rc) {
      save_job(path, &j);
      printf("frame %d: %s; stopped (finished frames are marked done "
             "in %s)\n",
             fr->index, g_abort ? "interrupted" : "scan failed", path);
      return 1;
    }
    nscan++;
    write_crop(out, &j, fr, off);
    fr->done = 1;
    if (save_job(path, &j))
      return 1;
    pg_file(out);
  }
  for (i = left = 0; i < j.nframes; i++)
    left += j.f[i].include && !j.f[i].done && !ref[i];
  if (g_abort && left) {
    save_job(path, &j);
    printf("interrupted; finished frames are marked done in %s\n", path);
    return 1;
  }
  if (nref) {
    printf("\n%d frame(s) scanned; REFUSED:", nscan);
    for (i = 0; i < j.nframes; i++)
      if (ref[i])
        printf(" %d", j.f[i].index);
    printf(" (fix them in %s and run again)\n", path);
    return 1;
  }
  printf(nscan ? "\nall frames scanned\n"
               : "\nnothing to scan (every "
                 "included frame is done)\n");
  return 0;
}

#define CAL_SCALES 8
#define CAL_MIN 6         /* usable scales needed for a fit */
#define CAL_RMS_MAX 600.0 /* CCD steps */
#define CAL_LENS_RMS_MAX 300.0

/* in-focus CCD/lens at magnification M on curve l (ft_focus.h model) */
static void lens_at(const ft_lens *l, double M, double *c, double *o) {
  double m = l->p * M;
  double u = l->fl * (1 + 1 / m), v = l->fl * (1 + m);
  *o = l->film - u;
  *c = *o + l->w - v;
}

/* calibrate-focus <x> <y> <w> [-h s]: fit this scanner's focus curve on a
 * textured window (the 1 mm band at y across x..x+w mm, clipped to the
 * CCD reach per scale). At eight optical scales, the CCD is bracketed
 * with the lens on the current curve: the first usable scale gets a
 * coarse bracket (5 crops, 3000 steps, moving up to +-FT_CAL_BRACKET_MAX),
 * then every scale a fine one (9 crops, 1000 steps) around the coarse
 * peak or the previous scale's. The magnification at the peak is exact
 * for the current curve (the CCD moving by d shortens v by d); thin-lens
 * least squares -> focus.txt in the current directory. focus.txt is only
 * written with >= CAL_MIN usable scales, rms <= CAL_RMS_MAX CCD and
 * CAL_LENS_RMS_MAX lens, every point within FT_CAL_MARGIN of the fit,
 * and every measured scale inside FlexColor's positions. */
static int cmd_calibrate_focus(int argc, char **argv) {
  static const double PPI[CAL_SCALES] = {3800, 4000, 1075, 2400,
                                         2700, 2850, 2000, 3150};
  double x, y, w, M[CAL_SCALES], S[CAL_SCALES], rc, rl;
  long C[CAL_SCALES], L[CAL_SCALES], prev = 0;
  ft_lens fit = g_lens;
  char *e1, *e2, *e3;
  FILE *fo;
  int i, n = 0, wr, have_prev = 0;

  if (argc < 3) {
    fprintf(stderr, "calibrate-focus <x_mm> <y_mm> <w_mm> [-h s]  (a "
                    "textured part of a frame, e.g. 13 130.4 7.6)\n");
    return 2;
  }
  x = strtod(argv[0], &e1);
  y = strtod(argv[1], &e2);
  w = strtod(argv[2], &e3);
  if (*e1 || *e2 || *e3 || e1 == argv[0] || e2 == argv[1] || e3 == argv[2] ||
      !isfinite(x) || !isfinite(y) || !(w > 0) || !isfinite(w)) {
    fprintf(stderr, "calibrate-focus: x y w must be numbers, w > 0\n");
    return 2;
  }
  for (i = 3; i < argc; i += 2) {
    if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
      if (parse_hold(argv[i + 1], &g_first_hold))
        return 2;
    } else {
      fprintf(stderr, "calibrate-focus: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  g_lamp = 255; /* the lamp level persists: set it */
  printf("calibrate-focus: %d scales, ~%d crops (~%d min)\n", CAL_SCALES,
         5 + CAL_SCALES * 9, ((5 + CAL_SCALES * 9) * CROP_S + 59) / 60);
  for (i = 0; i < CAL_SCALES && !g_abort; i++) {
    double scale = PPI[i] * ft_bin_for_ppi(PPI[i]), v, lo, hi;
    long ccd, optic, best = 0, centre = prev;
    int r;
    printf("\n=== scale %.0f (%g ppi)\n", scale, PPI[i]);
    if (ft_optics_for(&g_lens, scale, &ccd, &optic)) {
      printf("WARNING: no focus pair for scale %.0f; skipped\n", scale);
      continue;
    }
    if (focus_span(PPI[i], x, x + w, &lo, &hi)) {
      printf("WARNING: %.1f..%.1f mm is outside what a crop at scale "
             "%.0f can scan; skipped\n",
             x, x + w, scale);
      continue;
    }
    r = 0;
    if (!have_prev) {
      printf("coarse bracket\n");
      r = focus_bracket(PPI[i], (lo + hi) / 2, y, hi - lo, 0, 3000, 5,
                        FB_FRAME_SAMPLES, 1, &centre);
      if (r == FB_NO_PEAK) {
        printf("WARNING: no usable coarse peak at scale %.0f; "
               "skipped\n",
               scale);
        continue;
      }
      if (r == 0)
        printf("fine bracket around %+ld\n", centre);
    }
    if (r == 0)
      r = focus_bracket(PPI[i], (lo + hi) / 2, y, hi - lo, centre, 1000, 9,
                        FB_SAMPLES, 1, &best);
    if (r < 0) {
      printf("%s; focus.txt not written\n",
             g_abort ? "interrupted" : "focus crop failed");
      return 1;
    }
    if (r == FB_NO_PEAK) {
      printf("WARNING: no usable peak at scale %.0f; skipped\n", scale);
      continue;
    }
    v = g_lens.fl * (1 + g_lens.p * scale);
    S[n] = scale;
    M[n] = ((v - best) / g_lens.fl - 1) / g_lens.p;
    C[n] = ccd + best;
    L[n] = optic;
    printf("peak: CCD %ld (curve %+ld), lens %ld, magnification %.0f\n", C[n],
           best, L[n], M[n]);
    prev = best;
    have_prev = 1;
    n++;
  }
  if (g_abort) {
    printf("interrupted; focus.txt not written\n");
    return 1;
  }
  if (n < CAL_MIN) {
    fprintf(stderr,
            "calibrate-focus: %d usable scales (need %d); "
            "focus.txt not written\n",
            n, CAL_MIN);
    return 1;
  }
  if (ft_lens_fit(M, C, L, n, &fit, &rc, &rl)) {
    fprintf(stderr, "calibrate-focus: fit failed; focus.txt not written\n");
    return 1;
  }
  printf("\nfit (%d scales): film %.0f w %.0f fl %.0f p %.4e; rms %.0f CCD "
         "/ %.0f lens\n",
         n, fit.film, fit.w, fit.fl, fit.p, rc, rl);
  if (!(rc <= CAL_RMS_MAX) || !(rl <= CAL_LENS_RMS_MAX)) {
    fprintf(stderr,
            "calibrate-focus: residual over %.0f CCD / %.0f lens "
            "steps; focus.txt not written\n",
            CAL_RMS_MAX, CAL_LENS_RMS_MAX);
    return 1;
  }
  for (i = 0; i < n; i++) {
    long pc, po;
    double fc, foo;
    lens_at(&fit, M[i], &fc, &foo);
    printf("  scale %.0f: measured CCD %ld lens %ld, fit %.0f / %.0f\n", S[i],
           C[i], L[i], fc, foo);
    if (!(fabs(fc - C[i]) <= FT_CAL_MARGIN)) {
      fprintf(stderr,
              "calibrate-focus: scale %.0f is %.0f CCD steps "
              "off the fit (> %ld); focus.txt not written\n",
              S[i], fc - C[i], FT_CAL_MARGIN);
      return 1;
    }
    if (ft_optics_for(&fit, S[i], &pc, &po) || !ft_positions_ok(pc, po)) {
      fprintf(stderr,
              "calibrate-focus: the fitted curve puts scale "
              "%.0f outside FlexColor's positions; focus.txt "
              "not written\n",
              S[i]);
      return 1;
    }
  }
  fo = fopen("focus.txt", "w");
  wr = !fo || ft_lens_write(&fit, fo) != 0;
  if (fo)
    wr |= fclose(fo) != 0;
  if (wr) {
    fprintf(stderr, "cannot write focus.txt\n");
    return 1;
  }
  printf("wrote focus.txt (current directory)\n");
  return 0;
}

static int scan_args(int argc, char **argv) {
  const struct preset *p = &PRESETS[0];
  const ft_preview *pm = NULL;
  long keep = 0, y = -1;
  int frame = -1, i, rc = 0;
  DWORD hold = 10000;
  const char *out = "scan.raw";
  char name[32];

  if (argc >= 1 && strcmp(argv[0], "--job") == 0)
    return cmd_scanjob(argc - 1, argv + 1);
  for (i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] && !a[2] && i + 1 < argc) {
      const char *v = argv[++i];
      switch (a[1]) {
      case 'n':
        keep = atol(v);
        break;
      case 'y':
        y = atol(v);
        break;
      case 'f':
        frame = atoi(v);
        break;
      case 'h':
        if (parse_hold(v, &hold))
          return 2;
        break;
      case 'o':
        out = v;
        break;
      case 'p':
        if (parse_pos(v))
          return 2;
        break;
      case 'l':
        if (parse_lamp(v))
          return 2;
        break;
      default:
        fprintf(stderr, "scan: unknown option %s\n", a);
        return 2;
      }
    } else {
      size_t k;
      if (strcmp(a, "preview1000") == 0 || strcmp(a, "preview1500") == 0) {
        pm = ft_preview_find(a + 7);
        continue;
      }
      for (k = 0; k < sizeof PRESETS / sizeof PRESETS[0]; k++)
        if (strcmp(a, PRESETS[k].name) == 0)
          break;
      if (k == sizeof PRESETS / sizeof PRESETS[0]) {
        fprintf(stderr, "scan: unknown preset %s\n", a);
        return 2;
      }
      p = &PRESETS[k];
    }
  }
  if (pm) {
    if (y >= 0 || frame >= 0 || g_ccd >= 0) {
      fprintf(stderr,
              "scan %s: -y, -f and -p do not apply (whole "
              "holder, optics from the focus curve)\n",
              pm->name);
      return 2;
    }
    return preview_scan(pm, keep, hold, out) ? 1 : 0;
  }
  if (frame >= 0 && strcmp(p->name, "3f") != 0) {
    fprintf(stderr, "scan: -f only applies to the 3f preset\n");
    return 2;
  }
  if (frame > 6) {
    fprintf(stderr, "scan: frame 1-6 (0 = all)\n");
    return 2;
  }
  if (frame > 0)
    return cmd_scan(p, F3_Y[frame - 1], keep, hold, out);
  if (frame == 0) {
    /* FlexColor's 3f strip: one full scan cycle per frame */
    for (i = 0; i < 6 && rc == 0; i++) {
      sprintf(name, "frame%d.raw", i + 1);
      printf("\n=== frame %d/6\n", i + 1);
      rc = cmd_scan(p, F3_Y[i], keep, hold, name);
      hold = 2500; /* lamp warm after the first frame */
    }
    return rc;
  }
  return cmd_scan(p, y, keep, hold, out);
}

int main(int argc, char **argv) {
  const char *cmd;
  int rc = 0, i, k;

  /* --progress, --microcode PATH and --device PATH anywhere: taken out
   * before the commands parse argv */
  for (i = k = 1; i < argc; i++)
    if (strcmp(argv[i], "--progress") == 0)
      g_progress = 1;
    else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
      if (parse_probe(argv[++i])) {
        fprintf(stderr, "ftpoc: --probe L[:igp][,L...]\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--stall") == 0 && i + 1 < argc) {
      unsigned long ms;
      if (sscanf(argv[++i], "%ld:%lu", &g_stall_line, &ms) != 2) {
        fprintf(stderr, "ftpoc: --stall L:ms\n");
        return 2;
      }
      g_stall_ms = (DWORD)ms;
    } else if (strcmp(argv[i], "--microcode") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ftpoc: --microcode needs a path\n");
        return 2;
      }
      g_microcode = argv[++i];
    } else if (strcmp(argv[i], "--device") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ftpoc: --device needs a path\n");
        return 2;
      }
      io_set_device(argv[++i]);
    } else
      argv[k++] = argv[i];
  argc = k;
  argv[argc] = NULL;
  load_lens();
  if (argc < 2) {
    fprintf(
        stderr,
        "usage: ftpoc <command> [args]\n"
        "  inquiry            identity (run first)\n"
        "  poll [n]           n ScannerReady polls (default 10)\n"
        "  win                one SetWindow exchange (idle template), hexdump\n"
        "  getwin             GET WINDOW: live CCD/lens positions (@56/@60)\n"
        "  park               rest position (FlexColor's shutdown descriptor)\n"
        "  goto <ppi*10000>   idle template with @10 set, 0..63000000\n"
        "  move <+/-d> [n]    n relative @10 steps of d (max 5000000, 50 "
        "steps)\n"
        "  stop               end-of-scan descriptor x2 (FlexColor end/Stop)\n"
        "  pages              read the standard ReadBuffer pages\n"
        "  grab <n> [len]     n ReadExt blocks (default len 3996) -> grab.raw\n"
        "  scan [preset] [-n lines] [-y line] [-f frame] [-h hold_s] [-o "
        "file]\n"
        "                     presets: preview (525 ppi), 2000, 1000, 3f (630 "
        "ppi);\n"
        "                     preview1000, preview1500: whole-holder previews "
        "at\n"
        "                     the 6000 crop optics (-n, -h, -l, -o only)\n"
        "                     -n stop after n lines (default: the whole "
        "window)\n"
        "                     -y start line along the strip (holder-clamped)\n"
        "                     -f 3f frame 1-6, or 0 = whole strip "
        "(frameN.raw)\n"
        "                     -h hold before calibration, 0..120 s (default "
        "10; 35 cold)\n"
        "                     -p <ccd>,<lens> positions @56/@60 (clamped to\n"
        "                        FlexColor's values, lens - CCD >= 150000)\n"
        "                     -l <0..255> lamp level @76 (scans send 0)\n"
        "                     -> scan.raw (+ .cal 250 calibration lines, "
        ".txt)\n"
        "  crop <ppi> <x> <y> <w> <h> [-b bin] [-n lines] [-h s] [-p "
        "ccd,lens]\n"
        "       [-l lamp] [-o file]\n"
        "                     SYNTHESIZED window in mm at any ppi (525..6300)\n"
        "                     -b CCD px per output px (default the fastest);\n"
        "                        smaller = lower magnification, more light\n"
        "                     -> crop.raw (+ .cal .faul .txt)\n"
        "  frames [--film neg|slide] [--ppi n] [--sprockets yes|no]\n"
        "         [--preview standard|1000|1500] [-h s] [-o strip]\n"
        "                     preview (lamp 255), detect frames, 3-point "
        "focus\n"
        "                     check -> strip.job (edit it) + strip.png;\n"
        "                     fb.raw* are scratch focus crops\n"
        "                     -h first-scan hold, 0..120 s (default 10; 35 "
        "cold)\n"
        "                     --preview 525 ppi (standard, ~40 s), 1000 (~2.7\n"
        "                       min) or 1500 (~5.7 min); strip.raw keeps it\n"
        "                     prints per-frame optics options (bin: time,\n"
        "                       light, Dmax); pick one with the job's bin key\n"
        "  frames --detail neg|pos --ppi 3150|6300 [--select 1,3-6]\n"
        "         [--fmt dng|tiff] [--bw off|mono|rgb] [--keep-raw yes|no]\n"
        "         [--ring-mb N] [--film neg|slide] [-h s] [-o strip]\n"
        "                     detailed scan: preview at the scan's optics,\n"
        "                     focus per selected frame (bin 1), passes "
        "grouped\n"
        "                     by depth of focus -> strip.job; scan --job then\n"
        "                     writes strip_frameNN.dng (neg: digital "
        "negative)\n"
        "                     or .tif (pos: archive positive, density) while\n"
        "                     the scan runs (ring of --ring-mb, default 64)\n"
        "         [--share px [--select 1,3-6] [--hf-half-res on|off]]\n"
        "                     scan to share: px on each frame's long side\n"
        "                       (2000); the selected frames (default all\n"
        "                       but partial) grouped into windows at the\n"
        "                       optics with the most light; no focus check\n"
        "  scan --job strip.job [-h s]  scan the job's included frames\n"
        "                     (resumable; strip_frameNN.raw + .txt with the "
        "tight\n"
        "                     crop, named after the job file); a share job\n"
        "                     scans windows: strip_winNN.raw, then ftdev cut\n"
        "                     -h first-scan hold, 0..120 s (default 10; 35 "
        "cold)\n"
        "  calibrate-focus <x> <y> <w> [-h s]  fit this scanner's focus curve\n"
        "                     on a textured window (mm) -> focus.txt\n"
        "                     -h first-scan hold, 0..120 s (default 10; 35 "
        "cold)\n"
        "  maint io <0x342|0x344> <byte> --confirm\n"
        "  maint mem 4000:<off> <hexbytes> --confirm   (4000:7640..76bb)\n"
        "  maint peek <3000|4000|c000|e000|f000>:<off> <len>   (len <= 0x400)\n"
        "  maint dump-flash <file>   E000..FFFF + loader C000..C7FF backup "
        "(reads)\n"
        "                     one whitelisted microcode maintenance write or\n"
        "                     memory read, idle only; everything else is\n"
        "                     refused (README: Maintenance writes)\n"
        "  upload [file]      MICROCOD.3XX upload (loader mode only!); file:\n"
        "                     as --microcode\n"
        "  --progress         (frames, scan) machine-readable progress lines\n"
        "                     for the web server: progress begin|unit|lines|\n"
        "                     file|end ... (libft/ft_progress.h)\n"
        "  --probe L[:igp],.. (scan, crop) at image line L: INQUIRY (i), GET\n"
        "                     WINDOW (g), peek drum 4000:8ed0 (p); default i\n"
        "  --stall L:ms       (scan, crop) stop reading for ms at image line "
        "L\n"
        "  --microcode PATH   the MICROCOD.3XX to upload (default: beside\n"
        "                     ftpoc.exe, then the current directory)\n"
        "  --device PATH      probe only this device (Linux: e.g. /dev/sg1;\n"
        "                     default: every " IO_DEV_NAME " whose vendor is\n"
        "                     Imacon)\n"
        "\n"
        "After a power-up the scanner answers as its loader (\"SCSI "
        "Loader\"):\n"
        "every command but inquiry and upload then uploads the microcode\n"
        "first, finds it again (it can move to another \\\\.\\ScannerN) and\n"
        "carries on.\n"
        "CLOSE FlexColor first. Motion is slow and clamped. The first scan of "
        "a\n"
        "run holds at least 10 s (35 s after an automatic microcode upload),\n"
        "except that frames, scan --job and calibrate-focus -h set that first\n"
        "hold (0..120 s). Ctrl+C: scans (scan,\n"
        "crop, frames, scan --job, calibrate-focus) stop at the next line "
        "read\n"
        "and send END; park and goto finish their current move (Ctrl+C is "
        "held\n"
        "until then); move stops before its next step. Nothing new starts.\n");
    return 2;
  }
  cmd = argv[1];

  io_init();

  if (strcmp(cmd, "inquiry") == 0 || strcmp(cmd, "devices") == 0) {
    g_dev = open_scanner();
    rc = cmd_inquiry();
  } else {
    /* every other command needs a live device */
    g_dev = open_scanner();
    /* commands that scan or move: Ctrl+C stops at the next safe point
     * (end handshake sent, no new scan or move) instead of killing
     * the process mid-scan */
    if (strcmp(cmd, "scan") == 0 || strcmp(cmd, "crop") == 0 ||
        strcmp(cmd, "frames") == 0 || strcmp(cmd, "calibrate-focus") == 0 ||
        strcmp(cmd, "park") == 0 || strcmp(cmd, "goto") == 0 ||
        strcmp(cmd, "move") == 0)
      io_catch_interrupt(&g_abort);

    if (0) {
    } else if (strcmp(cmd, "upload") != 0 &&
               ft_upload_is_loader(g_inq, (size_t)g_inq_n) &&
               (rc = auto_upload()) != 0) {
      /* still the loader: the command does not run */
    } else if (strcmp(cmd, "poll") == 0) {
      int n = argc > 2 ? atoi(argv[2]) : 10;
      rc = cmd_poll(n, n > 20);
    } else if (strcmp(cmd, "getwin") == 0) {
      rc = cmd_getwin();
    } else if (strcmp(cmd, "win") == 0) {
      rc = cmd_win();
    } else if (strcmp(cmd, "park") == 0) {
      rc = cmd_park();
    } else if (strcmp(cmd, "move") == 0 && argc > 2) {
      rc = cmd_move(atol(argv[2]), argc > 3 ? atoi(argv[3]) : 1);
    } else if (strcmp(cmd, "goto") == 0 && argc > 2) {
      rc = cmd_goto(atol(argv[2]));
    } else if (strcmp(cmd, "busreset") == 0) {
      rc = io_busreset();
    } else if (strcmp(cmd, "scanstop") == 0) {
      rc = cmd_scanstop();
    } else if (strcmp(cmd, "stop") == 0) {
      rc = cmd_stop();
    } else if (strcmp(cmd, "pages") == 0) {
      rc = read_pages(1);
    } else if (strcmp(cmd, "grab") == 0 && argc > 2) {
      rc = cmd_grab(atol(argv[2]), argc > 3 ? (WORD)atoi(argv[3]) : 3996,
                    "grab.raw");
    } else if (strcmp(cmd, "crop") == 0) {
      rc = cmd_crop(argc - 2, argv + 2);
    } else if (strcmp(cmd, "frames") == 0) {
      rc = cmd_frames(argc - 2, argv + 2);
    } else if (strcmp(cmd, "calibrate-focus") == 0) {
      rc = cmd_calibrate_focus(argc - 2, argv + 2);
    } else if (strcmp(cmd, "scan") == 0) {
      rc = scan_args(argc - 2, argv + 2);
    } else if (strcmp(cmd, "maint") == 0) {
      rc = cmd_maint(argc - 2, argv + 2);
    } else if (strcmp(cmd, "upload") == 0) {
      rc = cmd_upload(argc > 2 ? argv[2] : g_microcode);
    } else {
      fprintf(stderr,
              "ftpoc: unknown command '%s' (try without args "
              "for usage)\n",
              cmd);
      rc = 2;
    }
  }

  if (strcmp(cmd, "frames") == 0 || strcmp(cmd, "scan") == 0)
    pg_end(rc == 0        ? FT_PG_OK
           : g_abort      ? FT_PG_STOPPED
           : g_pg_refused ? FT_PG_REFUSED
                          : FT_PG_FAILED);
  if (g_dev != IO_NONE)
    io_close(g_dev);
  return rc;
}
