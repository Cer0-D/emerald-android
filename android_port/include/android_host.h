/*
 * What the Java shell hands to native code (android_main.c), for the rest of
 * the Android backend: the two screens' windows, input, paths, lifecycle.
 */
#ifndef ANDROID_HOST_H
#define ANDROID_HOST_H

#include <stdbool.h>
#include <stdint.h>

struct ANativeWindow;

enum { HOST_SCREEN_TOP = 0, HOST_SCREEN_BOTTOM = 1, HOST_SCREENS = 2 };

/* The window to draw a screen into, or NULL; *generation changes whenever
 * the window does (the video backend then makes a new EGL surface). The
 * caller holds a reference until it calls AndroidHost_ReleaseWindow. */
struct ANativeWindow *AndroidHost_AcquireWindow(int screen, uint32_t *generation);
void AndroidHost_ReleaseWindow(struct ANativeWindow *window);
/* Called by the render thread once it no longer uses the old window: lets
 * surfaceDestroyed return. */
void AndroidHost_WindowDone(int screen, uint32_t generation);

/* The app's external files directory (romfs/ and sdmc/ live there). */
const char *AndroidHost_FilesDir(void);
/* The folder of it romfs:/ maps to: "romfs" (development) or "engine" (a
 * release's engine files, the game data in sdmc's emerald3ds.pak). */
const char *AndroidHost_RomfsDir(void);

/* Blocks the game thread while the activity is paused. */
void AndroidHost_WaitResumed(void);
bool AndroidHost_Paused(void);
/* The audio stream paused with the activity (android_audio.c). */
void AndroidAudio_SetPaused(bool paused);
/* While fast-forwarding, the time a shown frame must leave for the game
 * frames run unshown before the next (android_platform.c); 0 at 1x. */
float AndroidPlatform_FrameReserveMs(void);
uint32_t AndroidHost_ResumeCount(void);

/* Input as the shell last reported it (android_input.c reads it). */
typedef struct
{
    uint32_t buttons;            /* HOST_BTN_* */
    float stickX, stickY;        /* -1..1 */
    bool touchActive;
    float touchX, touchY;        /* bottom view pixels */
    int touchW, touchH;          /* bottom view size */
} AndroidHostInput;

enum
{
    HOST_BTN_A = 1 << 0, HOST_BTN_B = 1 << 1, HOST_BTN_X = 1 << 2, HOST_BTN_Y = 1 << 3,
    HOST_BTN_L = 1 << 4, HOST_BTN_R = 1 << 5, HOST_BTN_ZL = 1 << 6, HOST_BTN_ZR = 1 << 7,
    HOST_BTN_START = 1 << 8, HOST_BTN_SELECT = 1 << 9,
    HOST_BTN_UP = 1 << 10, HOST_BTN_DOWN = 1 << 11, HOST_BTN_LEFT = 1 << 12, HOST_BTN_RIGHT = 1 << 13,
};

void AndroidHost_GetInput(AndroidHostInput *out);

/* Where the 320x240 bottom picture sits in a w x h bottom view (android_video.c). */
void AndroidVideo_BottomViewport(int w, int h, int *x, int *y, int *vw, int *vh);

/* The fatal message to show, or NULL (android_log.c). */
const char *AndroidLog_FatalReason(void);

/* Debug: a capture of both screens was asked for (am start ... --ez capture true). */
bool AndroidHost_TakeCaptureRequest(void);

#endif
