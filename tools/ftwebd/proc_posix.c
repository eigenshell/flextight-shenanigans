/* SPDX-License-Identifier: GPL-2.0-or-later */
/* proc.h on POSIX (Linux now, the Pi later; tests run against a fake
 * ftpoc). The child leads its own process group, so stop reaches it and
 * anything it spawned, and never ftwebd. */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "proc.h"

int proc_spawn(proc *p, const char *const *argv, const char *cwd,
               char *err, size_t errn)
{
    int fd[2];
    pid_t pid;
    if (pipe(fd)) {
        snprintf(err, errn, "pipe: %s", strerror(errno));
        return -1;
    }
    /* the read end stays with ftwebd (spawns are serialised, so no
     * other child can be forked before this is set) */
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        snprintf(err, errn, "fork: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        if (chdir(cwd)) {
            fprintf(stderr, "ftwebd: cannot enter %s: %s\n", cwd,
                    strerror(errno));
            _exit(127);
        }
        execv(argv[0], (char *const *)argv);
        fprintf(stderr, "ftwebd: cannot run %s: %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);              /* both sides: no race with stop */
    close(fd[1]);
    p->pid = (int)pid;
    p->out = fd[0];
    return 0;
}

long proc_read(proc *p, char *buf, size_t n)
{
    ssize_t r;
    do
        r = read(p->out, buf, n);
    while (r < 0 && errno == EINTR);
    return (long)r;
}

int proc_wait(proc *p)
{
    int st;
    pid_t r;
    do
        r = waitpid(p->pid, &st, 0);
    while (r < 0 && errno == EINTR);
    close(p->out);
    p->out = -1;
    if (r < 0 || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

int proc_stop(proc *p)
{
    return kill(-p->pid, SIGINT) ? -1 : 0;
}

int proc_abspath(const char *prog, char *out, size_t n)
{
    char buf[PATH_MAX];
    if (!realpath(prog, buf) || access(buf, X_OK) || strlen(buf) >= n)
        return -1;
    strcpy(out, buf);
    return 0;
}
