#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "internal.h"
#include <tudor/log.h>

/* Minimal USER32 window layer for the HP "110" driver's CPowerStateWindow
 * (a hidden message window used for power-broadcast handling). We don't deliver
 * real messages (not needed for enroll/verify), but we implement enough that
 * the driver can register a class, create the window (incl. WM_NCCREATE /
 * WM_CREATE so it can stash its 'this' pointer via SetWindowLongPtrW), and run
 * an (empty) message loop without crashing. */

#define WM_CREATE   0x0001
#define WM_NCCREATE 0x0081
#define GWLP_WNDPROC  (-4)
#define GWLP_USERDATA (-21)

typedef ULONG_PTR UINT_PTR;
typedef LONG_PTR __winfnc (*WNDPROC)(void *hwnd, unsigned int msg, UINT_PTR wparam, LONG_PTR lparam);

typedef struct {
    unsigned int cbSize, style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    void *hInstance, *hIcon, *hCursor, *hbrBackground;
    const char16_t *lpszMenuName, *lpszClassName;
    void *hIconSm;
} WNDCLASSEXW;

typedef struct {
    void *lpCreateParams, *hInstance, *hMenu, *hwndParent;
    int cy, cx, y, x;
    long style;
    const char16_t *lpszName, *lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTW;

struct wndclass { char *name; WNDPROC proc; struct wndclass *next; };
static struct wndclass *classes;

struct window { WNDPROC proc; LONG_PTR userdata; LONG_PTR extra[32]; };

__winfnc WORD RegisterClassExW(const WNDCLASSEXW *cls) {
    if(!cls) return 0;
    struct wndclass *c = (struct wndclass*) calloc(1, sizeof(*c));
    c->name = winstr_to_str(cls->lpszClassName);
    c->proc = cls->lpfnWndProc;
    c->next = classes;
    classes = c;
    return 1; /* non-zero ATOM */
}
WINAPI(RegisterClassExW)

__winfnc BOOL UnregisterClassW(const char16_t *name, void *inst) {
    (void) name; (void) inst;
    return TRUE;
}
WINAPI(UnregisterClassW)

__winfnc void *CreateWindowExW(DWORD exstyle, const char16_t *clsname, const char16_t *winname,
                               DWORD style, int x, int y, int w, int h,
                               void *parent, void *menu, void *inst, void *param) {
    struct window *win = (struct window*) calloc(1, sizeof(*win));
    if(!win) return NULL;

    char *cname = winstr_to_str(clsname);
    for(struct wndclass *c = classes; c; c = c->next) {
        if(c->name && cname && strcmp(c->name, cname) == 0) { win->proc = c->proc; break; }
    }
    free(cname);

    if(win->proc) {
        CREATESTRUCTW cs = {0};
        cs.lpCreateParams = param; cs.hInstance = inst; cs.hMenu = menu; cs.hwndParent = parent;
        cs.cy = h; cs.cx = w; cs.y = y; cs.x = x; cs.style = (long) style;
        cs.lpszName = winname; cs.lpszClass = clsname; cs.dwExStyle = exstyle;
        if(!win->proc(win, WM_NCCREATE, 0, (LONG_PTR) &cs)) { free(win); return NULL; }
        win->proc(win, WM_CREATE, 0, (LONG_PTR) &cs);
    }
    return win;
}
WINAPI(CreateWindowExW)

__winfnc BOOL DestroyWindow(void *hwnd) { free(hwnd); return TRUE; }
WINAPI(DestroyWindow)

__winfnc LONG_PTR SetWindowLongPtrW(void *hwnd, int idx, LONG_PTR val) {
    struct window *w = (struct window*) hwnd;
    LONG_PTR old = 0;
    if(!w) return 0;
    if(idx == GWLP_USERDATA)      { old = w->userdata; w->userdata = val; }
    else if(idx == GWLP_WNDPROC)  { old = (LONG_PTR) w->proc; w->proc = (WNDPROC) val; }
    else if(idx >= 0 && idx < (int) sizeof(w->extra) - 8) { old = w->extra[idx/8]; w->extra[idx/8] = val; }
    return old;
}
WINAPI(SetWindowLongPtrW)

__winfnc LONG_PTR GetWindowLongPtrW(void *hwnd, int idx) {
    struct window *w = (struct window*) hwnd;
    if(!w) return 0;
    if(idx == GWLP_USERDATA)      return w->userdata;
    if(idx == GWLP_WNDPROC)       return (LONG_PTR) w->proc;
    if(idx >= 0 && idx < (int) sizeof(w->extra) - 8) return w->extra[idx/8];
    return 0;
}
WINAPI(GetWindowLongPtrW)

__winfnc LONG_PTR DefWindowProcW(void *hwnd, unsigned int msg, UINT_PTR wp, LONG_PTR lp) {
    (void) hwnd; (void) wp; (void) lp;
    if(msg == WM_NCCREATE) return 1; /* allow window creation */
    return 0;
}
WINAPI(DefWindowProcW)

__winfnc LONG_PTR CallWindowProcW(WNDPROC proc, void *hwnd, unsigned int msg, UINT_PTR wp, LONG_PTR lp) {
    if(!proc) return 0;
    return proc(hwnd, msg, wp, lp);
}
WINAPI(CallWindowProcW)

/* Message loop: report no messages / WM_QUIT so any driver pump exits cleanly. */
__winfnc BOOL GetMessageW(void *msg, void *hwnd, unsigned int min, unsigned int max) {
    (void) msg; (void) hwnd; (void) min; (void) max;
    return 0; /* WM_QUIT */
}
WINAPI(GetMessageW)

__winfnc BOOL PeekMessageW(void *msg, void *hwnd, unsigned int min, unsigned int max, unsigned int rem) {
    (void) msg; (void) hwnd; (void) min; (void) max; (void) rem;
    return 0; /* no message available */
}
WINAPI(PeekMessageW)

__winfnc BOOL TranslateMessage(const void *msg) { (void) msg; return FALSE; }
WINAPI(TranslateMessage)

__winfnc LONG_PTR DispatchMessageW(const void *msg) { (void) msg; return 0; }
WINAPI(DispatchMessageW)

__winfnc BOOL PostThreadMessageW(DWORD tid, unsigned int msg, UINT_PTR wp, LONG_PTR lp) {
    (void) tid; (void) msg; (void) wp; (void) lp;
    return TRUE;
}
WINAPI(PostThreadMessageW)

__winfnc BOOL PostMessageW(void *hwnd, unsigned int msg, UINT_PTR wp, LONG_PTR lp) {
    (void) hwnd; (void) msg; (void) wp; (void) lp;
    return TRUE;
}
WINAPI(PostMessageW)

__winfnc void PostQuitMessage(int code) { (void) code; }
WINAPI(PostQuitMessage)

/* WTSAPI32 session-change notifications — not needed for enroll/verify; succeed. */
__winfnc BOOL WTSRegisterSessionNotification(void *hwnd, DWORD flags) {
    (void) hwnd; (void) flags;
    return TRUE;
}
WINAPI(WTSRegisterSessionNotification)

__winfnc BOOL WTSUnRegisterSessionNotification(void *hwnd) {
    (void) hwnd;
    return TRUE;
}
WINAPI(WTSUnRegisterSessionNotification)
