/*
 * Features of the 3DS backend not on Android yet, answered as the 3DS
 * answers when they are off: local wireless (no adapter session), the
 * Pokédex device on the top screen, the frame profiler.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "3ds_platform.h"
#include "3ds_local.h"
#include "3ds_dex_top.h"

/* ── Local wireless (3ds_local.c: UDS) ────────────────────────────────── */
void LocalLink_Init(void) {}
void LocalLink_Shutdown(void) {}
uint8_t LocalLink_Enabled(void) { return 0; }
uint16_t LocalLink_MyId(void) { return 0; }
uint32_t LocalLink_Generation(void) { return 0; }
void LocalLink_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data)
{
    (void)hosting; (void)nextSlot; (void)data;
}
int LocalLink_GetPeers(struct LocalPeer *out, int max) { (void)out; (void)max; return 0; }
void LocalLink_Send(uint16_t dest, const void *data, uint32_t len) { (void)dest; (void)data; (void)len; }
uint8_t LocalLink_PollEvent(struct LocalEvent *ev) { (void)ev; return 0; }
uint32_t LocalLink_Milliseconds(void) { return (uint32_t)CtrPlatform_Milliseconds(); }
void LocalLink_StartSession(uint8_t kind) { (void)kind; }
void LocalLink_LeaveSession(void) {}
uint8_t LocalLink_GetSession(void) { return 0; }
uint8_t LocalLink_GetSessionKind(void) { return 0; }
uint8_t LocalLink_InSession(void) { return 0; }
void LocalLink_LeaveBeforeSleep(void) {}
void LocalLink_AfterWake(void) {}

/* ── The Pokédex on the top screen (3ds_dex_top.c) ─────────────────────── */
void CtrDexTop_Show(uint16_t national, const uint16_t *picture, unsigned frames, const char *name)
{
    (void)national; (void)picture; (void)frames; (void)name;
}
void CtrDexTop_BackdropBegin(void) {}
void CtrDexTop_BackdropRow(unsigned y, const uint16_t *row) { (void)y; (void)row; }
bool CtrDexTop_BackdropComplete(void) { return true; }
void CtrDexTop_Hide(void) {}
bool CtrDexTop_Active(void) { return false; }

/* ── Profiler (3ds_prof.c) ────────────────────────────────────────────── */
void CtrProf_EndFrame(float workMs, float waitMs, uint64_t frame) { (void)workMs; (void)waitMs; (void)frame; }
void CtrProf_LogIdentity(void)
{
    extern const char gCtrBuildVersion[], gCtrBuildCommit[];
    CtrLog_Write(CTR_LOG_BOOT, "Emerald Android %s (%s)", gCtrBuildVersion, gCtrBuildCommit);
}
static uintptr_t sScene;
uintptr_t CtrProf_Scene(void) { return sScene; }
void CtrProf_Close(void) {}
uint32_t Port_ProfTick(void) { return 0; }
void Port_ProfAdd(unsigned section, uint32_t start) { (void)section; (void)start; }
void Port_ProfScene(const void *callback2) { sScene = (uintptr_t)callback2; }
