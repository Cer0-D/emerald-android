/*
 * Input on Android: the Thor's buttons and left stick as the 3DS's buttons
 * and Circle Pad, the bottom display's touches as the touch screen in its
 * 320x240 pixels (where android_video.c draws the bottom picture). The
 * decoding itself is the 3DS's (3ds_input_decode.c, CtrInput_Update).
 */
#include <string.h>

#include "android_host.h"
#include "3ds_platform.h"
#include "3ds_bottom.h"

static CtrInput sInput;
static uint32_t sLastButtons;
static bool sDbgTouch;

void CtrInput_Clear(void)
{
    memset(&sInput, 0, sizeof(sInput));
    sLastButtons = 0;
}

void CtrInput_Scan(void)
{
    static const uint32_t keys[] =
    {
        HOST_BTN_A, HOST_BTN_B, HOST_BTN_SELECT, HOST_BTN_START, HOST_BTN_RIGHT,
        HOST_BTN_LEFT, HOST_BTN_UP, HOST_BTN_DOWN, HOST_BTN_R, HOST_BTN_L, HOST_BTN_X, HOST_BTN_Y
    };
    AndroidHostInput host;
    CtrInputSample sample = {0};

    AndroidHost_GetInput(&host);
    uint32_t down = host.buttons & ~sLastButtons;
    sLastButtons = host.buttons;
    /* ZR and ZL step the fast-forward speed, as on a New 3DS. */
    if ((down & HOST_BTN_ZR) && !CtrScenario_Active())
        CtrSettings_StepSpeed(1, false);
    if ((down & HOST_BTN_ZL) && !CtrScenario_Active())
        CtrSettings_StepSpeed(-1, false);
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (host.buttons & keys[i])
            sample.buttons |= 1u << i;
    /* The Circle Pad's range is about +-156; up is positive. */
    sample.circleX = (int16_t)(host.stickX * 156.0f);
    sample.circleY = (int16_t)(-host.stickY * 156.0f);
    if (host.touchActive && host.touchW > 0 && host.touchH > 0)
    {
        int vx, vy, vw, vh;

        AndroidVideo_BottomViewport(host.touchW, host.touchH, &vx, &vy, &vw, &vh);
        float px = (host.touchX - vx) * CTR_BOTTOM_WIDTH / (float)vw;
        float py = (host.touchY - vy) * CTR_BOTTOM_HEIGHT / (float)vh;
        if (px >= 0 && py >= 0 && px < CTR_BOTTOM_WIDTH && py < CTR_BOTTOM_HEIGHT)
        {
            sample.touchActive = true;
            sample.touchX = (uint16_t)px;
            sample.touchY = (uint16_t)py;
        }
        if (!sDbgTouch)
            CtrLog_Write(CTR_LOG_VIDEO, "TOUCHDBG raw %.0f,%.0f in %dx%d view %d,%d %dx%d -> %.1f,%.1f", host.touchX, host.touchY,
                         host.touchW, host.touchH, vx, vy, vw, vh, px, py);
        sDbgTouch = true;
    }
    if (!host.touchActive)
        sDbgTouch = false;
    CtrScenario_Input(&sample);
    CtrCycle_Input(&sample);
    CtrInput_Update(&sInput, &sample);
}

const CtrInput *CtrInput_Get(void)
{
    return &sInput;
}
