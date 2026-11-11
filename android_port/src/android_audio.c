/*
 * Audio on Android: the mixer's PCM frames to an AAudio stream.
 *
 * The game's sound engine (m4a, through 3ds_sound.c) mixes one frame of 701
 * stereo samples per game frame on the game thread, as the 3DS does without
 * its worker core, and hands it to CtrAudio_Queue. Here that frame goes into
 * a ring the stream's callback reads from.
 *
 * The stream plays at the device's own rate (48 kHz on the Thor), not the
 * mixer's 42060 Hz, so the callback resamples - linearly, which is plenty
 * for the GBA's 8-bit samples. Drift is handled as 3ds_audio.c handles it:
 * the game makes 701 samples per frame at whatever pace the display really
 * runs, so the resampling step is nudged by fractions of a percent towards
 * the ring depth we want. Inaudible, and the ring neither runs dry nor
 * overflows.
 *
 * A stream that dies (headphones plugged in, the output changed) is reopened
 * from the game thread on the next frame: AAudio forbids closing it from its
 * own callbacks.
 */
#include <aaudio/AAudio.h>
#include <stdatomic.h>
#include <string.h>

#include "3ds_audio.h"
#include "3ds_platform.h"

/* 8192 stereo frames: 195 ms at the mixer's rate, over eleven mixer frames. */
#define RING_FRAMES 8192u
#define RING_MASK (RING_FRAMES - 1u)
/* Mixer frames we keep queued: the 3DS's 50 ms of slack. */
#define TARGET_FRAMES (CTR_AUDIO_TARGET_DEPTH * 701.0f)
/* Per mixer frame of ring away from the target, as a fraction of the rate. */
#define TRIM 0.0025f
#define TRIM_MAX 0.0100f

static int16_t sRing[RING_FRAMES * 2];
/* Frames written (game thread) and read (callback); free-running counters. */
static atomic_uint sWritten, sRead;
static AAudioStreamBuilder *sBuilder;
static AAudioStream *sStream;
static atomic_bool sDead;
static bool sPaused;
static float sOutRate = 48000.0f;
/* The callback's place between two ring frames, 0..1. */
static float sPhase;
/* The ring's depth, smoothed: the callback reads in bursts. */
static float sDepth = TARGET_FRAMES;
static bool sPrimed;
static CtrAudioStats sStats;

CtrAudioStats *CtrAudio_Stats(void) { return &sStats; }

static aaudio_data_callback_result_t Callback(AAudioStream *stream, void *user, void *data, int32_t frames)
{
    int16_t *out = data;
    unsigned read = atomic_load_explicit(&sRead, memory_order_relaxed);
    unsigned written = atomic_load_explicit(&sWritten, memory_order_acquire);
    unsigned avail = written - read;
    float trim, step;

    (void)stream;
    (void)user;
    sDepth += ((float)avail - sDepth) * 0.05f;
    /* Wait for the target depth before playing, and again after running dry,
     * so a stall costs one gap rather than a crackle of them. */
    if (!sPrimed && avail >= (unsigned)TARGET_FRAMES)
        sPrimed = true;
    trim = (sDepth - TARGET_FRAMES) / 701.0f * TRIM;
    if (trim > TRIM_MAX) trim = TRIM_MAX;
    else if (trim < -TRIM_MAX) trim = -TRIM_MAX;
    step = (float)CTR_AUDIO_SAMPLE_RATE / sOutRate * (1.0f + trim);
    sStats.rateHz = (float)CTR_AUDIO_SAMPLE_RATE * (1.0f + trim);

    for (int32_t i = 0; i < frames; ++i)
    {
        /* Two ring frames to blend between. */
        if (!sPrimed || avail < 2)
        {
            if (sPrimed)
            {
                sPrimed = false;
                ++sStats.underruns;
            }
            memset(out + i * 2, 0, (size_t)(frames - i) * 2 * sizeof(int16_t));
            break;
        }
        const int16_t *a = &sRing[(read & RING_MASK) * 2], *b = &sRing[((read + 1) & RING_MASK) * 2];

        out[i * 2] = (int16_t)(a[0] + (b[0] - a[0]) * sPhase);
        out[i * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * sPhase);
        sPhase += step;
        while (sPhase >= 1.0f && avail >= 2)
        {
            sPhase -= 1.0f;
            ++read;
            --avail;
        }
    }
    atomic_store_explicit(&sRead, read, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void Error(AAudioStream *stream, void *user, aaudio_result_t error)
{
    (void)stream;
    (void)user;
    (void)error;
    atomic_store(&sDead, true);
}

static void CloseStream(void)
{
    if (sStream)
    {
        AAudioStream_requestStop(sStream);
        AAudioStream_close(sStream);
        sStream = NULL;
    }
}

static bool OpenStream(void)
{
    aaudio_result_t r;

    if (!sBuilder)
    {
        if (AAudio_createStreamBuilder(&sBuilder) != AAUDIO_OK)
        {
            sBuilder = NULL;
            return false;
        }
        AAudioStreamBuilder_setDirection(sBuilder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(sBuilder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(sBuilder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(sBuilder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(sBuilder, 2);
        AAudioStreamBuilder_setUsage(sBuilder, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(sBuilder, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(sBuilder, Callback, NULL);
        AAudioStreamBuilder_setErrorCallback(sBuilder, Error, NULL);
    }
    atomic_store(&sDead, false);
    r = AAudioStreamBuilder_openStream(sBuilder, &sStream);
    if (r != AAUDIO_OK)
    {
        sStream = NULL;
        CtrLog_Write(CTR_LOG_AUDIO, "AAudio: no stream (%s)", AAudio_convertResultToText(r));
        return false;
    }
    sOutRate = (float)AAudioStream_getSampleRate(sStream);
    if (sOutRate < 8000.0f)
        sOutRate = 48000.0f;
    /* Two bursts of buffer: low latency, room for the callback's jitter. */
    AAudioStream_setBufferSizeInFrames(sStream, AAudioStream_getFramesPerBurst(sStream) * 2);
    if (!sPaused && (r = AAudioStream_requestStart(sStream)) != AAUDIO_OK)
    {
        CtrLog_Write(CTR_LOG_AUDIO, "AAudio: stream would not start (%s)", AAudio_convertResultToText(r));
        CloseStream();
        return false;
    }
    CtrLog_Write(CTR_LOG_AUDIO, "AAudio: %d Hz, burst %d, mixer at %d Hz", (int)sOutRate,
                 (int)AAudioStream_getFramesPerBurst(sStream), CTR_AUDIO_SAMPLE_RATE);
    return true;
}

bool CtrAudio_Init(void)
{
    return OpenStream();
}

void CtrAudio_Shutdown(void)
{
    CloseStream();
    if (sBuilder)
    {
        AAudioStreamBuilder_delete(sBuilder);
        sBuilder = NULL;
    }
}

/* The activity paused or resumed (android_platform.c): the stream stops with
 * it, so the game holds no audio device in the background. */
void AndroidAudio_SetPaused(bool paused)
{
    if (paused == sPaused)
        return;
    sPaused = paused;
    if (!sStream)
        return;
    if (paused)
        AAudioStream_requestPause(sStream);
    else
    {
        /* What was queued before the pause is stale: start from empty. */
        atomic_store(&sRead, atomic_load(&sWritten));
        sPrimed = false;
        AAudioStream_requestStart(sStream);
    }
}

void CtrAudio_Queue(const int32_t *interleaved, int frames)
{
    unsigned written, read, room;
    int32_t peak = 0;

    if (atomic_load(&sDead))
    {
        /* The output changed under the stream: a new one on the new output. */
        CloseStream();
        if (OpenStream())
            CtrLog_Write(CTR_LOG_AUDIO, "AAudio: stream reopened");
    }
    if (interleaved == NULL || frames <= 0)
        return;
    written = atomic_load_explicit(&sWritten, memory_order_relaxed);
    read = atomic_load_explicit(&sRead, memory_order_acquire);
    room = RING_FRAMES - 1u - (written - read);
    if (!sStream || sPaused || (unsigned)frames > room)
    {
        /* No stream, or the ring full: the game is ahead of the output. */
        ++sStats.drops;
        return;
    }
    for (int i = 0; i < frames; ++i)
    {
        int16_t *dst = &sRing[((written + (unsigned)i) & RING_MASK) * 2];

        for (int c = 0; c < 2; ++c)
        {
            /* The mixer's fixed point has 1 << 23 to full scale: 8 bits
             * below the 16 played, rounded off (3ds_audio.c). */
            int32_t sample = (interleaved[i * 2 + c] + 128) >> 8;

            if (sample > 32767) sample = 32767;
            else if (sample < -32768) sample = -32768;
            dst[c] = (int16_t)sample;
            if (sample < 0) sample = -sample;
            if (sample > peak) peak = sample;
        }
    }
    atomic_store_explicit(&sWritten, written + (unsigned)frames, memory_order_release);
    ++sStats.frames;
    sStats.peak = (uint32_t)peak;
    sStats.queued = (written + (unsigned)frames - read) / 701u;
    /* Every 10 s: how the stream keeps up, for hearing a crackle again. */
    if (sStats.frames % 600 == 0)
        CtrLog_Write(CTR_LOG_AUDIO, "AAudio: depth %.0f frames, rate %.1f Hz, underruns %u, drops %u",
                     (double)sDepth, (double)sStats.rateHz, (unsigned)sStats.underruns, (unsigned)sStats.drops);
}

/* No worker: the mixer runs on the game thread, once a frame. */
bool CtrAudio_StartWorker(void (*mix)(void)) { (void)mix; return false; }
bool CtrAudio_Kick(void) { return false; }
void CtrAudio_SetClock(bool on) { (void)on; }
void CtrAudio_LockSound(void) {}
void CtrAudio_UnlockSound(void) {}
