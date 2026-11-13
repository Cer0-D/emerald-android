/*
 * Memory below 4 GiB for the arm64 build: see include/android_lowmem.h.
 *
 * Address space. The runtime's own heap and boot image are below 4 GiB too
 * (ART keeps them there for its 32-bit references), at addresses that change
 * from one boot to the next, so a range is found by asking for one 16 MiB
 * step at a time (MAP_FIXED_NOREPLACE never moves anything already mapped).
 *
 * The heap. malloc, calloc, realloc, free and the aligned allocators of every
 * unit in the library are wrapped (android.mk, --wrap) to dlmalloc's mspace
 * over one such range, reserved with MAP_NORESERVE: pages cost memory only
 * once touched. Should it fill, an allocation falls back to the system heap
 * (above 4 GiB: a buffer the game then took the address of would fail at
 * once, loudly, rather than allocation failing). free and realloc tell the two
 * apart by address, so memory the C library allocated itself (strdup,
 * getline...) is still freed by it.
 */
#include "android_lowmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "Emerald"
#define LOW_START 0x01000000UL
#define LOW_STEP  0x01000000UL
/* Below 2 GiB, not just 4: the game also rebuilds addresses from two signed
 * halfwords ((s16)hi << 16), which sign-extends one with bit 31 set into
 * garbage on a 64-bit build. Below 2 GiB that bit is never set. */
#define LOW_END   0x80000000ULL

#if UINTPTR_MAX > 0xFFFFFFFFu
#define LOW_NEEDED 1
#else
#define LOW_NEEDED 0
#endif

void *AndroidLow_Reserve(size_t size, bool writable)
{
    int prot = writable ? PROT_READ | PROT_WRITE : PROT_NONE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

    size = (size + 0xFFFFu) & ~(size_t)0xFFFFu;
#if LOW_NEEDED
    for (uint64_t a = LOW_START; a + size <= LOW_END; a += LOW_STEP)
    {
        void *p = mmap((void *)(uintptr_t)a, size, prot, flags | MAP_FIXED_NOREPLACE, -1, 0);

        if (p == (void *)(uintptr_t)a)
            return p;
        if (p != MAP_FAILED)
            munmap(p, size); /* a kernel without MAP_FIXED_NOREPLACE put it elsewhere */
    }
    return NULL;
#else
    void *p = mmap(NULL, size, prot, flags, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void AndroidLow_Release(void *base, size_t size)
{
    if (base != NULL)
        munmap(base, (size + 0xFFFFu) & ~(size_t)0xFFFFu);
}

void *AndroidLow_Stack(size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *base;

    size = (size + page - 1) & ~(page - 1);
    base = AndroidLow_Reserve(size + page, true);
    if (base == NULL)
        return NULL;
    mprotect(base, page, PROT_NONE); /* the guard: stacks grow down into it */
    return base + page;
}

void AndroidLow_FreeStack(void *stack, size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    if (stack != NULL)
        AndroidLow_Release((uint8_t *)stack - page, ((size + page - 1) & ~(page - 1)) + page);
}

#if LOW_NEEDED
/* dlmalloc, as an mspace only: no global malloc of its own, no growing. */
#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define MALLOC_FAILURE_ACTION
#define FOOTERS 0
#define INSECURE 0
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../third_party/dlmalloc/malloc.c"
#pragma clang diagnostic pop

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *p, size_t size);
void __real_free(void *p);
void *__real_memalign(size_t align, size_t size);
int __real_posix_memalign(void **out, size_t align, size_t size);
void *__real_aligned_alloc(size_t align, size_t size);
size_t __real_malloc_usable_size(const void *p);

/* As much as one range will take, largest first: below 2 GiB, ART leaves a
 * gap of about a gigabyte between its heap and its boot image. */
static const size_t kArenaSizes[] = {768u << 20, 512u << 20, 384u << 20, 256u << 20};

static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static mspace sSpace;
static uint8_t *sBase;
static size_t sSize;
static bool sWarned;

static void Init(void)
{
    for (size_t i = 0; i < sizeof(kArenaSizes) / sizeof(kArenaSizes[0]); i++)
    {
        sBase = AndroidLow_Reserve(kArenaSizes[i], true);
        if (sBase != NULL)
        {
            sSize = kArenaSizes[i];
            sSpace = create_mspace_with_base(sBase, sSize, 1);
            __android_log_print(ANDROID_LOG_INFO, TAG, "low memory: %zu MiB heap at %p", sSize >> 20, sBase);
            return;
        }
    }
    __android_log_print(ANDROID_LOG_ERROR, TAG, "low memory: no room below 4 GiB for a heap");
}

static inline bool Ready(void)
{
    pthread_once(&sOnce, Init);
    return sSpace != NULL;
}

bool AndroidLow_Owns(const void *p)
{
    return sBase != NULL && (const uint8_t *)p >= sBase && (const uint8_t *)p < sBase + sSize;
}

void AndroidLow_Stats(size_t *arena, size_t *used)
{
    *arena = sSize;
    *used = Ready() ? mspace_mallinfo(sSpace).uordblks : 0;
}

static void *Fallback(void *p, size_t size)
{
    if (p != NULL && !sWarned)
    {
        sWarned = true;
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "low memory: heap full, %zu bytes from the system heap (above 4 GiB)", size);
    }
    return p;
}

void *__wrap_malloc(size_t size)
{
    void *p = Ready() ? mspace_malloc(sSpace, size) : NULL;
    return p != NULL ? p : Fallback(__real_malloc(size), size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *p = Ready() ? mspace_calloc(sSpace, count, size) : NULL;
    return p != NULL ? p : Fallback(__real_calloc(count, size), count * size);
}

void __wrap_free(void *p)
{
    if (p == NULL)
        return;
    if (AndroidLow_Owns(p))
        mspace_free(sSpace, p);
    else
        __real_free(p);
}

void *__wrap_realloc(void *p, size_t size)
{
    if (p == NULL)
        return __wrap_malloc(size);
    if (!AndroidLow_Owns(p))
        return __real_realloc(p, size);
    if (size == 0)
    {
        mspace_free(sSpace, p);
        return NULL;
    }
    void *q = mspace_realloc(sSpace, p, size);
    if (q == NULL)
    {
        /* The arena is full: move it to the system heap, as malloc would. */
        size_t old = mspace_usable_size(p);
        q = Fallback(__real_malloc(size), size);
        if (q != NULL)
        {
            memcpy(q, p, old < size ? old : size);
            mspace_free(sSpace, p);
        }
    }
    return q;
}

void *__wrap_memalign(size_t align, size_t size)
{
    void *p = Ready() ? mspace_memalign(sSpace, align, size) : NULL;
    return p != NULL ? p : Fallback(__real_memalign(align, size), size);
}

void *__wrap_aligned_alloc(size_t align, size_t size)
{
    return __wrap_memalign(align, size);
}

int __wrap_posix_memalign(void **out, size_t align, size_t size)
{
    if (align < sizeof(void *) || (align & (align - 1)) != 0)
        return EINVAL;
    *out = __wrap_memalign(align, size);
    return *out != NULL ? 0 : ENOMEM;
}

size_t __wrap_malloc_usable_size(const void *p)
{
    if (p != NULL && AndroidLow_Owns(p))
        return mspace_usable_size(p);
    return __real_malloc_usable_size(p);
}
#else
bool AndroidLow_Owns(const void *p)
{
    (void)p;
    return false;
}

void AndroidLow_Stats(size_t *arena, size_t *used)
{
    *arena = *used = 0;
}
#endif
