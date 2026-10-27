/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * inject.c - launcher: starts FlexColor.exe suspended, injects ft_hook.dll
 * (next to inject.exe) via remote LoadLibrary, resumes, and stays attached
 * until the app exits, propagating its exit code.
 *
 * Usage:  inject.exe <path\to\FlexColor.exe>
 *
 * Build: i686-w64-mingw32-gcc -O2 -Wall -Wextra -o inject.exe inject.c
 *
 * Note: the injected DLL path is built as ANSI, so the remote thread must run
 * LoadLibraryA (an earlier draft said LoadLibraryW - that would misread the
 * ANSI path buffer; see README.md).
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef _WIN64
#error "inject must be built 32-bit (FlexColor is a 32-bit app)"
#endif

static void ft_strcpy(char* dst, const char* src) {
  while((*dst++ = *src++) != '\0');
}

static size_t ft_strlen_(const char* s) {
  const char* p = s;
  while(*p) p++;
  return (size_t)(p - s);
}

int main(int argc, char** argv) {
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  char self[MAX_PATH];
  char dllpath[MAX_PATH + 16];
  char* slash;
  size_t dirlen, dlllen;
  LPVOID remote;
  SIZE_T written;
  HANDLE hthread;
  DWORD wres, hmod, exitcode;
  FARPROC loadlib;

  if(argc < 2) {
    fprintf(stderr, "usage: inject.exe <path\\to\\FlexColor.exe>\n");
    return 2;
  }

  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  memset(&pi, 0, sizeof pi);

  if(!CreateProcessA(argv[1], NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                     NULL, &si, &pi)) {
    fprintf(stderr, "inject: CreateProcessA failed (%lu)\n",
            (unsigned long)GetLastError());
    return 1;
  }

  /* ft_hook.dll lives next to inject.exe */
  if(!GetModuleFileNameA(NULL, self, MAX_PATH)) {
    fprintf(stderr, "inject: GetModuleFileNameA failed (%lu)\n",
            (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }
  slash = strrchr(self, '\\');
  if(!slash) {
    fprintf(stderr, "inject: no backslash in own path\n");
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }
  dirlen = (size_t)(slash - self) + 1; /* keep the backslash */
  ft_strcpy(dllpath, self);
  dllpath[dirlen] = '\0';
  ft_strcpy(dllpath + dirlen, "ft_hook.dll");
  dlllen = ft_strlen_(dllpath) + 1; /* include NUL */

  remote = VirtualAllocEx(pi.hProcess, NULL, dlllen, MEM_COMMIT | MEM_RESERVE,
                          PAGE_READWRITE);
  if(!remote) {
    fprintf(stderr, "inject: VirtualAllocEx failed (%lu)\n",
            (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }
  if(!WriteProcessMemory(pi.hProcess, remote, dllpath, dlllen, &written) ||
     written != dlllen) {
    fprintf(stderr, "inject: WriteProcessMemory failed (%lu)\n",
            (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }

  loadlib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
  if(!loadlib) {
    fprintf(stderr, "inject: GetProcAddress(LoadLibraryA) failed (%lu)\n",
            (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }

  hthread =
    CreateRemoteThread(pi.hProcess, NULL, 0,
                       (LPTHREAD_START_ROUTINE)(void*)loadlib, remote, 0, NULL);
  if(!hthread) {
    fprintf(stderr, "inject: CreateRemoteThread failed (%lu)\n",
            (unsigned long)GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }

  /* The remote thread always terminates (LoadLibrary cannot block
   * indefinitely), so wait forever: under on-access AV scanning the first
   * LoadLibrary can exceed any fixed timeout, and timing out here would
   * needlessly kill FlexColor. */
  wres = WaitForSingleObject(hthread, INFINITE);
  hmod = 0;
  if(wres == WAIT_OBJECT_0)
    GetExitCodeThread(hthread, &hmod); /* = HINSTANCE of loaded DLL */
  CloseHandle(hthread);

  if(wres != WAIT_OBJECT_0 || hmod == 0) {
    fprintf(stderr, "inject: remote LoadLibrary failed (wait=%lu hmod=%lu)\n",
            (unsigned long)wres, (unsigned long)hmod);
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
  }

  ResumeThread(pi.hThread);

  /* keep the console attached; propagate the app's exit code */
  WaitForSingleObject(pi.hProcess, INFINITE);
  exitcode = 1;
  GetExitCodeProcess(pi.hProcess, &exitcode);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)exitcode;
}
