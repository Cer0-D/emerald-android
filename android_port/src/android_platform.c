/*
 * The platform layer on Android: 3ds_platform.c's frame loop (fast-forward,
 * real-time pacing, the battle's own speed) with the Activity's lifecycle in
 * place of APT, pthreads in place of libctru threads, and no console. Kept
 * in step with 3ds_platform.c: the pacing logic below is that file's.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#include "android_host.h"
#include "3ds_platform.h"
#include "3ds_daynight.h"
#include "3ds_data.h"
#include "3ds_audio.h"
#include "3ds_video.h"
#include "3ds_assets.h"
#include "3ds_local.h"
#include "3ds_bottom.h"

static CtrPlatformHooks sHooks;
static bool sInitialized, sExit, sWaiting;
static uint64_t sFrames, sLastFrame, sFrameMs;
static uint64_t sWorkStart, sLastTick, sExitStart;
static uint32_t sSeenResumes;
static CtrTiming sTiming;

uint64_t CtrPlatform_Milliseconds(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }
int64_t CtrPlatform_WallSeconds(void) { return (int64_t)time(NULL); }
uint64_t CtrPlatform_Ticks(void) { return svcGetSystemTick(); }
float CtrPlatform_TickMs(uint64_t ticks) { return (float)(ticks * 1000.0 / SYSCLOCK_ARM11); }
uint64_t CtrPlatform_FrameCount(void) { return sFrames; }
const CtrTiming *CtrPlatform_GetTiming(void) { return &sTiming; }
void CtrPlatform_NoteBottom(float ms) { sTiming.bottomMs = ms; }

/* ── Threads (the shim's threadCreate, and CtrPlatform_StartThread) ───── */
struct AndroidThread
{
    pthread_t thread;
    ThreadFunc entry;
    void *arg;
    bool detached;
};

static void *ThreadMain(void *p)
{
    struct AndroidThread *t = p;

    t->entry(t->arg);
    if (t->detached)
        free(t);
    return NULL;
}

Thread threadCreate(ThreadFunc entry, void *arg, size_t stackSize, int prio, int core, bool detached)
{
    struct AndroidThread *t = calloc(1, sizeof(*t));
    pthread_attr_t attr;

    (void)prio; (void)core;
    if (!t)
        return NULL;
    t->entry = entry;
    t->arg = arg;
    t->detached = detached;
    pthread_attr_init(&attr);
    if (stackSize < (256u << 10))
        stackSize = 256u << 10;
    pthread_attr_setstacksize(&attr, stackSize);
    if (detached)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->thread, &attr, ThreadMain, t) != 0)
    {
        pthread_attr_destroy(&attr);
        free(t);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return detached ? (Thread)1 : t;
}

Result threadJoin(Thread thread, u64 timeoutNs)
{
    (void)timeoutNs;
    if (thread && thread != (Thread)1)
        pthread_join(thread->thread, NULL);
    return 0;
}

void threadFree(Thread thread)
{
    if (thread && thread != (Thread)1)
        free(thread);
}

bool CtrPlatform_StartThread(void (*entry)(void *), void *arg, unsigned stack, int core)
{
    return threadCreate(entry, arg, stack, 0x31, core, true) != NULL;
}

void CtrPlatform_SleepUs(unsigned microseconds) { usleep(microseconds); }

_Static_assert(sizeof(CtrLock) == sizeof(LightLock), "CtrLock is a LightLock");
void CtrLock_Init(CtrLock *lock) { LightLock_Init((LightLock *)lock); }
void CtrLock_Lock(CtrLock *lock) { LightLock_Lock((LightLock *)lock); }
void CtrLock_Unlock(CtrLock *lock) { LightLock_Unlock((LightLock *)lock); }

/* ── Init / shutdown ──────────────────────────────────────────────────── */
bool CtrPlatform_Init(void)
{
    CtrLog_Init();
    sInitialized = true;
    sSeenResumes = AndroidHost_ResumeCount();
    CtrInput_Clear();
    sFrames = 0;
    sLastFrame = CtrPlatform_Milliseconds();
    CtrLog_Write(CTR_LOG_BOOT, "Android platform, files at %s", AndroidHost_FilesDir());
    if (!CtrFs_Init())
    {
        CtrLog_Write(CTR_LOG_ERROR, "platform init: filesystem failed");
        return false;
    }
    CtrSettings_Load();
    CtrScenario_Init();
    CtrProf_LogIdentity();
    if (!CtrData_Init())
        CtrPlatform_ShowDataError(CtrData_ErrorTitle(), CtrData_ErrorDetail());
    if (!CtrVideo_Init())
    {
        CtrLog_Write(CTR_LOG_ERROR, "platform init: video failed");
        return false;
    }
    CtrAudio_Init();
    LocalLink_Init();
    return true;
}

void CtrPlatform_SetHooks(const CtrPlatformHooks *hooks)
{
    if (hooks)
        sHooks = *hooks;
    else
        memset(&sHooks, 0, sizeof(sHooks));
}

void CtrPlatform_ReportMemory(const char *stage)
{
    struct mallinfo info = mallinfo();

    CtrLog_Write(CTR_LOG_BOOT, "memory %s: heap used=%u KiB, arena=%u KiB", stage,
                 (unsigned)(info.uordblks >> 10), (unsigned)(info.arena >> 10));
}

void CtrPlatform_RequestExit(void) { sExit = true; }

/* The activity went to the background: wait for it, then take up the game's
 * clocks as the 3DS's resume hook does. */
static void HandlePause(void)
{
    uint32_t resumes;

    if (AndroidHost_Paused())
    {
        CtrLog_Write(CTR_LOG_BOOT, "suspend");
        AndroidAudio_SetPaused(true);
        AndroidHost_WaitResumed();
        AndroidAudio_SetPaused(false);
    }
    resumes = AndroidHost_ResumeCount();
    if (resumes != sSeenResumes)
    {
        sSeenResumes = resumes;
        CtrDayNight_Resume();
        CtrInput_Clear();
        CtrBottom_OnResume();
        sLastFrame = CtrPlatform_Milliseconds();
        sLastTick = sExitStart = 0;
        CtrLog_Write(CTR_LOG_BOOT, "resume");
    }
}

bool CtrPlatform_BeginFrame(void)
{
    HandlePause();
    if (sExit)
        return false;
    sWorkStart = svcGetSystemTick();
    CtrInput_Scan();
    return true;
}

/* ── Frame pacing: 3ds_platform.c's ───────────────────────────────────── */
#define FRAME_MS (1000.0f / 60)
static bool sSoundTick = true;
static unsigned sEarned;
static unsigned sHidden;
static uint64_t sShownTick;
static float sPresentMs = 4.0f;

#define PACE_KEY "audio_clock"
#define PACE_HZ 59.8261f
#define PACE_LET_GO_FRAMES 4.0f
static bool sPace;
static bool sPaceMenus;
static uint64_t sPaceStart;
static float sPaceFrames;
static bool sPacedFrame;

static bool PaceBehind(uint64_t now)
{
    float elapsed;

    if (sPaceStart == 0)
    {
        sPaceStart = now;
        sPaceFrames = 0.0f;
    }
    sPaceFrames += 1.0f;
    elapsed = (float)((now - sPaceStart) * (double)PACE_HZ / SYSCLOCK_ARM11);
    if (elapsed - sPaceFrames > PACE_LET_GO_FRAMES || elapsed < sPaceFrames - 1.0f)
    {
        sPaceStart = now;
        sPaceFrames = 0.0f;
        return false;
    }
    return elapsed - sPaceFrames >= 1.0f;
}

static void PaceUpdate(void)
{
    bool setting = CtrSettings_GetInt(PACE_KEY, 1) != 0;
    bool on = setting && !CtrVideo_InBattle();
    bool menus = setting && CtrVideo_InBattle() && CtrVideo_TopHeld();

    if (on != sPace)
    {
        sPace = on;
        sPaceStart = 0;
        CtrAudio_SetClock(on);
    }
    if (menus != sPaceMenus)
    {
        sPaceMenus = menus;
        sPaceStart = 0;
    }
}

#define BADGE_MS 2000
static bool sBattleScene;
static uint64_t sBadgeUntil;

void CtrPlatform_SetBattleScene(bool on) { sBattleScene = on; }

bool CtrPlatform_BattleSpeedKeys(void)
{
    return sBattleScene && !LocalLink_InSession() && !CtrScenario_Active();
}

void CtrPlatform_StepBattleSpeed(int direction, bool wrap)
{
    CtrSettings_StepBattleSpeed(direction, wrap);
    sBadgeUntil = CtrPlatform_Milliseconds() + BADGE_MS;
}

int CtrPlatform_BattleSpeedBadge(void)
{
    if (!CtrPlatform_BattleSpeedKeys())
        return 0;
    return CtrPlatform_Milliseconds() < sBadgeUntil ? CtrSettings_BattleSpeedHalves() : 0;
}

static unsigned sFastEarly;
static float sGameAvgMs;
static unsigned sSpeedHalves = 2;
static bool FrameShown(uint64_t now)
{
    unsigned speed = LocalLink_InSession() ? 2
                   : CtrPlatform_BattleSpeedKeys() ? (unsigned)CtrSettings_BattleSpeedHalves()
                   : (unsigned)CtrSettings_SpeedHalves();

    sSpeedHalves = speed;
    sGameAvgMs = sTiming.gameMs > sGameAvgMs ? sTiming.gameMs : sGameAvgMs * 0.95f + sTiming.gameMs * 0.05f;
    float since = sShownTick ? (now - sShownTick) * 1000.0f / SYSCLOCK_ARM11 : FRAME_MS;

    sPacedFrame = (sPace || sPaceMenus) && speed <= 2;
    if (sPacedFrame)
    {
        bool behind = PaceBehind(now);

        if (behind && sHidden == 0)
        {
            sHidden = 1;
            return false;
        }
        sHidden = 0;
        return true;
    }
    sEarned += 2;
    /*
     * A frame is shown before its turn only when the frames run unshown since
     * the last have themselves taken a display frame's time. The present's
     * own cost is not counted: on the 3DS (3ds_platform.c) a game frame cost
     * milliseconds and the two together told whether one more fitted, but
     * here a game frame is a fraction of a millisecond while drawing a busy
     * voxel view (a map connection, seven maps in sight) takes the whole
     * frame - counted, it showed every frame and fast-forward ran at 1x.
     */
    if (speed <= 2 || since + sTiming.gameMs > FRAME_MS)
    {
        /* Shown before its turn: the speed given up for this frame. */
        if (speed > 2 && sEarned < speed)
            ++sFastEarly;
        sEarned = 0;
        return true;
    }
    if (sEarned >= speed)
    {
        sEarned -= speed;
        return true;
    }
    return false;
}

bool CtrPlatform_SoundTick(void) { return sSoundTick; }

/*
 * Fast-forward's share of a shown frame: at speed s (in halves) the game runs
 * s / 2 frames for each one shown, all but one unshown, and FrameShown shows
 * the next frame early - giving up the speed - whenever the time since the
 * last plus a game frame and a present would not fit in one display frame.
 * So the work a shown frame may do after drawing (the voxel world building
 * ahead, CtrVoxel_AfterSubmit) must leave those game frames their time: up to
 * now it filled the frame, and wherever there was much to build - the next
 * map coming into view at a connection - every frame came out shown, at 1x.
 * The game frame's cost rises at once and falls slowly; a little to spare.
 */
/* Fast-forward over the last 5 s, for the log: frames run, shown, shown
 * early (FrameShown), the worst game frame and present. */
static unsigned sFastFrames, sFastShown;
static float sFastGamePeak, sFastPresentPeak;
static uint64_t sFastStart;

static void FastForwardLog(bool shown)
{
    uint64_t now = CtrPlatform_Milliseconds();

    if (sSpeedHalves <= 2)
    {
        sFastStart = 0;
        return;
    }
    if (!sFastStart)
    {
        sFastStart = now;
        sFastFrames = sFastShown = sFastEarly = 0;
        sFastGamePeak = sFastPresentPeak = 0.0f;
    }
    ++sFastFrames;
    sFastShown += shown;
    if (sTiming.gameMs > sFastGamePeak) sFastGamePeak = sTiming.gameMs;
    if (shown && sPresentMs > sFastPresentPeak) sFastPresentPeak = sPresentMs;
    if (now - sFastStart >= 5000)
    {
        CtrLog_Write(CTR_LOG_VIDEO, "fast-forward x%.1f: %.1f game fps, %u shown, %u early, game %.1f ms "
                     "(peak %.1f), present peak %.1f ms, reserve %.1f ms",
                     sSpeedHalves / 2.0f, sFastFrames * 1000.0f / (float)(now - sFastStart), sFastShown, sFastEarly,
                     sGameAvgMs, sFastGamePeak, sFastPresentPeak, AndroidPlatform_FrameReserveMs());
        sFastStart = 0;
    }
}

float AndroidPlatform_FrameReserveMs(void)
{
    unsigned hidden;

    if (sSpeedHalves <= 2 || sPacedFrame)
        return 0.0f;
    hidden = (sSpeedHalves - 1) / 2;    /* 1.5x and 2x: 1, 2.5x and 3x: 2, 4x: 3 */
    return (float)hidden * sGameAvgMs + 1.0f;
}

void CtrPlatform_EndFrame(void)
{
    if (sWaiting)
        return;
    sWaiting = true;
    uint64_t endStart = svcGetSystemTick();
    sTiming.gameMs = (endStart - sWorkStart) * 1000.0f / SYSCLOCK_ARM11;
    PaceUpdate();
    bool shown = FrameShown(endStart);
    sSoundTick = shown || sPacedFrame;
    if (sHooks.audioFrame)
        sHooks.audioFrame();
    if (sHooks.vblank)
        sHooks.vblank();
    uint64_t presentStart = svcGetSystemTick();
    sTiming.vblankMs = (presentStart - endStart) * 1000.0f / SYSCLOCK_ARM11;
    if (sHooks.videoPresent && shown)
    {
        sHooks.videoPresent();
        sPresentMs = (svcGetSystemTick() - presentStart) * 1000.0f / SYSCLOCK_ARM11
                     - CtrVideo_GetStats()->waitMs;
        if (sPresentMs < 0)
            sPresentMs = 0;
        sShownTick = svcGetSystemTick();
    }
    FastForwardLog(shown);
    ++sFrames;
    uint64_t now = CtrPlatform_Milliseconds();
    sFrameMs = now - sLastFrame;
    sLastFrame = now;
    uint64_t tick = svcGetSystemTick();
    sTiming.frameMs = sLastTick ? (tick - sLastTick) * 1000.0 / SYSCLOCK_ARM11 : 0;
    sLastTick = tick;
    sTiming.workMs = (tick - sWorkStart) * 1000.0 / SYSCLOCK_ARM11;
    sTiming.workMs -= CtrVideo_GetStats()->waitMs;
    if (sTiming.workMs < 0)
        sTiming.workMs = 0;
    if (sTiming.workMs > sTiming.peakWorkMs)
        sTiming.peakWorkMs = sTiming.workMs;
    if (sTiming.workMs > 1000.0f / 60)
        ++sTiming.slowFrames;
    CtrProf_EndFrame(sTiming.workMs, CtrVideo_GetStats()->waitMs, sFrames);
    sWaiting = false;
}

void CtrPlatform_Shutdown(void)
{
    if (!sInitialized)
        return;
    sInitialized = false;
    CtrProf_Close();
    CtrAssets_StopWarmup();
    CtrSettings_Shutdown();
    if (sHooks.shutdown)
        sHooks.shutdown();
    LocalLink_Shutdown();
    CtrVideo_Shutdown();
    CtrAudio_Shutdown();
    CtrLog_Write(CTR_LOG_BOOT, "clean shutdown, platform frames=%llu", (unsigned long long)sFrames);
    CtrScenario_Close();
    CtrFs_Shutdown();
    CtrLog_Close();
}

/* No console on Android: the message goes to the log, and the video
 * backend shows it on the top screen (android_video.c). */
void CtrPlatform_ShowDataError(const char *title, const char *detail)
{
    CtrLog_Write(CTR_LOG_ERROR, "DATA ERROR: %s / %s", title, detail);
    CtrLog_ShowFatal(title);
    for (;;)
        sleep(1);
}

void CtrPlatform_Fatal(const char *reason)
{
    CtrLog_Write(CTR_LOG_ERROR, "FATAL: %s", reason);
    CtrLog_ShowFatal(reason);
    for (;;)
        sleep(1);
}

/* ── Profiling marks (3ds_platform.c's) ──────────────────────────────── */
void Port_ProfileMark(const char *label)
{
    static uint64_t sLast;
    static unsigned sLogged;
    uint64_t now = svcGetSystemTick();
    float ms = sLast ? (float)((now - sLast) * 1000.0 / SYSCLOCK_ARM11) : 0.0f;

    sLast = now;
    if (ms >= 0.8f && ms < 200.0f && sLogged < 400)
    {
        ++sLogged;
        CtrLog_Write(CTR_LOG_VIDEO, "PROF %s %.1f ms", label, ms);
    }
}

#define PROF_BUCKETS 12
static struct { const char *name; uint64_t ticks; } sProf[PROF_BUCKETS];
static uint64_t sProfLast;
static unsigned sProfFrames;

void Port_ProfBegin(void)
{
    if (sProfFrames >= 600)
    {
        char text[256] = "";
        size_t used = 0;

        for (unsigned i = 0; i < PROF_BUCKETS && sProf[i].name != NULL && used < sizeof(text) - 32; ++i)
        {
            used += (size_t)snprintf(text + used, sizeof(text) - used, " %s=%.2f", sProf[i].name,
                                     sProf[i].ticks * 1000.0 / SYSCLOCK_ARM11 / sProfFrames);
            sProf[i].ticks = 0;
        }
        CtrLog_Write(CTR_LOG_VIDEO, "PROF vblank handler (ms/frame):%s", text);
        sProfFrames = 0;
    }
    ++sProfFrames;
    sProfLast = svcGetSystemTick();
}

void Port_ProfAcc(const char *name)
{
    uint64_t now = svcGetSystemTick();

    for (unsigned i = 0; i < PROF_BUCKETS; ++i)
    {
        if (sProf[i].name == name || sProf[i].name == NULL)
        {
            sProf[i].name = name;
            sProf[i].ticks += now - sProfLast;
            break;
        }
    }
    sProfLast = now;
}
