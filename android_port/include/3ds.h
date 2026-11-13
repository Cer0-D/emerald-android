/*
 * The part of libctru the reused 3DS native units need on Android
 * (3ds_data.c, 3ds_settings.c, 3ds_prof.c, 3ds_scenario.c): types, the
 * system tick, light locks and events, threads. Everything maps onto
 * pthreads and CLOCK_MONOTONIC; nothing here touches graphics or services.
 */
#ifndef ANDROID_SHIM_3DS_H
#define ANDROID_SHIM_3DS_H

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef s32 Result;
typedef u32 Handle;

#define R_SUCCEEDED(r) ((Result)(r) >= 0)
#define R_FAILED(r) ((Result)(r) < 0)
#define U64_MAX UINT64_MAX
#define CUR_THREAD_HANDLE 0xFFFF8000

/* The 3DS's ARM11 tick rate: the tick unit every caller divides by. */
#define SYSCLOCK_ARM11 268111856ULL

static inline u64 svcGetSystemTick(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* ns -> ticks without overflow for centuries of uptime */
    return (u64)ts.tv_sec * SYSCLOCK_ARM11 + (u64)ts.tv_nsec * SYSCLOCK_ARM11 / 1000000000ULL;
}

static inline void svcSleepThread(s64 ns)
{
    struct timespec ts;

    if (ns <= 0)
    {
        sched_yield();
        return;
    }
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    nanosleep(&ts, NULL);
}

static inline Result svcGetThreadPriority(s32 *priority, Handle handle)
{
    (void)handle;
    *priority = 0x30;
    return 0;
}

static inline void svcOutputDebugString(const char *text, int length)
{
    (void)text;
    (void)length;
}

typedef enum { USERBREAK_PANIC = 0, USERBREAK_ASSERT = 1, USERBREAK_USER = 2 } UserBreakType;
static inline void svcBreak(UserBreakType reason)
{
    (void)reason;
    abort();
}

/* The system core's share for the application (ctr_voxel.c's page stream):
 * Android schedules the worker on any core, as if 30% were granted. */
static inline Result APT_GetAppCpuTimeLimit(u32 *percent)
{
    *percent = 30;
    return 0;
}
static inline Result APT_SetAppCpuTimeLimit(u32 percent)
{
    (void)percent;
    return 0;
}

/* Kernel and firmware versions (3ds_prof.c's identity line). */
#define GET_VERSION_MAJOR(v) (((v) >> 24) & 0xFF)
#define GET_VERSION_MINOR(v) (((v) >> 16) & 0xFF)
#define GET_VERSION_REVISION(v) (((v) >> 8) & 0xFF)
static inline u32 osGetKernelVersion(void) { return 0; }
static inline u32 osGetFirmVersion(void) { return 0; }

/* ── Light locks ───────────────────────────────────────────────────────
 * An s32 like libctru's (CtrLock is one too, and statics start at 1 or 0):
 * 0 or 1 unlocked, 2 locked, 3 locked with waiters. A futex mutex. */
typedef s32 LightLock;

static inline void CtrFutexWait(volatile s32 *addr, s32 value)
{
    syscall(__NR_futex, addr, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0);
}
static inline void CtrFutexWake(volatile s32 *addr, int count)
{
    syscall(__NR_futex, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}
static inline void LightLock_Init(LightLock *lock) { __atomic_store_n(lock, 1, __ATOMIC_RELEASE); }
static inline int LightLock_TryLock(LightLock *lock)
{
    s32 c = __atomic_load_n(lock, __ATOMIC_RELAXED);
    if (c > 1)
        return 1;
    return __atomic_compare_exchange_n(lock, &c, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : 1;
}
static inline void LightLock_Lock(LightLock *lock)
{
    if (LightLock_TryLock(lock) == 0)
        return;
    while (__atomic_exchange_n(lock, 3, __ATOMIC_ACQUIRE) > 1)
        CtrFutexWait(lock, 3);
}
static inline void LightLock_Unlock(LightLock *lock)
{
    if (__atomic_exchange_n(lock, 1, __ATOMIC_RELEASE) == 3)
        CtrFutexWake(lock, 1);
}

/* ── Light events ─────────────────────────────────────────────────────── */
typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool signalled;
    int mode;
} LightEvent;

static inline void LightEvent_Init(LightEvent *e, ResetType mode)
{
    pthread_mutex_init(&e->mutex, NULL);
    pthread_cond_init(&e->cond, NULL);
    e->signalled = false;
    e->mode = mode;
}
static inline void LightEvent_Clear(LightEvent *e)
{
    pthread_mutex_lock(&e->mutex);
    e->signalled = false;
    pthread_mutex_unlock(&e->mutex);
}
static inline void LightEvent_Signal(LightEvent *e)
{
    pthread_mutex_lock(&e->mutex);
    e->signalled = true;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->mutex);
}
static inline void LightEvent_Wait(LightEvent *e)
{
    pthread_mutex_lock(&e->mutex);
    while (!e->signalled)
        pthread_cond_wait(&e->cond, &e->mutex);
    if (e->mode == RESET_ONESHOT)
        e->signalled = false;
    pthread_mutex_unlock(&e->mutex);
}
/* 1 if it timed out, 0 if signalled. */
static inline int LightEvent_WaitTimeout(LightEvent *e, s64 ns)
{
    struct timespec until;
    int timedOut = 0;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(ns / 1000000000LL);
    until.tv_nsec += (long)(ns % 1000000000LL);
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&e->mutex);
    while (!e->signalled && !timedOut)
        timedOut = pthread_cond_timedwait(&e->cond, &e->mutex, &until) != 0;
    if (e->signalled && e->mode == RESET_ONESHOT)
        e->signalled = false;
    pthread_mutex_unlock(&e->mutex);
    return timedOut && !e->signalled;
}

/* ── glibc's fopencookie, on bionic's funopen (3ds_data.c's pak streams) ─
 * Only where _GNU_SOURCE asks for it, as glibc does: funopen is BSD. */
#ifdef _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
typedef ssize_t cookie_read_function_t(void *, char *, size_t);
typedef ssize_t cookie_write_function_t(void *, const char *, size_t);
typedef int cookie_seek_function_t(void *, off_t *, int);
typedef int cookie_close_function_t(void *);
typedef struct
{
    cookie_read_function_t *read;
    cookie_write_function_t *write;
    cookie_seek_function_t *seek;
    cookie_close_function_t *close;
} cookie_io_functions_t;

struct CtrCookie
{
    void *cookie;
    cookie_io_functions_t io;
};

static int CtrCookieRead(void *p, char *buf, int n)
{
    struct CtrCookie *c = (struct CtrCookie *)p;
    return c->io.read ? (int)c->io.read(c->cookie, buf, (size_t)n) : -1;
}
static fpos_t CtrCookieSeek(void *p, fpos_t offset, int whence)
{
    struct CtrCookie *c = (struct CtrCookie *)p;
    off_t at = (off_t)offset;
    if (!c->io.seek || c->io.seek(c->cookie, &at, whence) != 0)
        return -1;
    return (fpos_t)at;
}
static int CtrCookieClose(void *p)
{
    struct CtrCookie *c = (struct CtrCookie *)p;
    int result = c->io.close ? c->io.close(c->cookie) : 0;
    free(c);
    return result;
}
static inline FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io)
{
    struct CtrCookie *c = (struct CtrCookie *)malloc(sizeof(*c));
    FILE *file;

    (void)mode;
    if (!c)
        return NULL;
    c->cookie = cookie;
    c->io = io;
    file = funopen(c, CtrCookieRead, NULL, CtrCookieSeek, CtrCookieClose);
    if (!file)
        free(c);
    return file;
}
#endif

/* ── Threads ──────────────────────────────────────────────────────────── */
typedef struct AndroidThread *Thread;
typedef void (*ThreadFunc)(void *);
Thread threadCreate(ThreadFunc entry, void *arg, size_t stackSize, int prio, int core, bool detached);
Result threadJoin(Thread thread, u64 timeoutNs);
void threadFree(Thread thread);

/* ── Local wireless (UDS) over Bluetooth LE ─────────────────────────── */
#include "3ds_uds.h"

#endif
