/* Step 1: two-display smoke test. One EGL context, two window surfaces
 * (top = default display, bottom = the Thor's second display), a render thread
 * that clears each, and input reaching native code. */
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Emerald", __VA_ARGS__)

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *sPending[2];
static int sPendingSet[2];
static atomic_int sPaused;
static atomic_int sKeys, sTouchX = -1, sTouchY = -1;
static pthread_t sThread;
static int sStarted;

static EGLDisplay sDpy = EGL_NO_DISPLAY;
static EGLConfig sCfg;
static EGLContext sCtx = EGL_NO_CONTEXT;
static EGLSurface sSurf[2] = { EGL_NO_SURFACE, EGL_NO_SURFACE };
static ANativeWindow *sWin[2];
static EGLSurface sPbuf = EGL_NO_SURFACE;

static void EglInit(void)
{
    sDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(sDpy, NULL, NULL);
    const EGLint attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLint n;
    eglChooseConfig(sDpy, attr, &sCfg, 1, &n);
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    sCtx = eglCreateContext(sDpy, sCfg, EGL_NO_CONTEXT, ca);
    const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    sPbuf = eglCreatePbufferSurface(sDpy, sCfg, pa);
    eglMakeCurrent(sDpy, sPbuf, sPbuf, sCtx);
    LOG("GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

static void TakeSurfaces(void)
{
    pthread_mutex_lock(&sLock);
    for (int i = 0; i < 2; i++) {
        if (!sPendingSet[i]) continue;
        sPendingSet[i] = 0;
        if (sSurf[i] != EGL_NO_SURFACE) {
            eglMakeCurrent(sDpy, sPbuf, sPbuf, sCtx);
            eglDestroySurface(sDpy, sSurf[i]);
            sSurf[i] = EGL_NO_SURFACE;
        }
        if (sWin[i]) ANativeWindow_release(sWin[i]);
        sWin[i] = sPending[i];
        sPending[i] = NULL;
        if (sWin[i])
            sSurf[i] = eglCreateWindowSurface(sDpy, sCfg, sWin[i], NULL);
        LOG("screen %d surface %p", i, (void *)sSurf[i]);
    }
    pthread_mutex_unlock(&sLock);
}

static void *RenderThread(void *arg)
{
    (void)arg;
    EglInit();
    unsigned frame = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        TakeSurfaces();
        if (atomic_load(&sPaused) || (sSurf[0] == EGL_NO_SURFACE && sSurf[1] == EGL_NO_SURFACE)) {
            usleep(16000);
            continue;
        }
        float pulse = (frame % 120) / 120.0f;
        for (int i = 0; i < 2; i++) {
            if (sSurf[i] == EGL_NO_SURFACE) continue;
            eglMakeCurrent(sDpy, sSurf[i], sSurf[i], sCtx);
            EGLint w, h;
            eglQuerySurface(sDpy, sSurf[i], EGL_WIDTH, &w);
            eglQuerySurface(sDpy, sSurf[i], EGL_HEIGHT, &h);
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            if (i == 0) glClearColor(0.6f, 0.1f * pulse, 0.1f, 1);
            else glClearColor(0.1f, 0.2f, 0.6f + 0.3f * pulse, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (i == 0 && atomic_load(&sKeys)) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(w / 2 - 50, h / 2 - 50, 100, 100);
                glClearColor(1, 1, 1, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            int tx = atomic_load(&sTouchX), ty = atomic_load(&sTouchY);
            if (i == 1 && tx >= 0) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(tx - 30, h - ty - 30, 60, 60);
                glClearColor(1, 1, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            eglSwapBuffers(sDpy, sSurf[i]);
        }
        frame++;
        if (frame % 600 == 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            LOG("600 frames in %.2fs (%.1f fps)", s, 600 / s);
            t0 = t1;
        }
    }
    return NULL;
}

#define FN(name) Java_com_cer0_emerald_MainActivity_##name

JNIEXPORT void JNICALL FN(nativeInit)(JNIEnv *env, jclass cls, jstring dir)
{
    (void)cls;
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    LOG("init, files dir %s, sizeof(void*)=%zu", d, sizeof(void *));
    (*env)->ReleaseStringUTFChars(env, dir, d);
    if (!sStarted) {
        sStarted = 1;
        pthread_create(&sThread, NULL, RenderThread, NULL);
    }
}

JNIEXPORT void JNICALL FN(nativeSurface)(JNIEnv *env, jclass cls, jint screen, jobject surface)
{
    (void)cls;
    if (screen < 0 || screen > 1) return;
    ANativeWindow *w = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&sLock);
    if (sPending[screen]) ANativeWindow_release(sPending[screen]);
    sPending[screen] = w;
    sPendingSet[screen] = 1;
    pthread_mutex_unlock(&sLock);
    /* the surface must not be destroyed under the render thread */
    if (!surface)
        for (int i = 0; i < 100; i++) {
            pthread_mutex_lock(&sLock);
            int done = !sPendingSet[screen];
            pthread_mutex_unlock(&sLock);
            if (done) break;
            usleep(2000);
        }
}

JNIEXPORT void JNICALL FN(nativeKey)(JNIEnv *env, jclass cls, jint code, jboolean down)
{
    (void)env; (void)cls;
    LOG("key %d %s", code, down ? "down" : "up");
    if (down) atomic_fetch_add(&sKeys, 1);
    else if (atomic_load(&sKeys) > 0) atomic_fetch_sub(&sKeys, 1);
}

JNIEXPORT void JNICALL FN(nativeAxis)(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    LOG("axis %.2f %.2f", x, y);
}

JNIEXPORT void JNICALL FN(nativeTouch)(JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y, jint w, jint h)
{
    (void)env; (void)cls; (void)w; (void)h;
    if (action == 1 || action == 3) { atomic_store(&sTouchX, -1); return; }
    atomic_store(&sTouchX, (int)x);
    atomic_store(&sTouchY, (int)y);
    if (action == 0) LOG("touch %d,%d of %dx%d", (int)x, (int)y, w, h);
}

JNIEXPORT void JNICALL FN(nativePause)(JNIEnv *env, jclass cls, jboolean p)
{
    (void)env; (void)cls;
    atomic_store(&sPaused, p);
}
