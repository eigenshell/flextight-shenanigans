/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uiev.c - UI event timeline logger for the FlexColor capture rig.
 *
 * Runs ALONGSIDE a hooked FlexColor session, in any order (WH_MOUSE_LL /
 * WH_KEYBOARD_LL are system-wide; no injection into FlexColor needed — the
 * callbacks run in THIS process). Writes flextrace_uiev.jsonl next to this
 * exe, one compact JSON object per line, each stamped with:
 *
 *   t    GetTickCount() ms — the SAME clock ft_hook.dll stamps into the
 *        ioctl JSONL "t" field. One timeline across both files.
 *   what mouse button event / drag move / wheel / keystroke
 *   target  what was under the cursor: window class, control ID, window
 *        text (via SendMessageTimeout WM_GETTEXT — 100 ms ABORTIFHUNG cap,
 *        never blocks), window rect, up to two ancestor texts (dialog page)
 *   fg   foreground window text (context: which dialog had focus)
 *   title changes of the foreground window, polled at 4 Hz — catches
 *        progress text ("Scanning ... 47%") that changes without input
 *
 * Keystrokes are logged ONLY while a FlexColor.exe process owns the
 * foreground window (pid cache refreshed by the title poll, 4 Hz).
 * Mouse events are logged system-wide — clicks outside FlexColor are useful
 * gap context on a dedicated capture rig.
 *
 * Build (32-bit, same toolchain discipline as the rest — see README):
 *   i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o uiev.exe uiev.c
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <windows.h>

#define UIEV_LOG_NAME "flextrace_uiev.jsonl"
#define UIEV_TITLE_POLL_MS 250
#define UIEV_DRAG_MIN_MS 50 /* throttle move spam while dragging */
#define UIEV_GETTEXT_TIMEOUT 100

static FILE* g_log;
static char g_path[MAX_PATH];
static HHOOK g_hmouse, g_hkeyb;
static DWORD g_flexpid;        /* 0 = FlexColor not running */
static char g_last_title[512]; /* last logged foreground title */
static DWORD g_last_move_t;    /* drag-move throttle timestamp */

/* ---- path ---------------------------------------------------------------- */

static void uiev_path_init(void) {
  char p[MAX_PATH];
  char* slash;
  size_t dirlen, j;

  if (GetModuleFileNameA(NULL, p, sizeof p) == 0) p[0] = '\0';
  slash = strrchr(p, '\\');
  if (!slash) {
    /* no module path (shouldn't happen) — cwd */
    strcpy(g_path, UIEV_LOG_NAME);
    return;
  }
  dirlen = (size_t)(slash - p) + 1;
  for (j = 0; j < dirlen; j++) g_path[j] = p[j];
  for (j = 0; UIEV_LOG_NAME[j]; j++) g_path[dirlen + j] = UIEV_LOG_NAME[j];
  g_path[dirlen + j] = '\0';
}

/* ---- tiny JSON helpers ----------------------------------------------------
 */

/* one JSON string (escaped) — control chars are not expected in window
 * titles but never let a bad byte break the line framing */
static void uiev_json_str(const char* s) {
  fputc('"', g_log);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      fputc('\\', g_log);
      fputc(c, g_log);
    } else if (c < 0x20) {
      fprintf(g_log, "\\u%04x", (unsigned)c);
    } else {
      fputc(c, g_log);
    }
  }
  fputc('"', g_log);
}

static void uiev_wtext(HWND hwnd, char* out, int cap) {
  DWORD_PTR res;

  out[0] = '\0';
  if (hwnd == NULL) return;
  if (SendMessageTimeoutA(hwnd, WM_GETTEXT, (WPARAM)cap, (LPARAM)out,
                          SMTO_ABORTIFHUNG, UIEV_GETTEXT_TIMEOUT, &res) == 0)
    out[0] = '\0';
}

/* ---- FlexColor pid cache ---------------------------------------------------
 */

static void uiev_resolve_flexpid(void) {
  HANDLE snap;
  PROCESSENTRY32 pe;

  g_flexpid = 0;
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  pe.dwSize = sizeof pe;
  if (Process32First(snap, &pe)) {
    do {
      char name[MAX_PATH];
      size_t i;
      for (i = 0; pe.szExeFile[i] && i < sizeof name - 1; i++)
        name[i] = (char)tolower((unsigned char)pe.szExeFile[i]);
      name[i] = '\0';
      if (strcmp(name, "flexcolor.exe") == 0) {
        g_flexpid = pe.th32ProcessID;
        break;
      }
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
}

/* ---- mouse event logging ----------------------------------------------------
 */

/* kind: "mdown" "mup" "move" "wheel". btn may be NULL. wheel != 0 only for
 * wheel. */
static void uiev_log_mouse(const char* kind, int x, int y, const char* btn,
                           int wheel, DWORD t) {
  POINT pt;
  HWND w, fg, parent;
  char cls[128], wtxt[512], atxt[512], ftxt[512];
  RECT rc;
  int ctl, anc;

  pt.x = x;
  pt.y = y;
  w = WindowFromPoint(pt);
  fg = GetForegroundWindow();

  fprintf(g_log,
          "{\"tag\":\"uiev\",\"t\":%lu,\"kind\":\"%s\",\"x\":%d,\"y\":%d",
          (unsigned long)t, kind, x, y);
  if (btn != NULL) fprintf(g_log, ",\"btn\":\"%s\"", btn);
  if (wheel != 0) fprintf(g_log, ",\"wheel\":%d", wheel);

  if (w != NULL) {
    GetClassNameA(w, cls, sizeof cls);
    uiev_wtext(w, wtxt, sizeof wtxt);
    ctl = GetDlgCtrlID(w);
    GetWindowRect(w, &rc);
    fprintf(g_log, ",\"class\":");
    uiev_json_str(cls);
    fprintf(g_log, ",\"ctl\":%d,\"rect\":[%ld,%ld,%ld,%ld],\"wtext\":", ctl,
            (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom);
    uiev_json_str(wtxt);
    /* up to two ancestors: the dialog/page chain gives click context even
     * when the clicked child itself has no label */
    parent = GetParent(w);
    for (anc = 1; anc <= 2 && parent != NULL; anc++) {
      uiev_wtext(parent, atxt, sizeof atxt);
      fprintf(g_log, ",\"anc%d\":", anc);
      uiev_json_str(atxt);
      parent = GetParent(parent);
    }
  }
  uiev_wtext(fg, ftxt, sizeof ftxt);
  fprintf(g_log, ",\"fg\":");
  uiev_json_str(ftxt);
  fputs("}\n", g_log);
  fflush(g_log);
}

static LRESULT CALLBACK uiev_mouse_ll(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION) {
    MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
    DWORD t = GetTickCount();

    switch (wp) {
      case WM_LBUTTONDOWN:
        uiev_log_mouse("mdown", m->pt.x, m->pt.y, "l", 0, t);
        break;
      case WM_LBUTTONUP:
        uiev_log_mouse("mup", m->pt.x, m->pt.y, "l", 0, t);
        break;
      case WM_RBUTTONDOWN:
        uiev_log_mouse("mdown", m->pt.x, m->pt.y, "r", 0, t);
        break;
      case WM_RBUTTONUP:
        uiev_log_mouse("mup", m->pt.x, m->pt.y, "r", 0, t);
        break;
      case WM_MBUTTONDOWN:
        uiev_log_mouse("mdown", m->pt.x, m->pt.y, "m", 0, t);
        break;
      case WM_MBUTTONUP:
        uiev_log_mouse("mup", m->pt.x, m->pt.y, "m", 0, t);
        break;
      case WM_MOUSEWHEEL: {
        short delta = (short)HIWORD(m->mouseData);
        uiev_log_mouse("wheel", m->pt.x, m->pt.y, NULL, delta > 0 ? 1 : -1, t);
        break;
      }
      case WM_MOUSEMOVE:
        /* only while a button is held (a drag), throttled */
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) {
          if (t - g_last_move_t >= UIEV_DRAG_MIN_MS) {
            g_last_move_t = t;
            uiev_log_mouse("move", m->pt.x, m->pt.y, NULL, 0, t);
          }
        }
        break;
      default:
        break;
    }
  }
  return CallNextHookEx(g_hmouse, code, wp, lp);
}

/* ---- keyboard logging (FlexColor-scoped) ------------------------------------
 */

static LRESULT CALLBACK uiev_key_ll(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION &&
      (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) &&
      g_flexpid != 0) {
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    char ftxt[512];

    GetWindowThreadProcessId(fg, &pid);
    if (pid == g_flexpid) {
      uiev_wtext(fg, ftxt, sizeof ftxt);
      fprintf(g_log,
              "{\"tag\":\"uiev\",\"t\":%lu,\"kind\":\"key\",\"vk\":%lu,\"fg\":",
              (unsigned long)GetTickCount(), (unsigned long)k->vkCode);
      uiev_json_str(ftxt);
      fputs("}\n", g_log);
      fflush(g_log);
    }
  }
  return CallNextHookEx(g_hkeyb, code, wp, lp);
}

/* ---- foreground title poll
 * ----------------------------------------------------- */

static void uiev_title_poll(void) {
  HWND fg;
  char ftxt[512];

  uiev_resolve_flexpid(); /* keep the FlexColor pid cache fresh */
  fg = GetForegroundWindow();
  uiev_wtext(fg, ftxt, sizeof ftxt);
  if (strcmp(ftxt, g_last_title) != 0) {
    fprintf(g_log, "{\"tag\":\"uiev\",\"t\":%lu,\"kind\":\"title\",\"fg\":",
            (unsigned long)GetTickCount());
    uiev_json_str(ftxt);
    fputs("}\n", g_log);
    fflush(g_log);
    strncpy(g_last_title, ftxt, sizeof g_last_title - 1);
    g_last_title[sizeof g_last_title - 1] = '\0';
  }
}

/* ---- entry point
 * ----------------------------------------------------------------- */

int main(void) {
  MSG msg;

  uiev_path_init();
  g_log = fopen(g_path, "ab");
  if (g_log == NULL) return 1;

  g_hmouse =
    SetWindowsHookEx(WH_MOUSE_LL, uiev_mouse_ll, GetModuleHandle(NULL), 0);
  g_hkeyb =
    SetWindowsHookEx(WH_KEYBOARD_LL, uiev_key_ll, GetModuleHandle(NULL), 0);
  if (g_hmouse == NULL || g_hkeyb == NULL) {
    fprintf(g_log,
            "{\"tag\":\"uiev_loaded\",\"status\":\"failed\",\"mouse\":%d,"
            "\"key\":%d}\n",
            g_hmouse != NULL, g_hkeyb != NULL);
    fflush(g_log);
    return 1;
  }
  fprintf(g_log, "{\"tag\":\"uiev_loaded\",\"status\":\"ok\",\"path\":");
  uiev_json_str(g_path);
  fputs("}\n", g_log);
  fflush(g_log);

  SetTimer(NULL, 1, UIEV_TITLE_POLL_MS, NULL);

  /* thread timer (NULL hwnd): WM_TIMER arrives with no window proc, handle
   * it in the loop directly; everything else dispatches normally */
  while (GetMessage(&msg, NULL, 0, 0) > 0) {
    if (msg.message == WM_TIMER)
      uiev_title_poll();
    else {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
  return 0; /* unreachable in practice; closed via console/taskkill */
}
