/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fmt.h - platform-neutral JSONL record formatter for the FlexColor
 * DeviceIoControl transport trace.
 *
 * Pure C90, no libc beyond <stddef.h>: this file + fmt.c must compile both
 * as Windows DLL code (ft_hook.dll, i686-w64-mingw32-gcc / cl) and as a
 * native Linux unit test binary.
 *
 * Record contract (must stay compatible with the future trace parser):
 *   one compact JSON object per line, lowercase hex, no spaces, fields:
 *   h, seq, t, code, in_len, in_head, ok, out_len, out_head
 *
 * `seq` is a per-process monotonically increasing counter assigned by the
 * hook at interception time; it keys the binary payload sidecar
 * (flextrace_bufs.bin, see README) back to the JSONL record. Gaps are
 * expected (quiet mode, non-scanner codes, pass-through).
 *
 * `t` is GetTickCount() (ms) captured at interception. GetTickCount reads a
 * shared kernel page, so the value is the SAME clock in every process on the
 * box — uiev.exe stamps UI events with it too, and the two files interleave
 * on one timeline (see merge_trace.py). Wraps at ~49.7 days of uptime;
 * irrelevant for capture sessions.
 */
#ifndef FT_FMT_H
#define FT_FMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of in/out buffer captured in the head fields. Single definition
 * here so the formatter (fmt.c) and the hook (hook.c) cannot drift. */
#define FT_MAX_HEAD 64

/* Writes one JSONL record (no trailing newline) into buf.
 * Returns bytes written (excl. NUL) or -1 if cap is insufficient. */
int ft_fmt_ioctl_record(
    char *buf, size_t cap, unsigned long h_device, unsigned long seq,
    unsigned long tick, unsigned long io_code, size_t in_len,
    const unsigned char *in_head, size_t in_head_len, int success,
    size_t out_len, const unsigned char *out_head, size_t out_head_len
);

#ifdef __cplusplus
}
#endif

#endif /* FT_FMT_H */
