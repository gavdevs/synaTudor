#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "internal.h"

typedef struct {
    DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

_Static_assert(sizeof(SYSTEMTIME) == 16, "SYSTEMTIME must have the Windows ABI layout");

__winfnc BOOL QueryPerformanceCounter(uint64_t *counter) {
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        winerr_set_errno();
        return FALSE;
    }

    *counter = time.tv_sec * 1000000lu + time.tv_nsec;
    return TRUE;
}
WINAPI(QueryPerformanceCounter)

__winfnc DWORD GetTickCount() {
    uint64_t counter;
    if(!QueryPerformanceCounter(&counter)) { log_error("QueryPerformanceCounter failed!"); abort(); }
    return (DWORD) counter;
}
WINAPI(GetTickCount)

__winfnc void GetSystemTimeAsFileTime(FILETIME *outTime) {
    struct timeval time;
    assert(gettimeofday(&time, NULL) == 0);

    uint64_t us = time.tv_sec * 10000lu + time.tv_usec;
    outTime->dwLowDateTime =  (DWORD) ((us >>  0) & 0xffffffffu);
    outTime->dwHighDateTime = (DWORD) ((us >> 32) & 0xffffffffu);
}
WINAPI(GetSystemTimeAsFileTime)

__winfnc void GetLocalTime(SYSTEMTIME *outTime) {
    struct timeval time;
    assert(gettimeofday(&time, NULL) == 0);

    struct tm localTime;
    assert(localtime_r(&time.tv_sec, &localTime) != NULL);

    outTime->wYear = (WORD) (localTime.tm_year + 1900);
    outTime->wMonth = (WORD) (localTime.tm_mon + 1);
    outTime->wDayOfWeek = (WORD) localTime.tm_wday;
    outTime->wDay = (WORD) localTime.tm_mday;
    outTime->wHour = (WORD) localTime.tm_hour;
    outTime->wMinute = (WORD) localTime.tm_min;
    outTime->wSecond = (WORD) localTime.tm_sec;
    outTime->wMilliseconds = (WORD) (time.tv_usec / 1000);
}
WINAPI(GetLocalTime)