/*
 * Features of the 3DS backend not on Android yet, answered as the 3DS
 * answers when they are off: local wireless (no adapter session), the
 * Pokédex device on the top screen, the frame profiler.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "3ds_platform.h"
#include "3ds_dex_top.h"

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
