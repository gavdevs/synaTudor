#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include "winapi/internal.h"

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef void (__winfnc *get_local_time_fnc)(SYSTEMTIME *);

__winfnc void GetLocalTime(SYSTEMTIME *outTime);

//Unused by these tests, but required by time.c and its constructor registrations.
void __register_windows_api(struct __winapi_descr *descr) { (void) descr; }
void winerr_set_errno() {}
pthread_mutex_t LOG_LOCK = PTHREAD_MUTEX_INITIALIZER;
enum log_level LOG_LEVEL = LOG_ERROR;

static struct timeval fixedTime = { .tv_sec = 1704069004, .tv_usec = 987654 };

int __wrap_gettimeofday(struct timeval *time, void *timezone) {
    (void) timezone;
    *time = fixedTime;
    return 0;
}

static void check_fields(SYSTEMTIME *actual, const WORD expected[8]) {
    assert(sizeof(SYSTEMTIME) == 16);
    assert(memcmp(actual, expected, sizeof(SYSTEMTIME)) == 0);
}

static void expected_from_timeval(SYSTEMTIME *outTime, const struct timeval *time) {
    struct tm localTime;
    assert(localtime_r(&time->tv_sec, &localTime) != NULL);
    outTime->wYear = (WORD) (localTime.tm_year + 1900);
    outTime->wMonth = (WORD) (localTime.tm_mon + 1);
    outTime->wDayOfWeek = (WORD) localTime.tm_wday;
    outTime->wDay = (WORD) localTime.tm_mday;
    outTime->wHour = (WORD) localTime.tm_hour;
    outTime->wMinute = (WORD) localTime.tm_min;
    outTime->wSecond = (WORD) localTime.tm_sec;
    outTime->wMilliseconds = (WORD) (time->tv_usec / 1000);
}

static void check_exported_library(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    assert(library != NULL);
    void *symbol = dlsym(library, "GetLocalTime");
    assert(symbol != NULL);
    void *(*resolveWindowsApi)(const char *name);
    void *resolverSymbol = dlsym(library, "resolve_windows_api");
    assert(resolverSymbol != NULL);
    memcpy(&resolveWindowsApi, &resolverSymbol, sizeof(resolveWindowsApi));
    assert(resolveWindowsApi("GetLocalTime") == symbol);

    get_local_time_fnc getLocalTime;
    memcpy(&getLocalTime, &symbol, sizeof(getLocalTime));

    //2024-01-01 00:30:04.987654 UTC -> 2023-12-31 19:30:04.987 EST.
    assert(setenv("TZ", "EST5", 1) == 0);
    tzset();
    struct timeval before, after;
    SYSTEMTIME expectedBefore, expectedAfter, actual;
    assert(syscall(SYS_gettimeofday, &before, NULL) == 0);
    getLocalTime(&actual);
    assert(syscall(SYS_gettimeofday, &after, NULL) == 0);
    expected_from_timeval(&expectedBefore, &before);
    expected_from_timeval(&expectedAfter, &after);
    assert(memcmp(&actual, &expectedBefore, sizeof(actual)) == 0 ||
           memcmp(&actual, &expectedAfter, sizeof(actual)) == 0);
    assert(dlclose(library) == 0);
    puts("exported GetLocalTime: EST5 local-time check passed");
}

int main(int argc, char **argv) {
    SYSTEMTIME actual;

    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    GetLocalTime(&actual);
    const WORD utc[8] = { 2024, 1, 1, 1, 0, 30, 4, 987 };
    check_fields(&actual, utc);
    puts("GetLocalTime: UTC field order, weekday, date, and milliseconds passed");

    assert(setenv("TZ", "EST5", 1) == 0);
    tzset();
    GetLocalTime(&actual);
    const WORD est[8] = { 2023, 12, 0, 31, 19, 30, 4, 987 };
    check_fields(&actual, est);
    puts("GetLocalTime: timezone conversion and year boundary passed");

    if(argc == 2) check_exported_library(argv[1]);
    return 0;
}
