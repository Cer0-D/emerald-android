/*
 * The port's log on Android: every line to logcat (tag "Emerald"), and, as on
 * the 3DS, to sdmc:/3ds/emerald3ds/port.log when debug.txt exists there. The
 * file is written by its own thread so the game never waits on storage.
 */
#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "3ds_log.h"
#include "3ds_platform.h"

#define LOG_DIR "sdmc:/3ds/emerald3ds/"
#define RING_BYTES (256u << 10)

static const char *const sTags[] = { "BOOT", "VIDEO", "INPUT", "FS", "AUDIO", "GAME", "ERROR" };
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWake = PTHREAD_COND_INITIALIZER;
static char sRing[RING_BYTES];
static size_t sHead, sTail;
static bool sFileOn, sQuit;
static pthread_t sWriter;
static FILE *sFile;
static char sFatal[256];

bool CtrLog_Enabled(void) { return sFileOn; }

void CtrLog_LastDrain(float *ms, float *agoMs)
{
    *ms = 0.0f;
    *agoMs = 1.0e6f;
}

static void *Writer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&sLock);
    while (!sQuit || sHead != sTail)
    {
        while (sHead == sTail && !sQuit)
            pthread_cond_wait(&sWake, &sLock);
        size_t head = sHead, tail = sTail;
        pthread_mutex_unlock(&sLock);
        if (head != tail && sFile)
        {
            if (tail < head)
                fwrite(sRing + tail, 1, head - tail, sFile);
            else
            {
                fwrite(sRing + tail, 1, RING_BYTES - tail, sFile);
                fwrite(sRing, 1, head, sFile);
            }
            fflush(sFile);
        }
        pthread_mutex_lock(&sLock);
        sTail = head;
    }
    pthread_mutex_unlock(&sLock);
    return NULL;
}

void CtrLog_Init(void)
{
    struct stat st;

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/emerald3ds", 0777);
    if (stat(LOG_DIR "debug.txt", &st) == 0)
    {
        rename(LOG_DIR "port.log", LOG_DIR "port-prev.log");
        sFile = fopen(LOG_DIR "port.log", "w");
        if (sFile && pthread_create(&sWriter, NULL, Writer, NULL) == 0)
            sFileOn = true;
    }
    __android_log_print(ANDROID_LOG_INFO, "Emerald", "log started (file %s)", sFileOn ? "on" : "off");
}

void CtrLog_Close(void)
{
    if (!sFileOn)
        return;
    pthread_mutex_lock(&sLock);
    sQuit = true;
    pthread_cond_signal(&sWake);
    pthread_mutex_unlock(&sLock);
    pthread_join(sWriter, NULL);
    fclose(sFile);
    sFile = NULL;
    sFileOn = false;
}

static void Append(const char *text, size_t len)
{
    pthread_mutex_lock(&sLock);
    size_t used = (sHead + RING_BYTES - sTail) % RING_BYTES;
    if (used + len < RING_BYTES - 1)
    {
        for (size_t i = 0; i < len; i++)
        {
            sRing[sHead] = text[i];
            sHead = (sHead + 1) % RING_BYTES;
        }
        pthread_cond_signal(&sWake);
    }
    pthread_mutex_unlock(&sLock);
}

void CtrLog_Write(CtrLogCategory category, const char *format, ...)
{
    char line[1024];
    va_list args;
    int prefix, length;
    unsigned cat = (unsigned)category < sizeof(sTags) / sizeof(sTags[0]) ? (unsigned)category : 6;
    uint64_t ms = CtrPlatform_Milliseconds();

    prefix = snprintf(line, sizeof(line), "[%6llu.%03llu] [%s] ", (unsigned long long)(ms / 1000),
                      (unsigned long long)(ms % 1000), sTags[cat]);
    va_start(args, format);
    length = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1, format, args);
    va_end(args);
    if (length < 0)
        return;
    length += prefix;
    if (length > (int)sizeof(line) - 2)
        length = (int)sizeof(line) - 2;
    while (length > 0 && line[length - 1] == '\n')
        length--;
    line[length] = 0;
    __android_log_write(cat == 6 ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "Emerald", line + prefix);
    if (sFileOn)
    {
        line[length++] = '\n';
        Append(line, (size_t)length);
    }
}

void CtrLog_WriteBinary(const void *data, unsigned size)
{
    (void)data;
    (void)size;
}

/* The reason is kept for the video backend to show (android_video.c). */
void CtrLog_ShowFatal(const char *reason)
{
    snprintf(sFatal, sizeof(sFatal), "%s", reason ? reason : "fatal error");
    __android_log_print(ANDROID_LOG_FATAL, "Emerald", "FATAL: %s", sFatal);
}

const char *AndroidLog_FatalReason(void) { return sFatal[0] ? sFatal : NULL; }
