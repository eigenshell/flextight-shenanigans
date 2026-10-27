/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftio_posix.c — ftpoc's OS layer on Linux (ftio.h): SG_IO on /dev/sgN.
 *
 * The scanner sits on an sg node of its own (seen: /dev/sg1, SCSI
 * 0:0:2:0 on an Adaptec 2940AU) and keeps it across the microcode upload.
 * Only nodes whose sysfs vendor reads "Imacon" are opened, so no command
 * ever goes to another device. The node is opened O_EXCL: nothing else
 * may talk to the scanner meanwhile (the Windows driver opens
 * exclusively too).
 *
 * srb_exec is one blocking SG_IO ioctl. A resid underrun is a normal,
 * short completion (empty and partial reads while the host is ahead of
 * the CCD), never an error.
 *
 * The flood's queue uses the sg driver's own asynchronous interface:
 * write() queues a request (struct sg_io_hdr, pack_id = its sequence
 * number), read() with SG_SET_FORCE_PACK_ID collects exactly that one.
 * This is what the overlapped DeviceIoControl queue is on Windows: the
 * kernel holds DEPTH requests and the block layer dispatches the next
 * from the completion of the last, with no trip to user space between
 * reads, so the bus never idles while this process writes a line out.
 * Threads doing blocking SG_IO would add nothing and could complete out
 * of order. DEPTH (8) is below the driver's per-fd limit (SG_MAX_QUEUE,
 * 16).
 *
 * Signals: a signal that arrives during a blocking SG_IO orphans the
 * command; with SA_RESTART the ioctl then runs again (the CDB goes out a
 * second time), without it the result is lost. So the commands that
 * catch Ctrl+C block SIGINT and SIGTERM in every thread and one thread
 * takes them with sigwait() (io_catch_interrupt), as the console handler
 * runs on a thread of its own on Windows.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "ftio.h"
#include "sgio.h"

/* sgio.c spells the direction constants out: they must be the kernel's */
typedef char sgio_dirs_are_linux[SG_DXFER_NONE == -1 &&
                                     SG_DXFER_TO_DEV == -2 &&
                                     SG_DXFER_FROM_DEV == -3
                                   ? 1
                                   : -1];

#define SENSE_LEN 32
/* A reap waits this much past the kernel's own timeout for the request,
 * so the kernel's verdict (DID_TIME_OUT, with status) arrives first. */
#define REAP_SLACK_MS 1000

io_dev g_dev = -1;
static const char* g_device; /* --device */

/* the last failure: -1 errno, -3 status, host, driver and sense */
static int g_errno;
static int g_status, g_host, g_driver;
static BYTE g_sense[SENSE_LEN];

void io_init(void) {
  /* ftwebd reads our stdout: if it goes away mid-scan, writes fail
   * (as on Windows) instead of killing the process before the end
   * descriptor */
  signal(SIGPIPE, SIG_IGN);
}

void io_set_device(const char* path) { g_device = path; }

#define SG_NODES 32

int io_dev_path(int i, char* path, size_t cap) {
  if(g_device) {
    if(i > 0 || strlen(g_device) >= cap) return 0;
    strcpy(path, g_device);
    return 1;
  }
  if(i >= SG_NODES || cap < 16) return 0;
  sprintf(path, "/dev/sg%d", i);
  return 1;
}

/* 1: sysfs names another vendor than Imacon (the node is skipped), 0:
 * Imacon or no sysfs entry (then the INQUIRY decides) */
static int foreign_vendor(const char* path) {
  char sys[96], v[16];
  const char* base = strrchr(path, '/');
  FILE* f;
  int foreign;
  base = base ? base + 1 : path;
  if(strlen(base) > 32) return 0;
  sprintf(sys, "/sys/class/scsi_generic/%s/device/vendor", base);
  f = fopen(sys, "r");
  if(!f) return 0;
  foreign = !fgets(v, sizeof v, f) || strncmp(v, "Imacon", 6) != 0;
  fclose(f);
  return foreign;
}

io_dev io_open(const char* path) {
  int fd, ver = 0;
  if(!g_device && foreign_vendor(path)) return -1;
  fd = open(path, O_RDWR | O_NONBLOCK | O_EXCL);
  if(fd < 0) {
    if(g_device || (errno != ENOENT && errno != ENXIO && errno != ENODEV))
      fprintf(stderr, "%s: %s%s\n", path, strerror(errno),
              errno == EBUSY    ? " (another program has it open)"
              : errno == EACCES ? " (run as root, or a udev rule)"
                                : "");
    return -1;
  }
  if(ioctl(fd, SG_GET_VERSION_NUM, &ver) < 0 || ver < 30000) {
    fprintf(stderr, "%s: not an sg device\n", path);
    close(fd);
    return -1;
  }
  return fd;
}

void io_close(io_dev d) { close(d); }

static void fill_hdr(sg_io_hdr_t* h, const BYTE* cdb, int cdb_len, BYTE dir,
                     BYTE* data, DWORD data_len, BYTE* sense,
                     DWORD timeout_ms) {
  memset(h, 0, sizeof *h);
  h->interface_id = 'S';
  h->dxfer_direction = sgio_dir(dir, data_len);
  h->cmd_len = (unsigned char)cdb_len;
  h->mx_sb_len = SENSE_LEN;
  h->dxfer_len = data_len;
  h->dxferp = data_len ? data : NULL;
  h->cmdp = (unsigned char*)cdb; /* the kernel only reads it */
  h->sbp = sense;
  h->timeout = timeout_ms;
}

/* the completed h as ftpoc's code (bytes or -2/-3), the failure kept */
static int finish(const sg_io_hdr_t* h, const BYTE* sense) {
  unsigned got;
  int r = sgio_result(h->status, h->host_status, h->driver_status, h->resid,
                      h->dxfer_len, &got);
  if(r < 0) {
    g_status = h->status;
    g_host = h->host_status;
    g_driver = h->driver_status;
    memcpy(g_sense, sense, SENSE_LEN);
    return r;
  }
  return (int)got;
}

/* Zero padding for data-out commands whose CDB asks for more than the
 * host's payload (sgio_out_len: SetWindow's 84-byte descriptor in a
 * 21504-byte transfer). Linux must move every byte the target asks for:
 * aic7xxx fails a short data-out as an overrun (DID_ERROR) after the
 * target has already acted on it. Windows pads the same way unseen. */
#define OUT_PAD_MAX 0x10000
static BYTE g_pad[OUT_PAD_MAX];

int srb_exec(const BYTE* cdb, int cdb_len, BYTE dir, BYTE* data, DWORD data_len,
             DWORD timeout_ms) {
  sg_io_hdr_t h;
  BYTE sense[SENSE_LEN];
  DWORD want = data_len;
  int r;
  memset(sense, 0, sizeof sense);
  if(dir == SRB_DIR_OUT && data_len)
    want = sgio_out_len(cdb, cdb_len, data_len);
  if(want > data_len) {
    if(want > OUT_PAD_MAX) {
      g_errno = EINVAL;
      return -1;
    }
    memcpy(g_pad, data, data_len);
    memset(g_pad + data_len, 0, want - data_len);
    fill_hdr(&h, cdb, cdb_len, dir, g_pad, want, sense, timeout_ms);
  } else {
    fill_hdr(&h, cdb, cdb_len, dir, data, data_len, sense, timeout_ms);
  }
  if(ioctl(g_dev, SG_IO, &h) < 0) {
    g_errno = errno;
    return -1;
  }
  r = finish(&h, sense);
  /* the caller's bytes, as on Windows (SetWindow: 84) */
  return r > (int)data_len ? (int)data_len : r;
}

const char* srb_errstr(int r) {
  static char buf[160];
  switch(r) {
    case -1:
      sprintf(buf, "SG_IO rejected (errno %d: %.60s)", g_errno,
              strerror(g_errno));
      break;
    case -2:
      sprintf(buf, "timed out after wait");
      break;
    case IO_SHORT:
      sprintf(buf, "short reply (underrun)");
      break;
    case -3:
      sprintf(buf,
              "I/O completed with error status=%02x host=%02x%s%s "
              "driver=%02x sense key=%x asc=%02x ascq=%02x",
              g_status, g_host, *sgio_host_name(g_host) ? " " : "",
              sgio_host_name(g_host), g_driver, g_sense[2] & 0x0f, g_sense[12],
              g_sense[13]);
      break;
    default:
      sprintf(buf, "code %d", r);
      break;
  }
  return buf;
}

/* ------------------------------------------------------------ flood queue */

struct slot {
  sg_io_hdr_t h;
  BYTE sense[SENSE_LEN];
  BYTE* buf;
  int busy;
  long seq;
};

struct io_queue {
  struct slot* sl;
  int depth;
  BYTE cdb[10];
  WORD blklen;
  DWORD timeout_ms;
};

io_queue* ioq_open(int depth, const BYTE cdb[10], WORD blklen,
                   DWORD timeout_ms) {
  io_queue* q = calloc(1, sizeof *q);
  int i, one = 1;
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
    if(!q->sl[i].buf) {
      ioq_close(q, 0);
      return NULL;
    }
  }
  /* read() collects the request named by pack_id, not just any */
  if(ioctl(g_dev, SG_SET_FORCE_PACK_ID, &one) < 0) {
    printf("\nflood: SG_SET_FORCE_PACK_ID: %s\n", strerror(errno));
    ioq_close(q, 0);
    return NULL;
  }
  return q;
}

BYTE* ioq_buf(io_queue* q, int slot) { return q->sl[slot].buf; }

int ioq_issue(io_queue* q, int slot, long seq) {
  struct slot* s = &q->sl[slot];
  ssize_t n;
  memset(s->sense, 0, sizeof s->sense);
  fill_hdr(&s->h, q->cdb, 10, SRB_DIR_IN, s->buf, q->blklen, s->sense,
           q->timeout_ms);
  s->h.pack_id = (int)seq;
  do n = write(g_dev, &s->h, sizeof s->h);
  while(n < 0 && errno == EINTR);
  if(n != (ssize_t)sizeof s->h) {
    printf("\nflood issue %ld: %s\n", seq,
           n < 0 ? strerror(errno) : "short write");
    return -1;
  }
  s->busy = 1;
  s->seq = seq;
  return 0;
}

/* the slot's completion into s->h: 0, IOQ_TIMEOUT, or IOQ_ERROR (errno
 * printed) */
static int collect(struct slot* s, DWORD timeout_ms) {
  DWORD t0 = io_ms();
  sg_io_hdr_t r;
  for(;;) {
    ssize_t n;
    DWORD dt;
    struct pollfd p;
    /* a v3 header (negative direction) naming the request wanted */
    memset(&r, 0, sizeof r);
    r.interface_id = 'S';
    r.dxfer_direction = SG_DXFER_FROM_DEV;
    r.pack_id = (int)s->seq;
    n = read(g_dev, &r, sizeof r);
    if(n == (ssize_t)sizeof r) break;
    if(n >= 0 || (errno != EAGAIN && errno != EINTR)) {
      printf("\nflood read %ld: %s\n", s->seq,
             n < 0 ? strerror(errno) : "short header");
      return IOQ_ERROR;
    }
    dt = io_ms() - t0;
    if(dt >= timeout_ms) return IOQ_TIMEOUT;
    p.fd = g_dev;
    p.events = POLLIN;
    p.revents = 0;
    /* POLLIN: some request is done; it is ours unless out of order,
     * then the next read says EAGAIN again: wait in small steps */
    poll(&p, 1, (int)(timeout_ms - dt < 20 ? timeout_ms - dt : 20));
  }
  s->busy = 0;
  if(r.pack_id != (int)s->seq) {
    printf("\nflood read %ld: got request %d instead\n", s->seq, r.pack_id);
    return IOQ_ERROR;
  }
  s->h.status = r.status;
  s->h.host_status = r.host_status;
  s->h.driver_status = r.driver_status;
  s->h.resid = r.resid;
  return 0;
}

int ioq_reap(io_queue* q, int slot, long seq, DWORD timeout_ms, DWORD* got) {
  struct slot* s = &q->sl[slot];
  int r;
  *got = 0;
  if(!s->busy || s->seq != seq) {
    printf("\nflood read %ld: not in flight\n", seq);
    return IOQ_ERROR;
  }
  r = collect(s, timeout_ms + REAP_SLACK_MS);
  if(r) return r;
  r = finish(&s->h, s->sense);
  if(r == SGIO_TIMEOUT) {
    printf("\nflood read %ld: %s\n", seq, srb_errstr(-2));
    return IOQ_TIMEOUT;
  }
  if(r < 0) {
    printf("\nflood read %ld: %s\n", seq, srb_errstr(-3));
    return IOQ_ERROR;
  }
  *got = (DWORD)r;
  return *got == q->blklen ? IOQ_FULL : IOQ_SHORT;
}

void ioq_close(io_queue* q, int cancel) {
  int i, zero = 0;
  /* sg cannot cancel: collect what is still queued (each request ends
   * by its own kernel timeout at the latest). Whatever is left is
   * discarded by the driver; it copies into our buffers only on
   * read(), so freeing them is safe. */
  if(cancel)
    for(i = 0; i < q->depth; i++)
      if(q->sl[i].busy) collect(&q->sl[i], q->timeout_ms + REAP_SLACK_MS);
  if(g_dev >= 0) ioctl(g_dev, SG_SET_FORCE_PACK_ID, &zero);
  for(i = 0; i < q->depth; i++) free(q->sl[i].buf);
  free(q->sl);
  free(q);
}

/* -------------------------------------------------------------- bus reset */

/* sysfs attribute of SCSI device "h:c:t:l" into v ("" if none) */
static void sysattr(const char* dev, const char* attr, char* v, size_t cap) {
  char p[300];
  FILE* f;
  size_t n;
  v[0] = 0;
  if(strlen(dev) > 200) return;
  sprintf(p, "/sys/bus/scsi/devices/%s/%s", dev, attr);
  f = fopen(p, "r");
  if(!f) return;
  if(!fgets(v, (int)cap, f)) v[0] = 0;
  fclose(f);
  n = strlen(v);
  while(n && (v[n - 1] == '\n' || v[n - 1] == ' ')) v[--n] = 0;
}

/* SCSI bus reset of the bus that hosts the scanner: every device on it
 * returns to its power-on state. Refused if anything but the Imacon is
 * on that bus (the sysfs list, as the Windows port list). Needs root
 * (CAP_SYS_ADMIN). */
int io_busreset(void) {
  struct sg_scsi_id id;
  DIR* d;
  struct dirent* e;
  int foreign = 0, op = SG_SCSI_RESET_BUS;

  memset(&id, 0, sizeof id);
  if(ioctl(g_dev, SG_GET_SCSI_ID, &id) < 0) {
    printf("SG_GET_SCSI_ID failed (%s)\n", strerror(errno));
    return 1;
  }
  d = opendir("/sys/bus/scsi/devices");
  if(!d) {
    printf("no /sys/bus/scsi/devices; refusing\n");
    return 1;
  }
  while((e = readdir(d)) != NULL) {
    int h, c, t, l;
    char end, vendor[32], model[32], type[8];
    if(sscanf(e->d_name, "%d:%d:%d:%d%c", &h, &c, &t, &l, &end) != 4 ||
       h != id.host_no ||
       c != id.channel)
      continue;
    sysattr(e->d_name, "vendor", vendor, sizeof vendor);
    sysattr(e->d_name, "model", model, sizeof model);
    sysattr(e->d_name, "type", type, sizeof type);
    printf("scsi %s: type %s %s %s\n", e->d_name, type, vendor, model);
    if(strncmp(vendor, "Imacon", 6) != 0) foreign = 1;
  }
  closedir(d);
  if(foreign) {
    printf("refusing: scsi host %d channel %d hosts other devices too\n",
           id.host_no, id.channel);
    return 1;
  }
  if(ioctl(g_dev, SG_SCSI_RESET, &op) < 0) {
    printf("bus reset failed (%s)\n", strerror(errno));
    return 1;
  }
  printf("bus reset issued on scsi host %d channel %d\n", id.host_no,
         id.channel);
  return 0;
}

/* ------------------------------------------------------------ OS helpers */

static volatile int* g_flag;
static sigset_t g_sigs;

static void* sig_thread(void* arg) {
  int sig;
  (void)arg;
  for(;;)
    if(sigwait(&g_sigs, &sig) == 0) *g_flag = 1;
  return NULL;
}

void io_catch_interrupt(volatile int* flag) {
  pthread_t t;
  g_flag = flag;
  sigemptyset(&g_sigs);
  sigaddset(&g_sigs, SIGINT);
  sigaddset(&g_sigs, SIGTERM);
  /* before any other thread starts: they inherit the mask */
  if(pthread_sigmask(SIG_BLOCK, &g_sigs, NULL) != 0 ||
     pthread_create(&t, NULL, sig_thread, NULL) != 0) {
    fprintf(stderr, "ftpoc: cannot catch Ctrl+C\n");
    exit(1);
  }
  pthread_detach(t);
}

DWORD io_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (DWORD)((unsigned long long)ts.tv_sec * 1000u +
                 (unsigned long long)ts.tv_nsec / 1000000u);
}

void io_sleep(DWORD ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while(nanosleep(&ts, &ts) < 0 && errno == EINTR);
}

void io_exe_path(char exe[IO_PATH_MAX + 1]) {
  ssize_t n = readlink("/proc/self/exe", exe, IO_PATH_MAX);
  exe[n > 0 && n < IO_PATH_MAX ? n : 0] = 0;
}

int io_replace(const char* tmp, const char* path) {
  if(rename(tmp, path) == 0) return 0;
  g_errno = errno;
  return -1;
}

int io_free_bytes(const char* dir, double* bytes) {
  struct statvfs s;
  if(statvfs(dir ? dir : ".", &s) < 0) {
    g_errno = errno;
    return -1;
  }
  *bytes = (double)s.f_bavail * (double)s.f_frsize;
  return 0;
}

const char* io_syserr(void) {
  static char buf[96];
  sprintf(buf, "errno %d: %.60s", g_errno, strerror(g_errno));
  return buf;
}
