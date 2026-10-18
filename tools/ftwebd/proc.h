/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A child process with its stdout and stderr on one pipe, and a way to
 * interrupt it the way a console user would (ftpoc's Ctrl handler: stop at
 * the next line, END x2). proc_posix.c: SIGINT to the child's process
 * group; proc_win.c: CTRL_BREAK_EVENT to its console process group. */
#ifndef FTWEBD_PROC_H
#define FTWEBD_PROC_H
#include <stddef.h>

typedef struct {
#ifdef _WIN32
    void *process, *out;        /* HANDLEs */
    unsigned long pid;
#else
    int pid, out;
#endif
} proc;

/* argv[0] is the program (an absolute path); cwd the child's directory.
 * 0, or -1 with a message in err. */
int proc_spawn(proc *p, const char *const *argv, const char *cwd,
               char *err, size_t errn);
/* bytes read (> 0), 0 at end of output, -1 on error */
long proc_read(proc *p, char *buf, size_t n);
/* waits for the child and closes the pipe; its exit code (-1: killed or
 * unknown) */
int proc_wait(proc *p);
/* asks the child to stop (Ctrl+Break / SIGINT); 0 if delivered */
int proc_stop(proc *p);
/* an absolute path for prog (resolved against the current directory);
 * 0, or -1 if it does not exist */
int proc_abspath(const char *prog, char *out, size_t n);

#endif
