/*
 * Offline check of the sync-event lifetime rules.
 *
 * The vendor driver closes event handles during teardown while its own threads
 * may still be waiting on or signalling the same events. Windows keeps the
 * object alive until the pending operation finishes; libtudor must do the same.
 * Before the fix, closing the handle under a blocked waiter destroyed the
 * event's pthread condvar/mutex in use and aborted the process
 * ("'pthread_mutex_destroy(&evt->lock)' failed: 16").
 *
 * Also checks that a finite WaitForSingleObject() timeout is a real deadline:
 * pthread_cond_timedwait() needs an absolute time, not a relative one.
 *
 * No device access of any kind.
 */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "winapi/internal.h"

//The Win32 entry points are resolved dynamically by the loader and use the
//Microsoft x64 ABI, so windows.h doesn't declare them.
__winfnc HANDLE CreateEventA(void *attrs, BOOL manual_reset, BOOL initial_state, const char *name);
__winfnc BOOL SetEvent(HANDLE handle);
__winfnc BOOL CloseHandle(HANDLE handle);
__winfnc DWORD WaitForSingleObject(HANDLE handle, DWORD timeout);

struct waiter {
    HANDLE evt;
    DWORD result;
    long elapsed_ms;
};

static long now_ms() {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000L + time.tv_nsec / 1000000L;
}

static void *waiter_main(void *arg) {
    struct waiter *waiter = (struct waiter*) arg;

    long start = now_ms();
    waiter->result = WaitForSingleObject(waiter->evt, 60);
    waiter->elapsed_ms = now_ms() - start;

    return NULL;
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    unsigned base = win_event_live_count();

    //A finite wait is a real deadline, not an instant timeout
    HANDLE evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    assert(evt);
    assert(win_event_live_count() == base + 1);

    struct waiter waiter = { .evt = evt, .result = 0xdeadbeef, .elapsed_ms = -1 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, waiter_main, &waiter) == 0);

    //Let the waiter block on the event
    usleep(10000);
    assert(win_event_live_count() == base + 1);

    //Closing the handle under the blocked waiter must keep the object alive
    //until the wait returns instead of tearing down the condvar/mutex in use
    assert(CloseHandle(evt));
    assert(win_event_live_count() == base + 1);
    assert(pthread_join(thread, NULL) == 0);
    assert(waiter.result == WAIT_TIMEOUT);
    assert(waiter.elapsed_ms >= 40);
    assert(win_event_live_count() == base);

    //Signalling and closing back to back must stay safe
    for(int i = 0; i < 1000; i++) {
        HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
        assert(e);
        assert(SetEvent(e));
        assert(CloseHandle(e));
        assert(win_event_live_count() == base);
    }

    //Auto-reset semantics still hold with a live handle
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    assert(e);
    assert(SetEvent(e));
    assert(WaitForSingleObject(e, 1000) == 0);
    assert(WaitForSingleObject(e, 1000) == WAIT_TIMEOUT);
    assert(CloseHandle(e));
    assert(win_event_live_count() == base);

    puts("[TEST] sync event lifetime checks passed");
    return 0;
}
