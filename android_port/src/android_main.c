/*
 * The process side of the Android port: JNI entry points the Java shell
 * (MainActivity) calls, the windows of the two screens, input and lifecycle
 * state, and the game thread - the Android counterpart of main_3ds.c. The
 * game owns its loop (AgbMain never returns), so it gets a thread of its own,
 * which is also the thread that renders (android_video.c).
 */
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "android_host.h"
#include "android_lowmem.h"
#include "3ds_locale.h"
#include "3ds_platform.h"
#include "3ds_video.h"

/* ── Windows ──────────────────────────────────────────────────────────── */
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sCond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *sWindow[HOST_SCREENS];
static uint32_t sWindowGen[HOST_SCREENS];
static uint32_t sWindowDoneGen[HOST_SCREENS];

ANativeWindow *AndroidHost_AcquireWindow(int screen, uint32_t *generation)
{
    ANativeWindow *window;

    pthread_mutex_lock(&sLock);
    window = sWindow[screen];
    if (window)
        ANativeWindow_acquire(window);
    *generation = sWindowGen[screen];
    pthread_mutex_unlock(&sLock);
    return window;
}

void AndroidHost_ReleaseWindow(ANativeWindow *window)
{
    if (window)
        ANativeWindow_release(window);
}

void AndroidHost_WindowDone(int screen, uint32_t generation)
{
    pthread_mutex_lock(&sLock);
    if (generation > sWindowDoneGen[screen])
        sWindowDoneGen[screen] = generation;
    pthread_cond_broadcast(&sCond);
    pthread_mutex_unlock(&sLock);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */
static bool sPaused = true;
static uint32_t sResumes;
static bool sGameStarted;
static pthread_t sGameThread;
static char sFilesDir[512];
/* The folder romfs:/ is in: romfs (a development install's pushed data) or
 * engine (a release's engine files, extracted from the APK; Setup.java). */
static char sRomfsDir[64] = "romfs";
static atomic_bool sCapture;

const char *AndroidHost_FilesDir(void) { return sFilesDir; }
const char *AndroidHost_RomfsDir(void) { return sRomfsDir; }

bool AndroidHost_Paused(void)
{
    bool paused;

    pthread_mutex_lock(&sLock);
    paused = sPaused;
    pthread_mutex_unlock(&sLock);
    return paused;
}

uint32_t AndroidHost_ResumeCount(void)
{
    uint32_t count;

    pthread_mutex_lock(&sLock);
    count = sResumes;
    pthread_mutex_unlock(&sLock);
    return count;
}

void AndroidHost_WaitResumed(void)
{
    pthread_mutex_lock(&sLock);
    while (sPaused)
        pthread_cond_wait(&sCond, &sLock);
    pthread_mutex_unlock(&sLock);
}

bool AndroidHost_TakeCaptureRequest(void)
{
    return atomic_exchange(&sCapture, false);
}

/* ── Input ────────────────────────────────────────────────────────────── */
static AndroidHostInput sInput;
/* Buttons pressed since the game last looked: a press and release between
 * two frames (a quick tap, adb's keyevent) still counts as one frame held. */
static uint32_t sLatched;
static bool sTouchLatched;
static float sTouchLatchX, sTouchLatchY;

void AndroidHost_GetInput(AndroidHostInput *out)
{
    pthread_mutex_lock(&sLock);
    *out = sInput;
    out->buttons |= sLatched;
    sLatched = 0;
    if (!out->touchActive && sTouchLatched)
    {
        out->touchActive = true;
        out->touchX = sTouchLatchX;
        out->touchY = sTouchLatchY;
    }
    sTouchLatched = false;
    pthread_mutex_unlock(&sLock);
}

/* android.view.KeyEvent codes */
static uint32_t ButtonForKey(int code)
{
    switch (code)
    {
    case 96: return HOST_BTN_A;          /* BUTTON_A */
    case 97: return HOST_BTN_B;          /* BUTTON_B */
    case 99: return HOST_BTN_X;          /* BUTTON_X */
    case 100: return HOST_BTN_Y;         /* BUTTON_Y */
    case 102: return HOST_BTN_L;         /* BUTTON_L1 */
    case 103: return HOST_BTN_R;         /* BUTTON_R1 */
    case 104: return HOST_BTN_ZL;        /* BUTTON_L2 */
    case 105: return HOST_BTN_ZR;        /* BUTTON_R2 */
    case 108: return HOST_BTN_START;     /* BUTTON_START */
    case 109: return HOST_BTN_SELECT;    /* BUTTON_SELECT */
    case 82: return HOST_BTN_START;      /* MENU */
    case 19: return HOST_BTN_UP;         /* DPAD_UP */
    case 20: return HOST_BTN_DOWN;
    case 21: return HOST_BTN_LEFT;
    case 22: return HOST_BTN_RIGHT;
    /* A keyboard, for testing over adb */
    case 54: return HOST_BTN_B;          /* Z */
    case 52: return HOST_BTN_A;          /* X */
    case 66: return HOST_BTN_START;      /* ENTER */
    case 67: return HOST_BTN_SELECT;     /* DEL */
    default: return 0;
    }
}

/* ── The game thread ──────────────────────────────────────────────────── */
static void *GameThread(void *arg)
{
    (void)arg;
    AndroidHost_WaitResumed();
    if (!CtrPlatform_Init())
        CtrPlatform_Fatal("platform initialization failed");
    CtrPlatformHooks hooks = {0};
    hooks.vblank = CtrGame_VBlank;
    hooks.videoPresent = CtrVideo_Present;
    CtrPlatform_SetHooks(&hooks);
    if (!CtrPlatform_BeginFrame())
        return NULL;
    CtrGame_Init();
    CtrPlatform_Shutdown();
    return NULL;
}

/* ── JNI ──────────────────────────────────────────────────────────────── */
#define FN(name) Java_com_cer0_emerald_MainActivity_##name

JNIEXPORT void JNICALL FN(nativeInit)(JNIEnv *env, jclass cls, jstring dir, jstring romfs)
{
    (void)cls;
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    snprintf(sFilesDir, sizeof(sFilesDir), "%s", d);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    d = (*env)->GetStringUTFChars(env, romfs, NULL);
    snprintf(sRomfsDir, sizeof(sRomfsDir), "%s", d);
    (*env)->ReleaseStringUTFChars(env, romfs, d);
    pthread_mutex_lock(&sLock);
    if (!sGameStarted)
    {
        pthread_attr_t attr;

        sGameStarted = true;
        pthread_attr_init(&attr);
        /* The game's stack: deep battle script recursion and big locals.
         * Below 4 GiB (android_lowmem.h): the game keeps the addresses of its
         * locals in 32 bits too. */
        void *stack = AndroidLow_Stack(8u << 20);
        if (stack != NULL)
            pthread_attr_setstack(&attr, stack, 8u << 20);
        else
            pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_create(&sGameThread, &attr, GameThread, NULL);
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativeSurface)(JNIEnv *env, jclass cls, jint screen, jobject surface)
{
    (void)cls;
    if (screen < 0 || screen >= HOST_SCREENS)
        return;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    uint32_t gen;

    pthread_mutex_lock(&sLock);
    if (sWindow[screen])
        ANativeWindow_release(sWindow[screen]);
    sWindow[screen] = window;
    gen = ++sWindowGen[screen];
    /* A window going away must not be drawn into once this returns: wait
     * for the render thread to let go of it (bounded, in case the game
     * thread is stuck, e.g. not started yet). */
    if (!surface && sGameStarted && !sPaused)
    {
        struct timespec until;

        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (sWindowDoneGen[screen] < gen)
            if (pthread_cond_timedwait(&sCond, &sLock, &until) != 0)
                break;
    }
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativeKey)(JNIEnv *env, jclass cls, jint code, jboolean down)
{
    (void)env; (void)cls;
    uint32_t bit = ButtonForKey(code);

    if (!bit)
        return;
    pthread_mutex_lock(&sLock);
    if (down)
    {
        sInput.buttons |= bit;
        sLatched |= bit;
    }
    else
        sInput.buttons &= ~bit;
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativeAxis)(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&sLock);
    sInput.stickX = x;
    sInput.stickY = y;
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativeTouch)(JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y, jint w, jint h)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&sLock);
    /* MotionEvent: 0 DOWN, 1 UP, 2 MOVE, 3 CANCEL */
    sInput.touchActive = !(action == 1 || action == 3);
    sInput.touchX = x;
    sInput.touchY = y;
    sInput.touchW = w;
    sInput.touchH = h;
    if (action == 0)
    {
        sTouchLatched = true;
        sTouchLatchX = x;
        sTouchLatchY = y;
    }
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativePause)(JNIEnv *env, jclass cls, jboolean paused)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&sLock);
    if (sPaused && !paused)
        sResumes++;
    sPaused = paused;
    if (paused)
        memset(&sInput, 0, sizeof(sInput));
    pthread_cond_broadcast(&sCond);
    pthread_mutex_unlock(&sLock);
}

JNIEXPORT void JNICALL FN(nativeCapture)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    atomic_store(&sCapture, true);
}

/* The language of the follower's texts (FollowerFetcher.java): the game's,
 * as src/follower_mon.c picks its followers/text/LL.bin. */
JNIEXPORT jstring JNICALL FN(nativeFollowerLanguage)(JNIEnv *env, jclass cls)
{
    (void)cls;
#if GAME_LANGUAGE == LANGUAGE_SPANISH
    return (*env)->NewStringUTF(env, "es");
#elif GAME_LANGUAGE == LANGUAGE_FRENCH
    return (*env)->NewStringUTF(env, "fr");
#elif GAME_LANGUAGE == LANGUAGE_ITALIAN
    return (*env)->NewStringUTF(env, "it");
#elif GAME_LANGUAGE == LANGUAGE_GERMAN
    return (*env)->NewStringUTF(env, "de");
#else
    return (*env)->NewStringUTF(env, "en");
#endif
}

/* ── Loading ──────────────────────────────────────────────────────────── */
/*
 * Java loads libemeraldboot.so (android_port/boot), which puts this library
 * below 4 GiB and calls this: a library loaded that way is not one the
 * runtime looks in for Java_... names, so the methods are registered here.
 */
JNIEXPORT jint AndroidMain_OnLoad(JavaVM *vm, JNIEnv *env)
{
    static const JNINativeMethod methods[] = {
        {"nativeInit", "(Ljava/lang/String;Ljava/lang/String;)V", (void *)FN(nativeInit)},
        {"nativeSurface", "(ILjava/lang/Object;)V", (void *)FN(nativeSurface)},
        {"nativeKey", "(IZ)V", (void *)FN(nativeKey)},
        {"nativeAxis", "(FF)V", (void *)FN(nativeAxis)},
        {"nativeTouch", "(IFFII)V", (void *)FN(nativeTouch)},
        {"nativePause", "(Z)V", (void *)FN(nativePause)},
        {"nativeCapture", "()V", (void *)FN(nativeCapture)},
        {"nativeFollowerLanguage", "()Ljava/lang/String;", (void *)FN(nativeFollowerLanguage)},
    };
    jclass cls = (*env)->FindClass(env, "com/cer0/emerald/MainActivity");

    if (cls == NULL)
        return JNI_ERR;
    if ((*env)->RegisterNatives(env, cls, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    (*env)->DeleteLocalRef(env, cls);
    /* Local wireless play's radio (android_uds.c, LocalBluetooth.java). */
    {
        extern jint AndroidUds_OnLoad(JavaVM *vm, JNIEnv *env);
        if (AndroidUds_OnLoad(vm, env) == JNI_ERR)
            __android_log_print(ANDROID_LOG_ERROR, "Emerald", "local wireless: LocalBluetooth not found");
    }
    return JNI_VERSION_1_6;
}
