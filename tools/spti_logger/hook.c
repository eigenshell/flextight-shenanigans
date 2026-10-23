/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hook.c - ft_hook.dll: in-process inline hook on kernel32!DeviceIoControl
 * for tracing FlexColor 4.0.3's SCSI traffic (private IOCTL_SCSISCAN_CMD into
 * Imacon's scsiscan.sys) as JSONL.
 *
 * Usage on the tracing rig
 * ------------------------
 *   1. Build ft_hook.dll (32-bit!) and inject.exe, e.g. with mingw:
 *        i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -o ft_hook.dll hook.c
 * fmt.c i686-w64-mingw32-gcc -O2 -Wall -Wextra -o inject.exe inject.c or on
 * Windows with build.bat (Visual Studio cl).
 *   2. Place ft_hook.dll NEXT TO inject.exe (inject derives the DLL path from
 *      its own location).
 *   3. Run:  inject.exe "C:\Program Files\Imacon\FlexColor\FlexColor.exe"
 *      FlexColor starts suspended, the DLL is injected, the hook installed,
 *      then the app resumes.
 *   4. Traces land in flextrace_ioctl.jsonl NEXT TO ft_hook.dll (the trace
 *      path is derived from the DLL's own location at load time; C:\ root is
 *      NOT used because non-elevated processes cannot create files there on
 *      Vista+, and the silent fopen failure cost a whole debugging session).
 *      Appended, flushed per record.
 *      A line {"tag":"hook_loaded","target":"DeviceIoControl"} marks a loaded
 *      hook; one {"h":...,"code":...,"in_len":...,"in_head":...,"ok":...,
 *      "out_len":...,"out_head":...} line per DeviceIoControl call.
 *   5. First capture goal: identify the numeric value of the private
 *      IOCTL_SCSISCAN_CMD control code. ALL DeviceIoControl calls are logged;
 *      the scanner-handle ones (h = handle opened on \\.\Scanner%d) dominate
 *      and repeat the same `code` on every CDB exchange.
 *
 * Hook design notes / caveats
 * ---------------------------
 * - Classic 5-byte E9 inline hook on the kernel32.dll export. FlexColor.exe
 *   and FlexColor.dll import DeviceIoControl from KERNEL32, so this
 *   catches every call from any module in
 *   the process. On Win7+ kernel32!DeviceIoControl is a small thunk into
 *   kernelbase.dll; hooking the kernel32 entry is still correct for callers
 *   that import kernel32, but a module calling kernelbase directly would
 *   bypass the hook (none do here). On era hardware (Win2K/XP) kernel32 IS
 *   the implementation.
 * - Target is 32-bit code; rel32 offsets computed with
 *   (DWORD)((BYTE*)dst - (BYTE*)src - 5).
 * - Not thread-safe against concurrent first-call installation, but DllMain
 *   installs the hook before the app's threads run (injected into a suspended
 *   process), so there is no race window in practice.
 * - The CRT (fopen/fwrite) is used for logging. It does not call
 *   DeviceIoControl, and a simple depth guard breaks any accidental
 *   re-entry, so the hook cannot recurse into itself.
 * - Antivirus/EDR on modern Windows may flag inline hooking of system DLLs;
 *   prefer the era-appropriate rig (WinXP/2K x86) where this is unremarkable.
 * - Trace file grows unboundedly during long sessions; rotate
 *   flextrace_ioctl.jsonl (next to ft_hook.dll) between runs.
 *
 * Build: i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -static-libgcc -o
 * ft_hook.dll hook.c fmt.c
 */
#include "fmt.h"
#include <stdio.h>
#include <windows.h>

#ifdef _WIN64
#error "ft_hook must be built 32-bit (FlexColor is a 32-bit app)"
#endif

#define FT_LOG_NAME "flextrace_ioctl.jsonl"
#define FT_BUFS_NAME                                                           \
  "flextrace_bufs.bin" /* binary payload sidecar (see below) */
#define FT_TRAMP_SZ 64

typedef BOOL(WINAPI *DeviceIoControl_fn)(
    HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer,
    DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped
);

static DeviceIoControl_fn g_tramp; /* RWX trampoline: saved 5 bytes + jmp back */
static LPVOID g_tramp_mem;         /* VirtualAlloc'd, PAGE_EXECUTE_READWRITE */
static int g_installed;            /* hook installed flag */
static int g_inside;               /* recursion guard */
static int
    g_passthru; /* FT_PASSTHRU=1: hook installed, zero logging (bisect tool) */
static int g_quiet; /* FT_QUIET=1: log only scanner SRB codes (bisect tool) */
static long g_quiet_logged;       /* quiet-mode record cap counter */
static CRITICAL_SECTION g_log_cs; /* serializes trace-file writes */
static char g_log_path[MAX_PATH] =
    FT_LOG_NAME; /* resolved to the DLL's dir at load */
static char g_bufs_path[MAX_PATH] =
    FT_BUFS_NAME;           /* resolved next to g_log_path */
static FILE *g_log_file;    /* opened ONCE at boot; kept open
                               (per-record fopen/fclose was 2+
                               syscalls per ioctl and stalled the
                               app's realtime SCSI thread — the
                               driver answered the stalls with
                               ERROR_IO_DEVICE (1117), which the
                               app retried in a storm) */
static FILE *g_bufs_file;   /* binary sidecar with the DATA BUFFERS
                               the SRBs point at (payloads), keyed
                               to jsonl records by (pid, seq) */
static unsigned long g_seq; /* per-intercepted-call counter, keyed
                               into both the jsonl "seq" field and
                               the sidecar records */

/* FT_QUIET record cap: the SRB retry storm can hit thousands of identical
 * failures; beyond this many quiet-mode records the picture cannot get
 * clearer and the file only grows. */
#define FT_QUIET_LIMIT 500

/* The scanner SRB control codes (learned from the first live trace; see
 * README). Quiet mode logs ONLY these. */
#define FT_CODE_SRB 0x190012
#define FT_CODE_SRB2 0x190022

static void ft_copy(void *dst, const void *src, size_t n) {
  BYTE *d = (BYTE *)dst;
  const BYTE *s = (const BYTE *)src;
  while(n--)
    *d++ = *s++;
}

/* Resolve the trace paths to the directory containing ft_hook.dll. The C:\ root
 * is not writable for non-elevated processes on Vista+, and the earlier
 * silent fopen failure there cost a debugging session; the DLL's own
 * directory is always writable (inject.exe already requires the pair to be
 * co-located). Falls back to the bare filenames (process cwd) if the module
 * path cannot be determined. Builds BOTH trace paths from one dir prefix. */
static void ft_log_path_init(HINSTANCE hinst) {
  char p[MAX_PATH];
  DWORD n = GetModuleFileNameA(hinst, p, MAX_PATH);
  if(n > 0 && n < MAX_PATH) {
    char *slash = NULL;
    DWORD i;
    for(i = 0; i < n; i++)
      if(p[i] == '\\')
        slash = p + i;
    if(slash) {
      /* keep the trailing backslash, drop the DLL filename */
      size_t dirlen = (size_t)(slash - p) + 1;
      size_t j;
      for(j = 0; j < dirlen; j++) {
        g_log_path[j] = p[j];
        g_bufs_path[j] = p[j];
      }
      for(j = 0; FT_LOG_NAME[j]; j++)
        g_log_path[dirlen + j] = FT_LOG_NAME[j];
      g_log_path[dirlen + j] = '\0';
      for(j = 0; FT_BUFS_NAME[j]; j++)
        g_bufs_path[dirlen + j] = FT_BUFS_NAME[j];
      g_bufs_path[dirlen + j] = '\0';
    }
  }
}

/* Append one line (a full JSONL record) to the trace file as a SINGLE fwrite
 * of line+'\n' under the critical section: concurrent callers cannot
 * interleave records. The stream is opened once at boot and flushed per
 * record (fflush, not fclose — with a full stdio buffer most records cost
 * only a memcpy, no syscall). Per-record fopen/fclose measurably stalled the
 * host's realtime SCSI thread and made the scanner driver fail commands. */
static void ft_log_line(const char *line, size_t len) {
  char full[522];

  if(len + 2 > sizeof full)
    return;
  ft_copy(full, line, len);
  full[len] = '\n';
  full[len + 1] = '\0';

  EnterCriticalSection(&g_log_cs);
  if(g_log_file) {
    fwrite(full, 1, len + 1, g_log_file); /* line + newline, one write */
    fflush(g_log_file);
  }
  LeaveCriticalSection(&g_log_cs);
}

/* Foreign-schema control record: the GetLastError of a failed ioctl, captured
 * immediately after the tramp returns (our CRT logging clobbers it). Lets us
 * see the DRIVER's real reason instead of whatever stale value the host app
 * prints in its error dialog. Parsers must skip lines that don't match the
 * ioctl-record schema, so this costs the main schema nothing. */
static void ft_log_lasterr(
    unsigned long h, unsigned long code, unsigned long seq
) {
  char line[200];
  int n = snprintf(
      line, sizeof line,
      "{\"tag\":\"lasterr\",\"seq\":%lu,\"h\":\"0x%lx\","
      "\"code\":\"0x%lx\",\"err\":%lu}",
      seq, h, code, (unsigned long)GetLastError()
  );
  if(n > 0 && (size_t)n < sizeof line)
    ft_log_line(line, (size_t)n);
}

/* ---- binary payload sidecar --------------------------------------------- *
 * The 44-byte SRB header (in_head) names the CDB but not its payload.
 * Empirical anatomy after the first sidecar smoke run (see README):
 *   - SRB offset 36 is a readable pointer for every scanner SRB. For the
 *     read-type commands (12/3c/24) it points to a small DESCRIPTOR
 *     {u32 count=1, ptr} — the real data buffer is `ptr`, one indirection
 *     in. For df (2-byte payload) it points straight at raw payload bytes
 *     (0x01 0x00 = the poll code). So we dump the cand-36 target AND both
 *     nested dwords (candidates 0x51/0x52), VirtualQuery-guarded.
 *   - DeviceIoControl's lpOutBuffer (candidate 0x60) is the METHOD_OUT_DIRECT
 *     data carrier: the driver copies read responses there (the INQUIRY
 *     "Imacon" bytes showed up in out_head pre-sidecar) and write payloads
 *     may sit there pre-call. Dumped pre- and post-call, no chasing needed.
 * Every record is keyed (pid, seq, when, candidate) to its JSONL record.
 * `when`: 0 = pre-call (what the app sent), 1 = post-call on synchronous
 * success (what the device answered), 2 = late completion dump of an
 * overlapped (err 997) call — fired at the NEXT scanner-SRB hook, when the
 * app has waited for the I/O and the buffer is final (session 2 proved that
 * without this, async responses only appear as reused-buffer leftovers).
 * Silent on any doubt: a bad guess must never crash the host app.      */
#define FT_BUF_SIDE_CAP                                                        \
  8192                      /* per-dump cap: 3b microcode chunks are 4106 B    \
                             */
#define FT_CAND_SRBPTR 0x36 /* dword at SRB offset 36 (descriptor/payload) */
#define FT_CAND_IND0 0x51   /* *(u32*)(srbptr_target+0) */
#define FT_CAND_IND1 0x52   /* *(u32*)(srbptr_target+4) */
#define FT_CAND_OUTBUF 0x60 /* lpOutBuffer itself */

/* Is [ptr, ptr+len) committed and readable? len is clamped by the caller. */
static int ft_buf_readable(const void *ptr, size_t len) {
  MEMORY_BASIC_INFORMATION mbi;
  const BYTE *p = (const BYTE *)ptr;
  size_t off = 0;
  if(ptr == NULL || len == 0)
    return 0;
  while(off < len) {
    if(VirtualQuery(p + off, &mbi, sizeof mbi) == 0)
      return 0;
    if(mbi.State != MEM_COMMIT)
      return 0;
    switch(mbi.Protect & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: break;
    default: return 0; /* PAGE_GUARD, PAGE_NOACCESS, zero */
    }
    off += mbi.RegionSize;
  }
  return 1;
}

/* Write one sidecar record: 4B pid, 4B seq, 1B when, 1B cand, 2B len, payload.
 * Call under g_log_cs with g_bufs_file known-open. */
static void ft_sidecar_write(
    DWORD pid, unsigned long seq, int when, unsigned char cand, const void *ptr,
    size_t len
) {
  unsigned char hdr[12];
  unsigned short stored = (unsigned short)len;
  ft_copy(hdr + 0, &pid, 4);
  ft_copy(hdr + 4, &seq, 4);
  hdr[8] = (unsigned char)when;
  hdr[9] = cand;
  ft_copy(hdr + 10, &stored, 2);
  fwrite(hdr, 1, sizeof hdr, g_bufs_file);
  fwrite(ptr, 1, len, g_bufs_file);
}

/* Dump every payload candidate for one intercepted call.
 * `srb` = lpInBuffer (44-byte SRB), `outbuf` = lpOutBuffer. */
static void ft_sidecar_dump(
    unsigned long seq, const void *srb, const void *outbuf, size_t outbuf_len,
    int when
) {
  const BYTE *b = (const BYTE *)srb;
  DWORD dlen_le;
  size_t dlen;
  DWORD pid;
  void *srbptr = NULL;
  DWORD nested0, nested1;

  if(!g_bufs_file)
    return;
  ft_copy(&dlen_le, b + 16, 4); /* SRB data length, little-endian */
  dlen = dlen_le;
  if(dlen == 0)
    return;
  if(dlen > FT_BUF_SIDE_CAP)
    dlen = FT_BUF_SIDE_CAP; /* clamp: dump the head of oversized payloads
                             * (the 32 KB motor-table 3b write is 32852 B)
                             * rather than skipping them entirely */
  pid = GetCurrentProcessId();

  EnterCriticalSection(&g_log_cs);
  if(!g_bufs_file) {
    LeaveCriticalSection(&g_log_cs);
    return;
  }

  /* candidate 0x36: descriptor or raw payload buffer */
  ft_copy(&srbptr, b + FT_CAND_SRBPTR, 4);
  if(ft_buf_readable(srbptr, dlen))
    ft_sidecar_write(pid, seq, when, FT_CAND_SRBPTR, srbptr, dlen);

  /* candidates 0x51/0x52: one level of indirection into the descriptor */
  if(ft_buf_readable(srbptr, 8)) {
    ft_copy(&nested0, (BYTE *)srbptr + 0, 4);
    ft_copy(&nested1, (BYTE *)srbptr + 4, 4);
    if(ft_buf_readable((void *)nested0, dlen))
      ft_sidecar_write(pid, seq, when, FT_CAND_IND0, (void *)nested0, dlen);
    if(ft_buf_readable((void *)nested1, dlen))
      ft_sidecar_write(pid, seq, when, FT_CAND_IND1, (void *)nested1, dlen);
  }

  /* candidate 0x60: the METHOD_OUT_DIRECT carrier buffer */
  if(outbuf != NULL && outbuf_len > 0 && outbuf_len <= FT_BUF_SIDE_CAP &&
     ft_buf_readable(outbuf, outbuf_len))
    ft_sidecar_write(pid, seq, when, FT_CAND_OUTBUF, outbuf, outbuf_len);

  fflush(g_bufs_file);
  LeaveCriticalSection(&g_log_cs);
}

/* ---- async completion tracking ------------------------------------------- *
 * Overlapped (err 997) calls fill lpOutBuffer AFTER DeviceIoControl returns,
 * so the when=1 dump at return time is too early (the buffer still holds
 * pre-call content). The app must wait for completion before issuing the
 * next dependent SCSI command, so by the time the NEXT scanner SRB arrives
 * the buffer is final. We therefore track pending async calls and re-dump
 * their buffers once, tagged when=2, at the next scanner-SRB hook.
 * All pointers re-checked with VirtualQuery at dump time; buffers that the
 * app has since freed are skipped silently.                        */
#define FT_PEND_MAX 64

typedef struct {
  int active;
  unsigned long seq;
  DWORD tick;         /* issue time (GetTickCount, correlates with t) */
  const void *outbuf; /* lpOutBuffer as passed at issue */
  size_t outbuf_len;
  void *srbptr; /* SRB offset-36 target at issue */
  size_t dlen;  /* SRB data length at issue */
} ft_pend_rec;

static ft_pend_rec g_pend[FT_PEND_MAX];
static unsigned g_pend_next;

static void ft_pend_add(
    unsigned long seq, const void *outbuf, size_t outbuf_len, const void *srb
) {
  const BYTE *b = (const BYTE *)srb;
  DWORD dlen_le;
  ft_pend_rec *r = &g_pend[g_pend_next];

  ft_copy(&dlen_le, b + 16, 4);
  r->dlen = dlen_le;
  if(r->dlen > FT_BUF_SIDE_CAP)
    r->dlen = FT_BUF_SIDE_CAP; /* same clamp as ft_sidecar_dump */
  if(r->dlen == 0) {
    r->active = 0;
    return;
  }
  r->seq = seq;
  r->tick = GetTickCount();
  r->outbuf = outbuf;
  r->outbuf_len = outbuf_len;
  ft_copy(&r->srbptr, b + FT_CAND_SRBPTR, 4);
  r->active = 1;
  g_pend_next = (g_pend_next + 1) % FT_PEND_MAX;
}

/* re-dump one tracked buffer set, same candidates as the main sidecar.
 * Caller holds g_log_cs. */
static void ft_pend_dump_one(ft_pend_rec *r, DWORD pid) {
  DWORD nested0, nested1;

  if(r->srbptr != NULL && ft_buf_readable(r->srbptr, r->dlen))
    ft_sidecar_write(pid, r->seq, 2, FT_CAND_SRBPTR, r->srbptr, r->dlen);
  if(ft_buf_readable(r->srbptr, 8)) {
    ft_copy(&nested0, (BYTE *)r->srbptr + 0, 4);
    ft_copy(&nested1, (BYTE *)r->srbptr + 4, 4);
    if(ft_buf_readable((void *)nested0, r->dlen))
      ft_sidecar_write(pid, r->seq, 2, FT_CAND_IND0, (void *)nested0, r->dlen);
    if(ft_buf_readable((void *)nested1, r->dlen))
      ft_sidecar_write(pid, r->seq, 2, FT_CAND_IND1, (void *)nested1, r->dlen);
  }
  if(r->outbuf != NULL && r->outbuf_len > 0 &&
     r->outbuf_len <= FT_BUF_SIDE_CAP &&
     ft_buf_readable(r->outbuf, r->outbuf_len))
    ft_sidecar_write(pid, r->seq, 2, FT_CAND_OUTBUF, r->outbuf, r->outbuf_len);
}

static void ft_pend_flush(void) {
  DWORD pid = GetCurrentProcessId();
  unsigned i;

  EnterCriticalSection(&g_log_cs);
  if(g_bufs_file) {
    for(i = 0; i < FT_PEND_MAX; i++) {
      if(!g_pend[i].active)
        continue;
      g_pend[i].active = 0;
      ft_pend_dump_one(&g_pend[i], pid);
    }
    fflush(g_bufs_file);
  }
  LeaveCriticalSection(&g_log_cs);
}

static BOOL WINAPI hooked_DeviceIoControl(
    HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer,
    DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize,
    LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped
) {
  unsigned char in_head[FT_MAX_HEAD];
  unsigned char out_head[FT_MAX_HEAD];
  size_t in_head_len = 0, out_head_len = 0;
  BOOL ret;
  DWORD err_now = 0;
  char rec[540];
  int rlen;

  if(g_passthru || g_inside) /* never recurse through our own logging */
    return g_tramp(
        hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer,
        nOutBufferSize, lpBytesReturned, lpOverlapped
    );

  g_inside = 1;
  {
    unsigned long seq = g_seq++;

    /* pre-call state (quiet mode: only bother for the scanner SRB codes) */
    if(lpInBuffer != NULL && nInBufferSize > 0 &&
       (!g_quiet || dwIoControlCode == FT_CODE_SRB ||
        dwIoControlCode == FT_CODE_SRB2)) {
      in_head_len = nInBufferSize < FT_MAX_HEAD ? nInBufferSize : FT_MAX_HEAD;
      ft_copy(in_head, lpInBuffer, in_head_len);
    }

    /* payload sidecar: dump the SRB data buffer and lpOutBuffer pre-call
     * (what the app SENT: df/e0 write payloads, INQUIRY is a nothing) for
     * scanner SRBs except the streaming READ(10) (0x28) — its buffer is
     * multi-MB and fully described by the CDB anyway. First flush any
     * async completions still pending from earlier overlapped calls —
     * their buffers are final now (the app waited before re-issuing). */
    if(g_bufs_file && dwIoControlCode == FT_CODE_SRB && lpInBuffer != NULL &&
       nInBufferSize >= 44 && ((const BYTE *)lpInBuffer)[20] != 0x28) {
      ft_pend_flush();
      ft_sidecar_dump(seq, lpInBuffer, lpOutBuffer, nOutBufferSize, 0);
    }

    ret = g_tramp(
        hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer,
        nOutBufferSize, lpBytesReturned, lpOverlapped
    );
    if(ret == 0)
      err_now = GetLastError(); /* capture NOW: CRT below clobbers it */

    /* post-call: the buffers now hold what the DEVICE answered (df ack,
     * 24 status bytes, 3c buffer contents, INQUIRY page). Only when the
     * call RETURNED success: for overlapped I/O ret==0/err 997 means the
     * driver is still writing lpOutBuffer, so a post-dump would race it */
    if(g_bufs_file && ret != 0 && dwIoControlCode == FT_CODE_SRB &&
       lpInBuffer != NULL && nInBufferSize >= 44 &&
       ((const BYTE *)lpInBuffer)[20] != 0x28)
      ft_sidecar_dump(seq, lpInBuffer, lpOutBuffer, nOutBufferSize, 1);

    /* overlapped completion pending: the response lands later, in the
     * app thread's wait. Track it; the when=2 dump fires at the next
     * scanner-SRB hook, when the buffer is final. */
    if(g_bufs_file && ret == 0 && err_now == ERROR_IO_PENDING &&
       dwIoControlCode == FT_CODE_SRB && lpInBuffer != NULL &&
       nInBufferSize >= 44 && ((const BYTE *)lpInBuffer)[20] != 0x28)
      ft_pend_add(seq, lpOutBuffer, nOutBufferSize, lpInBuffer);

    /* The true failure reason must be captured NOW: our own CRT logging
     * calls below (fopen/fwrite/...) clobber GetLastError. Emitted as a
     * separate foreign-schema control record so the ioctl-record schema
     * stays untouched. */
    if(ret == 0 && (!g_quiet || dwIoControlCode == FT_CODE_SRB ||
                    dwIoControlCode == FT_CODE_SRB2))
      ft_log_lasterr(
          (unsigned long)(ULONG_PTR)hDevice, (unsigned long)dwIoControlCode, seq
      );

    /* quiet mode: drop everything except the scanner SRB codes (and cap the
     * storm) — bisects "hook presence on the call stack" against "logging
     * volume/timing" without changing what the hook intercepts. */
    if(g_quiet && dwIoControlCode != FT_CODE_SRB &&
       dwIoControlCode != FT_CODE_SRB2) {
      g_inside = 0;
      return ret;
    }
    if(g_quiet && g_quiet_logged >= FT_QUIET_LIMIT) {
      g_inside = 0;
      return ret;
    }
    g_quiet_logged++;

    /* post-call state */
    if(ret != 0 && lpOutBuffer != NULL && lpBytesReturned != NULL &&
       *lpBytesReturned > 0) {
      DWORD ob = *lpBytesReturned;
      out_head_len = ob < FT_MAX_HEAD ? ob : FT_MAX_HEAD;
      ft_copy(out_head, lpOutBuffer, out_head_len);
    }

    rlen = ft_fmt_ioctl_record(
        rec, sizeof rec, (unsigned long)(ULONG_PTR)hDevice, seq,
        (unsigned long)GetTickCount(), /* same clock as uiev.exe */
        (unsigned long)dwIoControlCode, (size_t)nInBufferSize, in_head,
        in_head_len, ret != 0,
        (ret != 0 && lpBytesReturned != NULL) ? (size_t)*lpBytesReturned : 0,
        out_head, out_head_len
    );
    if(rlen > 0)
      ft_log_line(rec, (size_t)rlen);
  }

  g_inside = 0;
  return ret;
}

/* Overwrite the first 5 bytes of `target` with E9 rel32 -> `dst`. */
static int ft_patch5(void *target, const void *dst) {
  BYTE *t = (BYTE *)target;
  DWORD old_prot;
  DWORD jmp_off;

  if(!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old_prot))
    return 0;
  jmp_off = (DWORD)((BYTE *)dst - t - 5);
  t[0] = 0xE9;
  ft_copy(t + 1, &jmp_off, 4);
  VirtualProtect(t, 5, old_prot, &old_prot);
  FlushInstructionCache(GetCurrentProcess(), t, 5);
  return 1;
}

/* Verify the 5 bytes we are about to steal form a known-good prologue of
 * complete instructions. Blindly copying 5 arbitrary bytes could split an
 * instruction mid-operand and crash the app. Known-good shapes:
 *   - XP/2K-era:  8B FF 55 8B EC   (mov edi,edi; push ebp; mov ebp,esp)
 *   - Win7+ kernel32 thunk: E9 rel32 (one complete jmp into kernelbase;
 *     the trampoline re-bases the rel32, see ft_install_hook)
 * Any 0xE8 (call rel32) is rejected outright (a call needs return-address
 * semantics the trampoline does not replicate). On mismatch the caller must
 * fail closed: no patch, DeviceIoControl left untouched. */
static int ft_prologue_ok(const BYTE *p, const char **reason) {
  static const BYTE xp_pro[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
  int i;

  for(i = 0; i < 5; i++) {
    if(p[i] == 0xE8) {
      *reason = "call_rel32_in_prologue";
      return 0;
    }
  }
  if(p[0] == 0xE9) {
    /* jmp-thunk: the five bytes are exactly one instruction, so stealing
     * them is safe. A copied E9 rel32 is NOT position-independent,
     * though: rel32 is measured from the NEXT instruction, so the copy
     * in the trampoline must be re-based by (target - tramp) to still
     * reach the original kernelbase destination (ft_install_hook does
     * this). The earlier verbatim copy sent every hooked call
     * (tramp - kernel32) bytes off the real function, which manifested
     * as DeviceIoControl "failing" with a stale unrelated LastError
     * while the app limped along on retries. */
    return 1;
  }
  for(i = 0; i < 5; i++) {
    if(p[i] != xp_pro[i]) {
      *reason = "unknown_prologue";
      return 0;
    }
  }
  return 1;
}

static int ft_install_hook(const char **reason) {
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  void *target;
  DWORD jmp_back;

  if(!k32) {
    *reason = "kernel32_not_found";
    return 0;
  }
  target = (void *)GetProcAddress(k32, "DeviceIoControl");
  if(!target) {
    *reason = "DeviceIoControl_not_found";
    return 0;
  }
  if(!ft_prologue_ok((const BYTE *)target, reason))
    return 0;

  /* 64 bytes of executable scratch: saved prologue + E9 back to target+5.
   * Cannot live in the DLL's .data (not executable) -> VirtualAlloc RWX. */
  g_tramp_mem = VirtualAlloc(
      NULL, FT_TRAMP_SZ, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
  );
  if(!g_tramp_mem) {
    *reason = "VirtualAlloc_failed";
    return 0;
  }

  ft_copy(g_tramp_mem, target, 5);
  if(((BYTE *)g_tramp_mem)[0] == 0xE9) {
    /* Re-base the stolen jmp-thunk: rel' = rel + (target - tramp), since
     * the E9 sits at offset 0 in both places and rel32 is measured from
     * the next instruction. Without this, the trampoline dispatches to
     * kernelbase_target + (tramp - kernel32) — arbitrary memory. */
    LONG rel, rebased;
    ft_copy(&rel, (BYTE *)g_tramp_mem + 1, 4);
    rebased = rel + (LONG)((BYTE *)target - (BYTE *)g_tramp_mem);
    ft_copy((BYTE *)g_tramp_mem + 1, &rebased, 4);
  }
  ((BYTE *)g_tramp_mem)[5] = 0xE9;
  jmp_back = (DWORD)((BYTE *)target + 5 - (BYTE *)g_tramp_mem - 5 - 5);
  ft_copy((BYTE *)g_tramp_mem + 6, &jmp_back, 4);
  /* tramp layout: [0..4] saved bytes, [5] E9, [6..9] rel32 -> target+5 */

  g_tramp = (DeviceIoControl_fn)(void *)g_tramp_mem;

  if(!ft_patch5(target, (void *)hooked_DeviceIoControl)) {
    *reason = "VirtualProtect_failed";
    return 0;
  }
  return 1;
}

static void ft_log_boot(const char *status, const char *prologue) {
  char line[200];
  int n;
  /* hand-written; does not depend on hook installation success */
  n = snprintf(
      line, sizeof line,
      "{\"tag\":\"hook_loaded\",\"target\":\"DeviceIoControl\","
      "\"status\":\"%s\",\"prologue\":\"%s\",\"pid\":%lu}",
      status, prologue, (unsigned long)GetCurrentProcessId()
  );
  if(n > 0 && (size_t)n < sizeof line)
    ft_log_line(line, (size_t)n);
}

static void ft_log_install_failed(const char *reason) {
  char line[200];
  int n;
  n = snprintf(
      line, sizeof line,
      "{\"tag\":\"install_failed\",\"target\":\"DeviceIoControl\","
      "\"reason\":\"%s\",\"pid\":%lu}",
      reason, (unsigned long)GetCurrentProcessId()
  );
  if(n > 0 && (size_t)n < sizeof line)
    ft_log_line(line, (size_t)n);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  (void)hinst;
  (void)reserved;
  if(reason == DLL_PROCESS_ATTACH) {
    const char *fail_reason = "unknown";
    const char *prologue =
        (*(BYTE *)GetProcAddress(
             GetModuleHandleA("kernel32.dll"), "DeviceIoControl"
         ) == 0xE9)
            ? "e9_thunk"
            : "xp_hotpatch";
    char envbuf[8];
    DWORD envn = GetEnvironmentVariableA("FT_PASSTHRU", envbuf, sizeof envbuf);
    DWORD qn = GetEnvironmentVariableA("FT_QUIET", envbuf, sizeof envbuf);
    InitializeCriticalSection(&g_log_cs);
    DisableThreadLibraryCalls(hinst);
    ft_log_path_init(hinst);
    /* open ONCE: per-record fopen/fclose dominated the per-call cost and
     * stalled the host's SCSI thread (see g_log_file comment) */
    g_log_file = fopen(g_log_path, "a");
    /* binary payload sidecar; append so a crash in a previous session
     * does not destroy payloads already correlated with its jsonl */
    g_bufs_file = fopen(g_bufs_path, "ab");
    g_passthru = (envn > 0 && envn < sizeof envbuf);
    g_quiet = (qn > 0 && qn < sizeof envbuf);
    g_installed = ft_install_hook(&fail_reason);
    if(g_installed)
      ft_log_boot("ok", prologue);
    else
      ft_log_install_failed(fail_reason);
  } else if(reason == DLL_PROCESS_DETACH) {
    EnterCriticalSection(&g_log_cs);
    if(g_log_file) {
      fclose(g_log_file);
      g_log_file = NULL;
    }
    if(g_bufs_file) {
      fclose(g_bufs_file);
      g_bufs_file = NULL;
    }
    LeaveCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_log_cs);
  }
  return TRUE;
}
