/* SPDX-License-Identifier: GPL-2.0-or-later */
/* proc.h on Windows (the scanner box: XP/7, mingw). The child gets its own
 * console process group (CREATE_NEW_PROCESS_GROUP), so CTRL_BREAK_EVENT
 * reaches only it; ftpoc's on_ctrl takes Ctrl+Break like Ctrl+C. ftwebd
 * must run in a console for that (it is a console program). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proc.h"

/* argv -> one command line, every argument quoted; arguments are built by
 * ftwebd and never hold a '"' (refused) */
static int cmdline(const char *const *argv, char *out, size_t n,
                   char *err, size_t errn)
{
    size_t k = 0;
    int i;
    for (i = 0; argv[i]; i++) {
        size_t m = strlen(argv[i]);
        if (strchr(argv[i], '"')) {
            snprintf(err, errn, "argument with a quote: %s", argv[i]);
            return -1;
        }
        if (k + m + 4 >= n) {
            snprintf(err, errn, "command line too long");
            return -1;
        }
        if (i)
            out[k++] = ' ';
        out[k++] = '"';
        memcpy(out + k, argv[i], m);
        k += m;
        out[k++] = '"';
    }
    out[k] = 0;
    return 0;
}

int proc_spawn(proc *p, const char *const *argv, const char *cwd,
               char *err, size_t errn)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    char cmd[4096];
    if (cmdline(argv, cmd, sizeof cmd, err, errn))
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err, errn, "CreatePipe: error %lu", GetLastError());
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    if (!CreateProcessA(argv[0], cmd, NULL, NULL, TRUE,
                        CREATE_NEW_PROCESS_GROUP, NULL, cwd, &si, &pi)) {
        snprintf(err, errn, "cannot run %s in %s: error %lu", argv[0], cwd,
                 GetLastError());
        CloseHandle(rd);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        return -1;
    }
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->pid = pi.dwProcessId;
    p->out = rd;
    return 0;
}

long proc_read(proc *p, char *buf, size_t n)
{
    DWORD got;
    if (!ReadFile((HANDLE)p->out, buf, (DWORD)n, &got, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return (long)got;
}

int proc_wait(proc *p)
{
    DWORD code;
    int rc = -1;
    WaitForSingleObject((HANDLE)p->process, INFINITE);
    if (GetExitCodeProcess((HANDLE)p->process, &code))
        rc = (int)code;
    CloseHandle((HANDLE)p->process);
    CloseHandle((HANDLE)p->out);
    p->process = p->out = NULL;
    return rc;
}

int proc_stop(proc *p)
{
    return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, p->pid) ? 0 : -1;
}

int proc_abspath(const char *prog, char *out, size_t n)
{
    DWORD k = GetFullPathNameA(prog, (DWORD)n, out, NULL);
    if (k == 0 || k >= n)
        return -1;
    return GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES ? -1 : 0;
}
