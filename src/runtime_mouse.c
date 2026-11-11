/* The mouse for the game's own sceMouse path (GOW3_MOUSE=1; research/docs/study-mouse-input.md, step 2).
 * Windows: a Raw Input reader on its own thread, the method MGS2 settled on (MGS2-Lab
 * research/docs/mouse-input.md 3.11): a message-only window, normal priority, at most one wake-up a
 * millisecond, every queued report drained in one GetRawInputBuffer batch. SDL's relative mode is not
 * used: its reader wakes per report at TIME_CRITICAL, and Windows keeps one raw mouse registration per
 * process, so it would replace this one. The counts go into atomics; sceMouseRead (the game thread,
 * once a frame) only takes them, no call into Windows. The window thread (gpu/shim/window.cpp) decides
 * when the game has the mouse (runtime_mouse_capture). Elsewhere: no mouse (the stubs' old answer). */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int enabled=-1;

int runtime_mouse_enabled(void) {
#ifdef _WIN32
    if (enabled<0) { const char *v=getenv("GOW3_MOUSE"); enabled=v && strcmp(v,"1")==0; }
    return enabled;
#else
    return 0;
#endif
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define MOUSE_BUTTONS 0x1fu /* sceMouse: 1 left, 2 right, 4 middle, 8 thumb 1, 16 thumb 2 */

static atomic_int captured;               /* the window thread: the game has the mouse */
static atomic_uint capture_serial;        /* +1 at each capture: the reader blocks held buttons */
static atomic_llong total_x, total_y, total_wheel; /* counts not yet read, wheel in WHEEL_DELTA units */
static atomic_uint shown_buttons;         /* buttons the game sees */
static atomic_ullong reports, wakes, batches; /* statistics for the log */
static HWND reader_window;

/* Raw Input button flags -> sceMouse bits: down / up pairs in RI_MOUSE_BUTTON_1..5 order. */
static unsigned apply_buttons(unsigned held,USHORT flags) {
    static const USHORT down[5]={RI_MOUSE_BUTTON_1_DOWN,RI_MOUSE_BUTTON_2_DOWN,RI_MOUSE_BUTTON_3_DOWN,
                                 RI_MOUSE_BUTTON_4_DOWN,RI_MOUSE_BUTTON_5_DOWN};
    static const USHORT up[5]={RI_MOUSE_BUTTON_1_UP,RI_MOUSE_BUTTON_2_UP,RI_MOUSE_BUTTON_3_UP,
                               RI_MOUSE_BUTTON_4_UP,RI_MOUSE_BUTTON_5_UP};
    for (int i=0;i<5;i++) {
        if (flags&down[i]) held|=1u<<i;
        if (flags&up[i]) held&=~(1u<<i);
    }
    return held;
}

/* Every queued report in one batch. The physical buttons are followed even without capture; a
 * capture blocks the buttons held at that moment (the click that took the mouse) until each is let
 * go, so the game never sees half a click. */
static void drain(void) {
    static uint64_t buffer[8*1024]; /* 64 KiB, 8-byte aligned as Raw Input blocks are */
    static unsigned held, blocked, serial;
    long long dx=0, dy=0, wheel=0, n=0;
    for (;;) {
        UINT size=sizeof(buffer);
        const UINT count=GetRawInputBuffer((RAWINPUT *)buffer,&size,sizeof(RAWINPUTHEADER));
        if (count==0 || count==(UINT)-1) break;
        const unsigned char *at=(const unsigned char *)buffer;
        for (UINT i=0;i<count;i++) {
            const RAWINPUT *ri=(const RAWINPUT *)at;
            /* NEXTRAWINPUTBLOCK: MinGW's header lacks the QWORD it needs */
            at+=(ri->header.dwSize+7u)&~7u;
            if (ri->header.dwType!=RIM_TYPEMOUSE) continue;
            const RAWMOUSE *m=&ri->data.mouse;
            if (!(m->usFlags&MOUSE_MOVE_ABSOLUTE)) { dx+=m->lLastX; dy+=m->lLastY; }
            held=apply_buttons(held,m->usButtonFlags);
            if (m->usButtonFlags&RI_MOUSE_WHEEL) wheel+=(SHORT)m->usButtonData;
            n++;
        }
    }
    atomic_fetch_add_explicit(&batches,1,memory_order_relaxed);
    if (!n) return;
    atomic_fetch_add_explicit(&reports,(unsigned long long)n,memory_order_relaxed);
    const unsigned now=atomic_load(&capture_serial);
    if (now!=serial) { serial=now; blocked=MOUSE_BUTTONS; }
    if (atomic_load(&captured)) {
        atomic_fetch_add(&total_x,dx); atomic_fetch_add(&total_y,dy); atomic_fetch_add(&total_wheel,wheel);
        atomic_store(&shown_buttons,held&~blocked);
    } else {
        atomic_store(&shown_buttons,0);
    }
    blocked&=held;
}

static DWORD WINAPI reader_thread(void *unused) {
    (void)unused;
    WNDCLASSEXW wc; memset(&wc,0,sizeof(wc)); wc.cbSize=sizeof(wc);
    wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"MediEvilRawMouse";
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,wc.hInstance,NULL);
    /* INPUTSINK: a message-only window is never in the foreground; the window thread's capture
     * decides when counts reach the game. Ordinary mouse messages stay on (the menu, the cursor). */
    RAWINPUTDEVICE rid={1,2,RIDEV_INPUTSINK,hwnd};
    if (!hwnd || !RegisterRawInputDevices(&rid,1,sizeof(rid))) {
        printf("Mouse: Raw Input not available (window %s, error %lu); the game sees no mouse movement\n",
               hwnd ? "ok" : "failed",GetLastError());
        fflush(stdout);
        return 0;
    }
    reader_window=hwnd;
    printf("Mouse: Raw Input reader running (batched, at most one wake-up a millisecond, normal priority)\n");
    fflush(stdout);
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    MSG msg;
    for (;;) {
        MsgWaitForMultipleObjectsEx(0,NULL,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        atomic_fetch_add_explicit(&wakes,1,memory_order_relaxed);
        drain();
        while (PeekMessageW(&msg,NULL,0,WM_INPUT-1,PM_REMOVE) || PeekMessageW(&msg,NULL,WM_INPUT+1,0xFFFFFFFF,PM_REMOVE)) {
            if (msg.message==WM_QUIT) return 0;
            DispatchMessageW(&msg);
        }
        if (timer) {
            LARGE_INTEGER due; due.QuadPart=-10000; /* 1 ms */
            SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE);
            WaitForSingleObject(timer,5);
        } else {
            Sleep(1);
        }
    }
}

void runtime_mouse_start(void) {
    static atomic_int started;
    if (!runtime_mouse_enabled() || atomic_exchange(&started,1)) return;
    HANDLE thread=CreateThread(NULL,0,reader_thread,NULL,0,NULL);
    if (thread) CloseHandle(thread);
}

/* The window thread, on a change only. A release drops counts not yet read. Logs the reader's
 * rates since the last change and whether the raw mouse registration is still this reader's. */
void runtime_mouse_capture(int on) {
    static ULONGLONG since; static unsigned long long last_reports, last_wakes, last_batches;
    if (on) atomic_fetch_add(&capture_serial,1);
    atomic_store(&captured,on);
    if (!on) {
        atomic_store(&total_x,0); atomic_store(&total_y,0); atomic_store(&total_wheel,0);
        atomic_store(&shown_buttons,0);
    }
    const ULONGLONG now=GetTickCount64();
    const unsigned long long r=atomic_load(&reports), w=atomic_load(&wakes), b=atomic_load(&batches);
    const double s=since ? (double)(now-since)/1000.0 : 0.0;
    RAWINPUTDEVICE devices[8]; UINT count=8; HWND owner=NULL;
    const UINT got=GetRegisteredRawInputDevices(devices,&count,sizeof(RAWINPUTDEVICE));
    for (UINT i=0;i<got && i<8;i++) if (devices[i].usUsagePage==1 && devices[i].usUsage==2) owner=devices[i].hwndTarget;
    printf("Mouse: %s; last %.1f s: %llu reports (%.0f/s), %llu wake-ups (%.0f/s), %llu batches; raw mouse %s\n",
           on ? "captured" : "released",s,r-last_reports,s>0 ? (double)(r-last_reports)/s : 0.0,w-last_wakes,
           s>0 ? (double)(w-last_wakes)/s : 0.0,b-last_batches,
           !reader_window ? "reader not running" : owner==reader_window ? "registered to the reader" :
           owner ? "TAKEN BY ANOTHER WINDOW" : "NOT REGISTERED");
    fflush(stdout);
    since=now; last_reports=r; last_wakes=w; last_batches=b;
}

int runtime_mouse_take(int32_t *x,int32_t *y,int32_t *wheel,uint32_t *buttons) {
    if (!runtime_mouse_enabled()) return 0;
    const long long dx=atomic_exchange(&total_x,0), dy=atomic_exchange(&total_y,0);
    const long long w=atomic_exchange(&total_wheel,0), notches=w/WHEEL_DELTA;
    if (w-notches*WHEEL_DELTA) atomic_fetch_add(&total_wheel,w-notches*WHEEL_DELTA);
    *x=(int32_t)dx; *y=(int32_t)dy; *wheel=(int32_t)notches;
    *buttons=atomic_load(&shown_buttons);
    return 1;
}
#else
void runtime_mouse_start(void) {}
void runtime_mouse_capture(int on) { (void)on; }
int runtime_mouse_take(int32_t *x,int32_t *y,int32_t *wheel,uint32_t *buttons) {
    (void)x; (void)y; (void)wheel; (void)buttons; return 0;
}
#endif
