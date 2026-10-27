/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftio_win.c — ftpoc's OS layer on Windows (ftio.h).
 *
 * Talks to the scanner directly through the Microsoft scsiscan.sys driver,
 * replicating the exact 44-byte SRB + ioctl 0x190012 transport that
 * FlexColor.exe uses (validated against 67k+ hooked DeviceIoControl calls,
 * sessions 2-6). Moved here unchanged from ftpoc.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftio.h"

#define IOCTL_SRB 0x190012

io_dev g_dev = INVALID_HANDLE_VALUE;
static HANDLE g_evt = NULL;
static const char* g_device; /* --device */

/* SRB status byte and 14-byte sense buffer of the last -3 failure
 * (SCSISCAN_CMD pSrbStatus @0x24 / pSenseBuffer @0x28). ERROR_IO_DEVICE
 * (1117) is a CHECK CONDITION; the sense data says why. */
static BYTE g_srb_status;
static BYTE g_sense[14];

static void die(const char* what) {
  fprintf(stderr, "ftpoc: %s failed, GetLastError=%lu\n", what, GetLastError());
  exit(1);
}

void io_init(void) {
  g_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
  if(!g_evt) die("CreateEvent");
}

void io_set_device(const char* path) { g_device = path; }

/* \\.\Scanner0..15, as FlexColor's FindDevices */
int io_dev_path(int i, char* path, size_t cap) {
  if(g_device) {
    if(i > 0 || strlen(g_device) >= cap) return 0;
    strcpy(path, g_device);
    return 1;
  }
  if(i >= 16 || cap < 24) return 0;
  sprintf(path, "\\\\.\\Scanner%d", i);
  return 1;
}

io_dev io_open(const char* path) {
  return CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, NULL);
}

void io_close(io_dev d) { CloseHandle(d); }

/* Send one 44-byte SRB; SCSI payload rides lpOutBuffer (METHOD_OUT_DIRECT).
 * Returns bytes transferred, or a negative code:
 *  -1 DeviceIoControl rejected, -2 timed out (cancelled), -3 completion error
 */
int srb_exec(const BYTE* cdb, int cdb_len, BYTE dir, BYTE* data, DWORD data_len,
             DWORD timeout_ms) {
  BYTE srb[0x70];
  OVERLAPPED ov;
  DWORD transferred = 0;

  memset(srb, 0, sizeof srb);
  *(DWORD*)(srb + 0x04) = 44;       /* SRB header length      */
  srb[0x08] = dir;                  /* 0x40 in / 0x80 out     */
  srb[0x0c] = (BYTE)cdb_len;        /* 6 or 10 on the wire    */
  srb[0x0d] = 0x0e;                 /* constant in all traces */
  *(DWORD*)(srb + 0x10) = data_len; /* payload length (LE)    */
  memcpy(srb + 0x14, cdb, cdb_len); /* 16-byte CDB slot       */
  *(DWORD*)(srb + 0x24) = (DWORD)(ULONG_PTR)(srb + 0x2c); /* postproc ptr  */
  *(DWORD*)(srb + 0x28) = (DWORD)(ULONG_PTR)(srb + 0x34); /* sense ptr     */
  *(DWORD*)(srb + 0x30) = (DWORD)(ULONG_PTR)data;         /* data ptr      */
  *(DWORD*)(srb + 0x68) = (DWORD)(ULONG_PTR)g_evt;        /* event (async) */

  memset(&ov, 0, sizeof ov);
  ov.hEvent = g_evt;
  ResetEvent(g_evt);

  if(!DeviceIoControl(g_dev, IOCTL_SRB, srb, 44, data, data_len, &transferred,
                      &ov)) {
    if(GetLastError() != ERROR_IO_PENDING) return -1;
    if(WaitForSingleObject(g_evt, timeout_ms) != WAIT_OBJECT_0) {
      CancelIo(g_dev);
      return -2;
    }
    if(!GetOverlappedResult(g_dev, &ov, &transferred, TRUE)) {
      g_srb_status = srb[0x2c];
      memcpy(g_sense, srb + 0x34, sizeof g_sense);
      return -3;
    }
  }
  return (int)transferred;
}

const char* srb_errstr(int r) {
  static char buf[128];
  switch(r) {
    case -1:
      sprintf(buf, "DeviceIoControl rejected (GLE=%lu)", GetLastError());
      break;
    case -2:
      sprintf(buf, "timed out after wait");
      break;
    case -3:
      sprintf(buf,
              "I/O completed with error (GLE=%lu) srb=%02x "
              "sense key=%x asc=%02x ascq=%02x",
              GetLastError(), g_srb_status, g_sense[2] & 0x0f, g_sense[12],
              g_sense[13]);
      break;
    default:
      sprintf(buf, "code %d", r);
      break;
  }
  return buf;
}

/* Pipelined flood (ftpoc.c read_flood): each slot owns its SRB (the
 * driver writes status/sense back through pointers into it), buffer,
 * event and OVERLAPPED; the port driver issues the queued requests
 * back-to-back. */
struct slot {
  BYTE srb[0x70];
  OVERLAPPED ov;
  BYTE* buf;
  int busy;
};

struct io_queue {
  struct slot* sl;
  int depth;
  BYTE cdb[10];
  WORD blklen;
  DWORD timeout_ms; /* the cancel's wait per slot */
};

io_queue* ioq_open(int depth, const BYTE cdb[10], WORD blklen,
                   DWORD timeout_ms) {
  io_queue* q = calloc(1, sizeof *q);
  int i;
  if(!q) return NULL;
  q->sl = calloc(depth, sizeof *q->sl);
  if(!q->sl) {
    free(q);
    return NULL;
  }
  q->depth = depth;
  memcpy(q->cdb, cdb, 10);
  q->blklen = blklen;
  q->timeout_ms = timeout_ms;
  for(i = 0; i < depth; i++) {
    q->sl[i].buf = malloc(blklen);
    q->sl[i].ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if(!q->sl[i].buf || !q->sl[i].ov.hEvent) {
      ioq_close(q, 0);
      return NULL;
    }
  }
  return q;
}

BYTE* ioq_buf(io_queue* q, int slot) { return q->sl[slot].buf; }

int ioq_issue(io_queue* q, int slot, long seq) {
  struct slot* s = &q->sl[slot];
  BYTE* srb = s->srb;
  memset(srb, 0, sizeof s->srb);
  *(DWORD*)(srb + 0x04) = 44;
  srb[0x08] = SRB_DIR_IN;
  srb[0x0c] = 10;
  srb[0x0d] = 0x0e;
  *(DWORD*)(srb + 0x10) = q->blklen;
  memcpy(srb + 0x14, q->cdb, 10);
  *(DWORD*)(srb + 0x24) = (DWORD)(ULONG_PTR)(srb + 0x2c);
  *(DWORD*)(srb + 0x28) = (DWORD)(ULONG_PTR)(srb + 0x34);
  ResetEvent(s->ov.hEvent);
  s->ov.Internal = s->ov.InternalHigh = 0;
  if(!DeviceIoControl(g_dev, IOCTL_SRB, srb, 44, s->buf, q->blklen, NULL,
                      &s->ov) &&
     GetLastError() != ERROR_IO_PENDING) {
    printf("\nflood issue %ld: GLE=%lu\n", seq, GetLastError());
    return -1;
  }
  s->busy = 1;
  return 0;
}

int ioq_reap(io_queue* q, int slot, long seq, DWORD timeout_ms, DWORD* got) {
  struct slot* s = &q->sl[slot];
  *got = 0;
  if(WaitForSingleObject(s->ov.hEvent, timeout_ms) != WAIT_OBJECT_0)
    return IOQ_TIMEOUT;
  s->busy = 0;
  if(!GetOverlappedResult(g_dev, &s->ov, got, TRUE)) {
    DWORD gle = GetLastError();
    /* SRB_STATUS_DATA_OVERRUN (0x12) also means UNDERRUN on
     * Windows: a short transfer, the host is ahead of the CCD.
     * Keep the bytes that did arrive and carry on. */
    if(gle == ERROR_MORE_DATA && (s->srb[0x2c] & 0x3f) == 0x12)
      return IOQ_SHORT;
    printf("\nflood read %ld: GLE=%lu srb=%02x\n", seq, gle, s->srb[0x2c]);
    return IOQ_ERROR;
  }
  return IOQ_FULL;
}

void ioq_close(io_queue* q, int cancel) {
  int i;
  if(cancel) {
    CancelIo(g_dev);
    for(i = 0; i < q->depth; i++)
      if(q->sl[i].busy && q->sl[i].ov.hEvent)
        WaitForSingleObject(q->sl[i].ov.hEvent, q->timeout_ms);
  }
  for(i = 0; i < q->depth; i++) {
    free(q->sl[i].buf);
    if(q->sl[i].ov.hEvent) CloseHandle(q->sl[i].ov.hEvent);
  }
  free(q->sl);
  free(q);
}

typedef struct {
  BYTE PathId;
} STORAGE_BUS_RESET_REQUEST_;

/* SCSI bus reset on the HBA port that hosts the Imacon: every device on the
 * bus returns to its power-on state. Refuses if the port has a disk (type
 * 0) or anything that is not the scanner on it. Needs admin. */
#define IOCTL_SCSI_GET_INQUIRY_DATA 0x0004100c
#define IOCTL_STORAGE_RESET_BUS 0x002d5000

int io_busreset(void) {
  static BYTE info[16384];
  int port;
  for(port = 0; port < 8; port++) {
    char path[24];
    HANDLE h;
    DWORD n = 0, nbus, b, off;
    int has_imacon = 0, foreign = 0, path_id = -1;
    sprintf(path, "\\\\.\\Scsi%d:", port);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                    NULL);
    if(h == INVALID_HANDLE_VALUE) continue;
    memset(info, 0, sizeof info);
    if(!DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0, info,
                        sizeof info, &n, NULL)) {
      printf("%s: inquiry list failed (GLE=%lu)\n", path, GetLastError());
      CloseHandle(h);
      continue;
    }
    /* SCSI_ADAPTER_BUS_INFO { u8 NumberOfBuses; SCSI_BUS_DATA[] (8 B:
     * u8 NumberOfLogicalUnits, u8 InitiatorBusId, u32 InquiryDataOffset) }
     * -> chain of SCSI_INQUIRY_DATA { u8 PathId, TargetId, Lun,
     * DeviceClaimed; u32 InquiryDataLength; u32 NextInquiryDataOffset;
     * u8 InquiryData[] } */
    nbus = info[0];
    for(b = 0; b < nbus; b++) {
      off = *(DWORD*)(info + 4 + b * 8 + 4);
      while(off && off + 16 + 36 <= n) {
        BYTE* e = info + off;
        BYTE* inq = e + 12;
        printf("%s path %u target %u lun %u: type %u %.8s %.16s\n", path, e[0],
               e[1], e[2], inq[0] & 0x1f, inq + 8, inq + 16);
        if(memcmp(inq + 8, "Imacon", 6) == 0) {
          has_imacon = 1;
          path_id = e[0];
        } else {
          foreign = 1;
        }
        off = *(DWORD*)(e + 8);
      }
    }
    if(has_imacon) {
      STORAGE_BUS_RESET_REQUEST_ rq;
      if(foreign) {
        printf("refusing: %s hosts other devices too\n", path);
        CloseHandle(h);
        return 1;
      }
      rq.PathId = (BYTE)path_id;
      if(!DeviceIoControl(h, IOCTL_STORAGE_RESET_BUS, &rq, sizeof rq, NULL, 0,
                          &n, NULL)) {
        printf("bus reset failed (GLE=%lu)\n", GetLastError());
        CloseHandle(h);
        return 1;
      }
      printf("bus reset issued on %s path %d\n", path, path_id);
      CloseHandle(h);
      return 0;
    }
    CloseHandle(h);
  }
  printf("no SCSI port lists the Imacon\n");
  return 1;
}

static volatile int* g_flag;

static BOOL WINAPI on_ctrl(DWORD type) {
  if(type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    *g_flag = 1;
    return TRUE;
  }
  return FALSE;
}

void io_catch_interrupt(volatile int* flag) {
  g_flag = flag;
  SetConsoleCtrlHandler(on_ctrl, TRUE);
}

DWORD io_ms(void) { return GetTickCount(); }

void io_sleep(DWORD ms) { Sleep(ms); }

void io_exe_path(char exe[IO_PATH_MAX + 1]) {
  DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
  exe[n > 0 && n < MAX_PATH ? n : 0] = 0;
}

int io_replace(const char* tmp, const char* path) {
  return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int io_free_bytes(const char* dir, double* bytes) {
  ULARGE_INTEGER avail;
  if(!GetDiskFreeSpaceExA(dir, &avail, NULL, NULL)) return -1;
  *bytes = (double)avail.QuadPart;
  return 0;
}

const char* io_syserr(void) {
  static char buf[32];
  sprintf(buf, "GLE=%lu", GetLastError());
  return buf;
}
