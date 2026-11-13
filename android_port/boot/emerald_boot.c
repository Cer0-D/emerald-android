/*
 * libemeraldboot.so: what Java loads (MainActivity). On request
 * (MainActivity.nativeLoadGame) it loads one language's game library,
 * libemerald_<code>.so, below 4 GiB and has it register its native methods.
 * The APK carries one library per cartridge language; a process loads one.
 *
 * The game keeps addresses in 32 bits - its assembled data holds them as
 * `.int symbol`, its code in u32 variables - so on arm64 the library, and
 * everything it hands the game (android_port/src/android_lowmem.c), has to sit
 * below 4 GiB, where such an address is the address. Android loads a library
 * anywhere; android_dlopen_ext loads one into address space the caller
 * reserved. ART keeps its own heap and boot image below 4 GiB too, at
 * addresses that change between boots, so the range is found, not fixed.
 *
 * A 32-bit build has nothing to place: the library is loaded as it is.
 */
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "Emerald"
#define LIBRARY_PREFIX "libemerald_"
#define LOW_START 0x01000000UL
#define LOW_STEP 0x01000000UL
/* Below 2 GiB: see android_port/src/android_lowmem.c (sign extension). */
#define LOW_END 0x80000000ULL

typedef jint (*OnLoadFn)(JavaVM *, JNIEnv *);

#if UINTPTR_MAX > 0xFFFFFFFFu
/* The span of the library's loadable segments: what it needs reserved. */
static size_t LibrarySpan(const char *path)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph;
    uint64_t lo = UINT64_MAX, hi = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
    {
        close(fd);
        return 0;
    }
    for (int i = 0; i < eh.e_phnum; i++)
    {
        if (pread(fd, &ph, sizeof(ph), (off_t)(eh.e_phoff + (uint64_t)i * eh.e_phentsize)) != (ssize_t)sizeof(ph))
            break;
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_vaddr < lo)
            lo = ph.p_vaddr;
        if (ph.p_vaddr + ph.p_memsz > hi)
            hi = ph.p_vaddr + ph.p_memsz;
    }
    close(fd);
    return hi > lo ? (size_t)(hi - (lo & ~0xFFFFull)) : 0;
}

static void *ReserveLow(size_t size)
{
    size = (size + 0xFFFFu) & ~(size_t)0xFFFFu;
    for (uint64_t a = LOW_START; a + size <= LOW_END; a += LOW_STEP)
    {
        void *p = mmap((void *)(uintptr_t)a, size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

        if (p == (void *)(uintptr_t)a)
            return p;
        if (p != MAP_FAILED)
            munmap(p, size);
    }
    return NULL;
}
#endif

static void *LoadGame(const char *path)
{
#if UINTPTR_MAX > 0xFFFFFFFFu
    size_t span = LibrarySpan(path);
    void *base;
    android_dlextinfo ext;
    void *handle;

    if (span == 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "boot: cannot read %s", path);
        return NULL;
    }
    base = ReserveLow(span);
    if (base == NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "boot: no %zu MiB free below 4 GiB", span >> 20);
        return NULL;
    }
    memset(&ext, 0, sizeof(ext));
    ext.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
    ext.reserved_addr = base;
    ext.reserved_size = (span + 0xFFFFu) & ~(size_t)0xFFFFu;
    handle = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext);
    if (handle != NULL)
        __android_log_print(ANDROID_LOG_INFO, TAG, "boot: game library at %p (%zu MiB)", base, span >> 20);
    return handle;
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static JavaVM *sVm;
static char sLoaded[8];   /* the language loaded, "" none */

/* MainActivity.nativeLoadGame(code): the game library for language `code`
 * (en, fr...), loaded and its natives registered; true when that one is
 * loaded. False when it could not be, or when another language's already
 * is (a process holds one: the caller restarts it). */
static jboolean LoadGameNative(JNIEnv *env, jclass cls, jstring jcode)
{
    Dl_info self;
    char path[512];
    const char *code;
    void *handle;
    OnLoadFn onLoad;
    bool ok = false;

    (void)cls;
    if (jcode == NULL || (code = (*env)->GetStringUTFChars(env, jcode, NULL)) == NULL)
        return JNI_FALSE;
    if (sLoaded[0])
    {
        ok = strcmp(sLoaded, code) == 0;
        if (!ok)
            __android_log_print(ANDROID_LOG_WARN, TAG, "boot: %s is loaded, %s asked for", sLoaded, code);
        goto done;
    }
    /* Two to seven letters: a language code, nothing that names a path. */
    for (size_t i = 0; code[i]; i++)
        if (code[i] < 'a' || code[i] > 'z' || i >= sizeof(sLoaded) - 1)
            goto done;
    if (strlen(code) < 2)
        goto done;
    /* The game libraries are beside this one, in the APK's native library dir. */
    if (dladdr((void *)LoadGameNative, &self) == 0 || self.dli_fname == NULL)
        goto done;
    snprintf(path, sizeof(path), "%s", self.dli_fname);
    char *slash = strrchr(path, '/');
    if (slash == NULL)
        goto done;
    snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), LIBRARY_PREFIX "%s.so", code);

    handle = LoadGame(path);
    if (handle == NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "boot: %s did not load: %s", path, dlerror());
        goto done;
    }
    onLoad = (OnLoadFn)dlsym(handle, "AndroidMain_OnLoad");
    if (onLoad == NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "boot: %s has no AndroidMain_OnLoad", path);
        goto done;
    }
    if (onLoad(sVm, env) == JNI_ERR)
        goto done;
    snprintf(sLoaded, sizeof(sLoaded), "%s", code);
    __android_log_print(ANDROID_LOG_INFO, TAG, "boot: %s loaded", path);
    ok = true;
done:
    (*env)->ReleaseStringUTFChars(env, jcode, code);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    static const JNINativeMethod methods[] = {
        {"nativeLoadGame", "(Ljava/lang/String;)Z", (void *)LoadGameNative},
    };
    JNIEnv *env;
    jclass cls;

    (void)reserved;
    sVm = vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    cls = (*env)->FindClass(env, "com/cer0/emerald/MainActivity");
    if (cls == NULL || (*env)->RegisterNatives(env, cls, methods, 1) != 0)
        return JNI_ERR;
    (*env)->DeleteLocalRef(env, cls);
    return JNI_VERSION_1_6;
}
