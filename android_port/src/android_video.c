/*
 * Video on Android, first stage: a software GBA renderer.
 *
 * The 3DS compositor (3ds_video.c) draws the emulated GBA registers, VRAM, OAM
 * and palette with the PICA200 and knows the port's every screen (stages,
 * centred menus, the battle at 1.4x, the bottom screen's game screens, the
 * voxel world). This file is the simple, complete-enough path to see the game
 * on the Thor while that compositor is brought over: each frame it renders
 * the GBA picture line by line on the CPU (text and affine backgrounds,
 * sprites, windows, blending) into a 400x240 image - or 240x160 centred, for
 * the screens the game composes as one GBA picture - and shows it scaled on
 * the top display. The bottom display shows the bottom screen's canvas
 * (3ds_bottom_ui.c), as the 3DS's LCD would.
 *
 * Both displays are drawn from the game thread with one EGL context: a
 * window surface per display, swap interval 0, and the frame paced here to
 * the GBA's 59.73 Hz.
 */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "android_host.h"
#include "3ds_platform.h"
#include "3ds_video.h"
#include "3ds_bottom.h"
#include "citro3d.h"
#include "ctr_voxel.h"
#include "voxel_swoop.h"
#include "voxel_battle.h"
#include "3ds_battle_hud.h"
#include "ctr_battle_layout.h"
#include "3ds_corner_art.h"
#include "3ds_daynight.h"

/* The battle's hints to the compositor (battle_intro.c, battle_bg.c). */
unsigned char CtrBattleIntro_Sliding(void);
unsigned char CtrBattleBg_MoveBgShown(void);

#define TOP_W CTR_GAME_WIDTH
#define TOP_H CTR_GAME_HEIGHT
#define GBA_W 240
#define GBA_H 160
#define FRAME_NS 16742706LL /* 59.7275 Hz */

/* ── Emulated hardware the game draws into ─────────────────────────────── */
static CtrVideoMemory sMem;
static CtrVideoStats sStats;

static inline uint16_t Reg(unsigned offset) { return sMem.regs[offset >> 1]; }

/* Hints from the game (3ds_video.h) that this renderer uses. */
static bool sStage, sBattle, sTransition, sHoldTop, sStageZoomed, sFieldBanner;
/* The 2D field's text layer: 3ds_video.c draws BG0 moved right by this much
 * and unwrapped (CTR_FIELD_UI_SHIFT), its 32 columns only reaching 256px:
 * the standard text box (x 16..232) centred on the screen. */
#define FIELD_UI_SHIFT (TOP_W / 2 - 124)
static bool sFieldUi;
static unsigned sCentred;
static uint16_t sLineRegs[GBA_H][CTR_LINE_REGS];
static unsigned sLineRegCount;

/* ── Output images ────────────────────────────────────────────────────── */
static uint32_t sTop[TOP_W * TOP_H];
/* The bottom canvas in the 3DS framebuffer's layout: column-major, each
 * column bottom-to-top (3ds_bottom.h). Uploaded as a 240x320 texture. */
static uint16_t sBottom[CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT];
/* Where RenderPicture writes: sTop, or the 2D field under the swoop's
 * crossfade (sField2D). */
static uint32_t sField2D[TOP_W * TOP_H];
static uint32_t *sOut = sTop;
/* A game screen in the bottom screen's window (CtrVideo_BottomInUse, below). */
static bool sBottomInUse, sBottomNative, sNativeMenus;
static uint32_t sBottomPicture[TOP_W * GBA_H];
/*
 * The voxel world's overlay (3ds_video.c ComposeVoxelOverlay): only what the
 * game lays over the world - its text layer (BG0) and the sprites placed on
 * the screen or tagged as weather - premultiplied RGBA over transparency.
 * The map's own sprites are cards in the 3D scene (voxel_entities.c).
 */
static bool sOverlay;
/* The swoop has squashed the cards away: the field's own sprites go into
 * the overlay too, under everything else of it. */
static bool sOverlayFieldObjs;
/* Backgrounds left out of the picture (bit per BG): the 2D field under the
 * swoop's crossfade has no text layer, the battle over the voxel world no
 * scenery (sWorldLayers). */
static unsigned sExcludeLayers;
/* The battle over the voxel world: the backdrop is transparent - the world
 * shows there - except where the windows hide BG3 (the intro's curtain). */
static bool sWorldBehind;
enum { OBJ_FIELD, OBJ_WEATHER, OBJ_SCREEN, OBJ_CLASSES };
static bool sBottomDirty = true;

/* ── GBA renderer ─────────────────────────────────────────────────────── */
typedef struct
{
    uint16_t color;   /* BGR555 */
    uint8_t prio;     /* 0..3, 4 = none */
    uint8_t flags;    /* 1 = semi-transparent OBJ */
} Pixel;

#define MAX_LINE 512
static Pixel sBgLine[4][MAX_LINE];
static Pixel sObjLine[MAX_LINE];
static Pixel sObjClassLine[OBJ_CLASSES][MAX_LINE];
static uint8_t sObjWin[MAX_LINE];
static int32_t sAffX[2], sAffY[2];

static inline uint32_t ToRGBA(uint16_t c)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint16_t Pal(unsigned index) { return sMem.palette[index] & 0x7FFF; }

/* A per-line register (HBlank DMA effects), or the latched one. */
static unsigned sLine;
static inline uint16_t LReg(unsigned offset)
{
    if (sLineRegCount && sLine < sLineRegCount && offset >= CTR_LINE_REG_FIRST
        && offset < CTR_LINE_REG_FIRST + CTR_LINE_REGS * 2)
        return sLineRegs[sLine][(offset - CTR_LINE_REG_FIRST) >> 1];
    return Reg(offset);
}

/*
 * The 2D battle's view wider than the GBA's picture (3ds_video.c DrawBattleBg):
 * rendered from GBA (sOrgX, sOrgY), so that the magnified scene covers the
 * whole top screen. The terrain (BG3) wraps across and repeats its top tile
 * row above, and is split between the two sides as the game shifts their
 * sprites (ctr_battle_layout.h); the text layer, and the other backgrounds
 * unless their map wraps, stay inside the picture; a window that reaches an
 * edge of the picture reaches the edge of the view.
 */
static bool sBattleWide;
static int sOrgX, sOrgY;
/*
 * A stage (the intro, the title, the credits...) magnified to the screen's
 * height, its view wider than the GBA's picture (after 3ds_video.c
 * DrawStageBg, for margins a third as wide as the 3DS's): a background whose
 * map carries its art on past the picture wraps across the margins
 * (StageLayerWraps); any other is mirrored about the picture's edge. Nothing is
 * faded: cut off 24 pixels out, the 3DS's fade to black read as bands. The
 * intro's leaves scene fills the width instead. Sprites, windows and the
 * other wide rules are the battle's (sWide).
 */
static bool sStageWide;
/*
 * Sprites in a stage's margins: the scene stops moving one once it is past
 * the GBA screen's edge - done with it - and leaves it there, which the
 * wider view would show for good. One standing wholly outside the picture is
 * carried on off the screen at the pace it was going (sObjDrift), so it
 * leaves as it would have; one that never moved (waiting off screen to come
 * in) is not drawn. Sideways only: a flier bobs up and down where it was left.
 */
static int16_t sObjLastX[128];
static uint32_t sObjStill[128];
static bool sObjParked[128];
/* Where a sprite left standing outside the picture is drawn from its place:
 * carried on off the screen at the pace it went (StageSprites). */
static int16_t sObjDrift[128];
/* This render's stage layers that wrap across the margins (StageRender),
 * and whether any does. */
static bool sStageWraps[4], sStageAnyWraps, sStageLayerEmpty[4];
/*
 * A centred screen laid out as a stage (StageCentredFill): as the 3DS
 * composes it (3ds_video.c, sCentredFills), only its background is carried
 * out - each margin tile the tilemap's own edge tile in its row, moved in by
 * the inset past a border - and its other layers and windows stay in the
 * picture. The edge is the tilemap's, not the screen's: when the professor's
 * speech slides its background aside, the margins keep its empty side.
 */
static bool sStageMenu;
/*
 * How the centred screen on show fills its margins (3ds_video.c,
 * CentredFill): the layers carried out - each margin tile the tilemap's edge
 * column moved in by the insets, or read on through the tilemap as the GBA
 * wraps it (wrap), or one tile of it (tile), or one tilemap entry (entry). A
 * screen without one is magnified all the same, its margins black.
 */
typedef struct
{
    uint8_t layers;
    int8_t insetL, insetR;
    bool backmost, wrap, tile, entry;
    uint8_t tileColumn, tileRow;
    uint16_t tileEntry;
} StageFill;
static StageFill sStageFill;
/* The centred screen's affine map repeated in its margins, as the 3DS fills
 * them (3ds_video.c, DrawAffineBg) - but the blender's bowl and the
 * roulette's wheel, which would show copies of themselves. */
static bool sStageAffineWrap;
/* The Game Corner's backdrop round its screens (3ds_corner_art.c), as
 * GBA colours: the roulette's felt under the whole picture, the casino's
 * wall beside the slot machine. */
static uint16_t sCornerPattern[CTR_CORNER_PATTERN_DIM * CTR_CORNER_PATTERN_DIM];
static unsigned sCornerShown;
static uint16_t CornerBackdrop(int x, int y, uint16_t backdrop);
/* A still layer over a wrapping one (the title's Rayquaza over its clouds)
 * thins out across this many margin pixels, an ordered dither, showing the
 * scene under it rather than a mirrored copy of itself. */
#define STAGE_THIN_PIXELS 12
static const uint8_t sBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
#define sWide (sBattleWide || sStageWide)
#define STAGE_ZOOM ((float)TOP_H / GBA_H)
#define STAGE_MARGIN_X ((int)((TOP_W / STAGE_ZOOM - GBA_W) / 2.0f + 0.999f))
#define STAGE_VIEW_W (GBA_W + 2 * STAGE_MARGIN_X)

/*
 * The intro's leaves scene (behind the Game Freak logo, then the meadow it
 * pans up to): four still layers whose art ends at the GBA screen's edges.
 * Repeated or mirrored they read as copies, and the 3DS's margin art
 * (stage/leaves.bin) was made to fade into black over 56 pixels: unfaded,
 * its seam a dozen pixels out showed. Here the scene fills the width
 * instead, as a letterboxed one does (StageRender). Known by its
 * backgrounds' control registers (intro.c, Task_Scene1_Load): four 256x512
 * 4bpp maps on character base 0 at screen bases 16, 18, 20 and 22.
 */
static bool LeavesScene(void)
{
    for (unsigned bg = 0; bg < 4; ++bg)
        if (Reg(8 + bg * 2) != (bg | ((16 + 2 * bg) << 8) | 0x8000))
            return false;
    return true;
}


/* The scene's pass: the text layer stops where the GBA's text box begins
 * (line 112), which the band's own pass draws (BattleRender). */
static bool sBattleScene;
unsigned char CtrBattleBg_MoveBgShown(void);
unsigned char CtrBattleIntro_Sliding(void);
/* This frame's battle intro slides the terrain in on BG1 and BG2 (read once
 * a render, BattleRender). */
static bool sBattleSliding;

/*
 * A move's background that is one picture for the GBA's screen - Psychic,
 * Thunder, Ice, Dark, Solar Beam, the impacts: art over the 240x160 picture
 * and nothing in the rows the GBA never shows - has nothing of its own to
 * carry into the margins (3ds_video.c BattlePicture): such a layer is drawn
 * magnified instead, uniformly, to cover the whole top screen with a few
 * pixels to spare, a GBA point p drawn at f + k (p - f). The fixed point f
 * is the lowest that still brings the picture's top edge over the screen's,
 * so the battlers' part of the art moves as little as it can. Backgrounds
 * made to wrap (Sandstorm, the water, the fog) keep the wrap; the terrain is
 * never one.
 */
#define BATTLE_PICTURE_SPARE 4.0f
#define BATTLE_PICTURE_SCALE ((CTR_GAME_WIDTH / CTR_BATTLE_ZOOM + 2 * BATTLE_PICTURE_SPARE) / 240.0f)
/* GBA line on the screen's top edge: (120, 112) is on the text box's top. */
#define BATTLE_PICTURE_SCREEN_TOP (112.0f - (CTR_GAME_HEIGHT - 48) / CTR_BATTLE_ZOOM)
#define BATTLE_PICTURE_FIX_Y ((BATTLE_PICTURE_SCREEN_TOP - BATTLE_PICTURE_SPARE) / (1.0f - BATTLE_PICTURE_SCALE))
#define BATTLE_PICTURE_FIX_X 120.0f
/* This render's picture layers (BattleRender). */
static bool sBattlePicture[4];

/* Whether a background tile has any pixel drawn (3ds_video.c GetTileSlot). */
static bool TileShown(unsigned chars, unsigned entry, bool color256)
{
    unsigned bytes = color256 ? 64 : 32, address = chars + (entry & 1023) * bytes;

    if (address + bytes > 0x10000)
        return false;
    for (unsigned i = 0; i < bytes; ++i)
        if (sMem.vram[address + i])
            return true;
    return false;
}

static unsigned BgMapEntry(unsigned map, unsigned size, unsigned column, unsigned row)
{
    unsigned at;

    column &= (size & 1) ? 63u : 31u;
    at = map + CtrVideo_TextMapOffset(0, row, size) + (column & 31) * 2 + ((column >> 5) & 1) * 2048;
    return at + 1 < 0x10000 ? (unsigned)(sMem.vram[at] | sMem.vram[at + 1] << 8) : 0;
}

/* A battle background that is one picture: four fifths of the screen's
 * 30x20 tiles drawn, and next to nothing in the map's rows below them. */
static bool BattlePictureLayer(unsigned bg)
{
    unsigned control, size, map, chars, scrollX, scrollY, rows;
    unsigned shown = 0, hidden = 0, hiddenTiles = 0, lastEntry = ~0u;
    bool color256, lastFull = false;

    if (!sBattle || bg == 0 || bg > 3 || (bg == 3 && !CtrBattleBg_MoveBgShown()))
        return false;
    if ((Reg(0) & 7) != 0 && !((Reg(0) & 7) == 1 && bg < 2))
        return false; /* text backgrounds only */
    control = Reg(8 + bg * 2);
    size = control >> 14;
    map = ((control >> 8) & 31) * 0x800;
    chars = ((control >> 2) & 3) * 0x4000;
    color256 = (control & 128) != 0;
    scrollX = Reg(0x10 + bg * 4) & 511;
    scrollY = Reg(0x12 + bg * 4) & 511;
    rows = (size & 2) ? 64 : 32;
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned c = 0; c < 30; ++c)
        {
            unsigned entry = BgMapEntry(map, size, (scrollX >> 3) + c, ((scrollY >> 3) + r) & (rows - 1));

            if (entry != lastEntry)
            {
                lastEntry = entry;
                lastFull = TileShown(chars, entry, color256);
            }
            if (r < 20)
                shown += lastFull;
            else
            {
                hidden += lastFull;
                ++hiddenTiles;
            }
        }
    return shown * 5 >= 600 * 4 && hidden * 10 <= hiddenTiles;
}

static int BattleTerrainLine(int y)
{
    const int top = CTR_BATTLE_SIDE_SPLIT - CTR_BATTLE_ENEMY_RISE;
    const int bottom = CTR_BATTLE_SIDE_SPLIT + CTR_BATTLE_PLAYER_DROP;

    if (y < top) return y + CTR_BATTLE_ENEMY_RISE;
    if (y < bottom) return CTR_BATTLE_SIDE_SPLIT + ((y - bottom) & 3);
    /* The terrain's map is empty from line 112: its last four lines go on. */
    if (y - CTR_BATTLE_PLAYER_DROP >= 112)
        return 112 - 4 + ((y - CTR_BATTLE_PLAYER_DROP - 112) & 3);
    return y - CTR_BATTLE_PLAYER_DROP;
}

static void TextLine(int bg, int y, int width)
{
    uint16_t cnt = Reg(0x08 + 2 * bg);
    unsigned prio = cnt & 3;
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenBase = ((cnt >> 8) & 31) * 0x800;
    bool bpp8 = (cnt >> 7) & 1;
    unsigned size = (cnt >> 14) & 3;
    unsigned mapW = (size & 1) ? 512 : 256, mapH = (size & 2) ? 512 : 256;
    unsigned hofs = LReg(0x10 + 4 * bg) & 0x1FF, vofs = LReg(0x12 + 4 * bg) & 0x1FF;
    int srcY = y;
    bool inside = true;     /* only the GBA picture's columns */
    bool picture = sBattleWide && sBattleScene && sBattlePicture[bg];
    if (sBattleWide)
    {
        int gy = y + sOrgY;
        bool terrain = bg == 3 && !CtrBattleBg_MoveBgShown();

        srcY = terrain ? BattleTerrainLine(gy) : gy;
        if (picture)
        {
            float py = BATTLE_PICTURE_FIX_Y + (gy - BATTLE_PICTURE_FIX_Y) / BATTLE_PICTURE_SCALE;

            if (py < 0.0f || py >= GBA_H)
            {
                for (int x = 0; x < width; x++)
                    sBgLine[bg][x].prio = 4;
                return;
            }
            srcY = (int)py;
        }
        /* The terrain, and its pieces sliding in on BG1/BG2 through the
         * intro, carry on across the view; the rest stays in the picture
         * unless its map wraps. */
        inside = bg == 0 || (bg != 3 && mapW != 512 && !(sBattleSliding && (bg == 1 || bg == 2)));
        if (bg == 0 && sBattleScene && gy >= 112)
        {
            for (int x = 0; x < width; x++)
                sBgLine[bg][x].prio = 4;
            return;
        }
        if (srcY < 0)
        {
            if (bg != 3)
            {
                for (int x = 0; x < width; x++)
                    sBgLine[bg][x].prio = 4;
                return;
            }
            srcY = ((srcY % 8) + 8) % 8; /* the top tile row, repeated */
        }
        if (srcY >= GBA_H && bg != 3 && bg != 0)
            inside = true;
    }
    unsigned sy = ((unsigned)srcY + vofs) & (mapH - 1);
    unsigned ty = sy >> 3, py = sy & 7;
    const uint8_t *vram = sMem.vram;

    for (int x = 0; x < width; x++)
    {
        int lx = x, edgeTile = -1;
        unsigned edgePx = 0;
        if (sStageWide)
        {
            /* A layer whose map carries its art on, as the GBA wraps it
             * (StageLayerWraps); any other mirrored about the picture's
             * edge. */
            lx = x + sOrgX;
            if (sStageMenu && (lx < 0 || lx >= GBA_W))
            {
                if (!((sStageFill.layers >> bg) & 1))
                {
                    sBgLine[bg][x].prio = 4;
                    continue;
                }
                /* Wrapping: the tilemap read on at the scroll, as is. Else
                 * the tilemap's edge column (or the one tile), unscrolled,
                 * at the picture's fine scroll; its row the line's (sy). */
                if (!sStageFill.wrap)
                {
                    edgeTile = sStageFill.tile || sStageFill.entry ? sStageFill.tileColumn
                             : lx < 0 ? sStageFill.insetL : 29 - sStageFill.insetR;
                    edgePx = (unsigned)(lx + (int)(hofs & 7)) & 7;
                }
            }
            /* Mirrored about the picture's edge: 24 pixels of the scene's
             * own texture (clouds, grass), where the edge band repeated
             * read as a comb. */
            if (sStageWraps[bg] || sStageMenu)
                ;
            else if (sStageAnyWraps && (lx < 0 || lx >= GBA_W))
            {
                int d = lx < 0 ? -lx : lx - GBA_W + 1;

                if (d >= STAGE_THIN_PIXELS || sBayer4[(y & 3) * 4 + (x & 3)] < (unsigned)(d * 16 / STAGE_THIN_PIXELS))
                {
                    sBgLine[bg][x].prio = 4;
                    continue;
                }
                lx = lx < 0 ? -lx - 1 : 2 * GBA_W - 1 - lx;
            }
            else if (lx < 0)
                lx = -lx - 1;
            else if (lx >= GBA_W)
                lx = 2 * GBA_W - 1 - lx;
        }
        else if (sBattleWide)
        {
            lx = x + sOrgX;
            if (picture)
            {
                float px = BATTLE_PICTURE_FIX_X + (lx - BATTLE_PICTURE_FIX_X) / BATTLE_PICTURE_SCALE;

                if (px < 0.0f || px >= GBA_W)
                {
                    sBgLine[bg][x].prio = 4;
                    continue;
                }
                lx = (int)px;
            }
            else if (inside && (lx < 0 || lx >= GBA_W || y + sOrgY < 0 || y + sOrgY >= GBA_H))
            {
                sBgLine[bg][x].prio = 4;
                continue;
            }
        }
        if (bg == 0 && sFieldUi)
        {
            lx = x - FIELD_UI_SHIFT;
            if (lx < 0 || lx >= 256)
            {
                sBgLine[bg][x].prio = 4;
                continue;
            }
        }
        unsigned sx = ((unsigned)lx + hofs) & (mapW - 1);
        unsigned tx = sx >> 3, px = sx & 7;
        unsigned rowTy = ty;
        bool fixedEntry = false;
        if (edgeTile >= 0)
        {
            tx = (unsigned)edgeTile;
            px = edgePx;
            edgeTile = -1;
            if (sStageFill.tile)
                rowTy = sStageFill.tileRow;
            fixedEntry = sStageFill.entry;
        }
        unsigned block = (tx >> 5) + (rowTy >> 5) * (mapW >> 8);
        unsigned entryAt = screenBase + block * 0x800 + (((rowTy & 31) << 5) + (tx & 31)) * 2;
        Pixel *out = &sBgLine[bg][x];

        out->prio = 4;
        if (!fixedEntry && entryAt + 1 >= 0x10000)
            continue;
        uint16_t e = fixedEntry ? sStageFill.tileEntry : (uint16_t)(vram[entryAt] | (vram[entryAt + 1] << 8));
        unsigned tile = e & 0x3FF;
        unsigned fx = (e & 0x400) ? 7 - px : px, fy = (e & 0x800) ? 7 - py : py;
        unsigned idx;

        if (bpp8)
        {
            unsigned at = charBase + tile * 64 + fy * 8 + fx;
            if (at >= 0x10000)
                continue;
            idx = vram[at];
            if (!idx)
                continue;
            out->color = Pal(idx);
        }
        else
        {
            unsigned at = charBase + tile * 32 + fy * 4 + (fx >> 1);
            if (at >= 0x10000)
                continue;
            idx = (vram[at] >> ((fx & 1) * 4)) & 15;
            if (!idx)
                continue;
            /* The roulette's table colour round its wheel and grid: the
             * felt under it shows instead (3ds_video.c, sKeyedEntry). */
            if (sCornerShown == CTR_CENTRED_ROULETTE && sStageMenu && (e >> 12) * 16 + idx == 5 * 16 + 1)
                continue;
            out->color = Pal((e >> 12) * 16 + idx);
        }
        out->prio = (uint8_t)prio;
        out->flags = 0;
    }
}

static void AffineLine(int bg, int width)
{
    uint16_t cnt = Reg(0x08 + 2 * bg);
    unsigned prio = cnt & 3;
    unsigned charBase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenBase = ((cnt >> 8) & 31) * 0x800;
    bool wrap = (cnt >> 13) & 1;
    int size = 128 << ((cnt >> 14) & 3);
    unsigned base = bg == 2 ? 0x20 : 0x30;
    int16_t pa = (int16_t)LReg(base), pc = (int16_t)LReg(base + 4);
    int32_t x = sAffX[bg - 2], y = sAffY[bg - 2];
    const uint8_t *vram = sMem.vram;

    if (sWide)
    {
        x += pa * sOrgX;
        y += pc * sOrgX;
    }
    for (int i = 0; i < width; i++, x += pa, y += pc)
    {
        bool margin = sWide && (i + sOrgX < 0 || i + sOrgX >= GBA_W || (int)sLine >= GBA_H);
        if (margin && !(sStageWide && sStageAffineWrap && (int)sLine < GBA_H))
        {
            sBgLine[bg][i].prio = 4;
            continue;
        }
        int tx = x >> 8, ty = y >> 8;
        Pixel *out = &sBgLine[bg][i];

        out->prio = 4;
        if (wrap || margin)
        {
            tx &= size - 1;
            ty &= size - 1;
        }
        else if (tx < 0 || ty < 0 || tx >= size || ty >= size)
            continue;
        unsigned mapAt = screenBase + (unsigned)((ty >> 3) * (size >> 3) + (tx >> 3));
        if (mapAt >= 0x10000)
            continue;
        unsigned at = charBase + vram[mapAt] * 64u + (unsigned)((ty & 7) * 8 + (tx & 7));
        if (at >= 0x10000)
            continue;
        unsigned idx = vram[at];
        if (!idx)
            continue;
        out->color = Pal(idx);
        out->prio = (uint8_t)prio;
        out->flags = 0;
    }
}

static uint32_t sOamAnchored[4];
static int16_t sOamAnchor[128];

/* The game's tags on OAM entries (3ds_video.h), as 3ds_video.c keeps them. */
static uint32_t sVoxelWeatherOam[4], sVoxelCloudOam[4];
static uint32_t sScreenOam[4], sFogOam[4], sLatticeOam[4], sParticleOam[4];

static inline bool Tagged(const uint32_t *marks, unsigned i) { return (marks[i >> 5] >> (i & 31)) & 1; }

/* The battle's info boxes drawn from their pictures (3ds_battle_hud.h):
 * their sprites' tiles are left out of the battle's picture. */
static const CtrHudBox *sHud;
static int8_t sHudOamBox[128];
/* Where each box's picture lies over the scene, in GBA pixels, and its
 * sprites' priority (HudFrame): the scene's render marks sHudCoveredNow when
 * a background drawn in front of a box, or a window hiding the sprites, falls
 * in one (3ds_video.c HudCoverUpdate, HudWindowsShow); the next frame then
 * shows the game's own box, as the 3DS does. */
static int sHudRect[CTR_HUD_BOXES][5];
static unsigned sHudRects;
static bool sHudCovered, sHudCoveredNow;

/* The overlay's class of OAM entry `i`, or -1 when the world draws it or it
 * is a card in the scene (3ds_video.c DrawObjects, sVoxelObjPass). */
static int OverlayClass(unsigned i)
{
    bool weather = Tagged(sVoxelWeatherOam, i), screen = Tagged(sScreenOam, i);

    if (Tagged(sFogOam, i) && CtrVoxel_DrawsFog())
        return -1;
    if ((Tagged(sLatticeOam, i) || Tagged(sParticleOam, i)) && CtrVoxel_DrawsWeather())
        return -1;
    if (Tagged(sVoxelCloudOam, i))
        return -1;
    if (weather)
        return OBJ_WEATHER;
    if (screen)
        return OBJ_SCREEN;
    return sOverlayFieldObjs ? OBJ_FIELD : -1;
}

static int OamUnwrapY(unsigned index, int y)
{
    int anchor = sOamAnchor[index];

    while (y - anchor > 128) y -= 256;
    while (anchor - y > 128) y += 256;
    return y;
}

static const uint8_t sObjW[4][4] = { {8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0} };
static const uint8_t sObjH[4][4] = { {8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0} };

static const uint16_t *ObjExtPalette(unsigned n);

static void ObjLine(int line, int width, int height)
{
    const uint16_t *oam = sMem.oam;
    const uint8_t *objVram = sMem.vram + 0x10000;
    bool map1d = (Reg(0) >> 6) & 1;

    for (int i = 0; i < width; i++)
    {
        sObjLine[i].prio = 4;
        sObjWin[i] = 0;
    }
    if (sOverlay)
        for (int c = 0; c < OBJ_CLASSES; c++)
            for (int i = 0; i < width; i++)
                sObjClassLine[c][i].prio = 4;
    for (int n = 0; n < 128; n++)
    {
        uint16_t a0 = oam[n * 4], a1 = oam[n * 4 + 1], a2 = oam[n * 4 + 2];
        Pixel *objLine = sObjLine;
        if (sHud && sHudOamBox[n] >= 0)
            continue;
        if (sStageWide && sObjParked[n])
            continue;
        if (sOverlay)
        {
            int cls = OverlayClass((unsigned)n);
            if (cls < 0)
                continue;
            objLine = sObjClassLine[cls];
        }
        bool affine = (a0 >> 8) & 1;
        if (!affine && ((a0 >> 9) & 1))
            continue;                         /* disabled */
        unsigned shape = a0 >> 14, sz = a1 >> 14, mode = (a0 >> 10) & 3;
        if (shape == 3 || mode == 3)
            continue;
        int w = sObjW[shape][sz], h = sObjH[shape][sz];
        bool dbl = affine && ((a0 >> 9) & 1);
        int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int y = a0 & 255, x = a1 & 511;

        if (height == GBA_H || sWide)
        {
            /* The GBA's own reading: back from the other side only past the
             * end of the coordinate range. */
            if (y + bh > 256)
                y -= 256;
            /* A wide view reaches left of the picture: x past its right
             * edge is one leaving or entering on the left (9-bit). */
            if (sWide ? x - sOrgX >= width : x + bw > 512)
                x -= 512;
            if (sStageWide)
                x += sObjDrift[n];
        }
        else
        {
            /* The wide field: the 8-bit y is ambiguous on a 240-line view (a
             * sprite leaving the top at -20 reads 236), so it is placed by
             * its sprite's true y (sprite.c's anchors), as 3ds_video.c does. */
            if (x >= width)
                x -= 512;
            if (sOamAnchored[n >> 5] & (1u << (n & 31)))
                y = OamUnwrapY((unsigned)n, y);
            else if (y >= height)
                y -= 256;
        }
        int ly = line + (sWide ? sOrgY : 0) - y;
        if (ly < 0 || ly >= bh)
            continue;
        bool bpp8 = (a0 >> 13) & 1;
        unsigned tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, pal = a2 >> 12;
        const uint16_t *ext = bpp8 ? NULL : ObjExtPalette((unsigned)n);
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine)
        {
            unsigned g = (a1 >> 9) & 31;
            pa = (int16_t)oam[g * 16 + 3];
            pb = (int16_t)oam[g * 16 + 7];
            pc = (int16_t)oam[g * 16 + 11];
            pd = (int16_t)oam[g * 16 + 15];
        }
        for (int lx = 0; lx < bw; lx++)
        {
            int sx = x + lx - (sWide ? sOrgX : 0);
            if (sx < 0 || sx >= width)
                continue;
            int tx, ty;
            if (affine)
            {
                int cx = lx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
            }
            else
            {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            unsigned t;
            if (bpp8)
            {
                unsigned rowTiles = map1d ? (unsigned)w / 8 * 2 : 32;
                t = tile + (unsigned)(ty / 8) * rowTiles + (unsigned)(tx / 8) * 2;
                unsigned at = (t & 0x3FF) * 32 + (unsigned)((ty & 7) * 8 + (tx & 7));
                if (at >= 0x8000)
                    continue;
                unsigned idx = objVram[at];
                if (!idx)
                    continue;
                if (mode == 2) { sObjWin[sx] = 1; continue; }
                if (prio < objLine[sx].prio)
                {
                    objLine[sx].color = Pal(256 + idx);
                    objLine[sx].prio = (uint8_t)prio;
                    objLine[sx].flags = mode == 1;
                }
            }
            else
            {
                unsigned rowTiles = map1d ? (unsigned)w / 8 : 32;
                t = tile + (unsigned)(ty / 8) * rowTiles + (unsigned)(tx / 8);
                unsigned at = (t & 0x3FF) * 32 + (unsigned)((ty & 7) * 4 + ((tx & 7) >> 1));
                if (at >= 0x8000)
                    continue;
                unsigned idx = (objVram[at] >> ((tx & 1) * 4)) & 15;
                if (!idx)
                    continue;
                if (mode == 2) { sObjWin[sx] = 1; continue; }
                if (prio < objLine[sx].prio)
                {
                    objLine[sx].color = ext ? ext[idx] & 0x7FFF : Pal(256 + pal * 16 + idx);
                    objLine[sx].prio = (uint8_t)prio;
                    objLine[sx].flags = mode == 1;
                }
            }
        }
    }
}

/* A window edge is 8 bits: 255 as the far edge means the screen's edge on a
 * wider screen (3ds_video.c WindowEdge). */
static void WindowRange(uint16_t limits, int extent, int *first, int *last)
{
    int a = limits >> 8, b = limits & 255;
    *first = a < extent ? a : extent;
    *last = (b == 255 && a < 255) ? extent : (b < extent ? b : extent);
}

static bool InWindow(uint16_t h, uint16_t v, int x, int y, int width, int height)
{
    int x0, x1, y0, y1;
    if (sWide)
    {
        /* In GBA coordinates; an edge on the picture's goes on to the view's. */
        x += sOrgX;
        y += sOrgY;
        WindowRange(v, GBA_H, &y0, &y1);
        if (y0 <= y1 && !(y >= y0 && y < y1) && !((y0 == 0 && y < 0) || (y1 >= GBA_H && y >= GBA_H)))
            return false;
        if (y0 > y1 && !(y >= y0 || y < y1))
            return false;
        WindowRange(h, GBA_W, &x0, &x1);
        if (x0 <= x1)
            return (x >= x0 && x < x1) || (x0 == 0 && x < 0) || (x1 >= GBA_W && x >= GBA_W);
        return x >= x0 || x < x1;
    }
    WindowRange(v, height, &y0, &y1);
    bool inY = y0 <= y1 ? (y >= y0 && y < y1) : (y >= y0 || y < y1);
    if (!inY)
        return false;
    WindowRange(h, width, &x0, &x1);
    return x0 <= x1 ? (x >= x0 && x < x1) : (x >= x0 || x < x1);
}

static inline uint16_t BlendAlpha(uint16_t a, uint16_t b, unsigned eva, unsigned evb)
{
    unsigned r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    unsigned g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    unsigned bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

static inline uint16_t Brighten(uint16_t c, unsigned evy)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static inline uint16_t Darken(uint16_t c, unsigned evy)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* `src` (BGR555) at alpha a (0..255) over the premultiplied `dst`. */
static inline uint32_t Over(uint32_t dst, uint16_t src, unsigned a)
{
    uint32_t c = ToRGBA(src), out = 0;

    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        unsigned s = shift == 24 ? 255u : (c >> shift) & 255u, d = (dst >> shift) & 255u;
        out |= ((s * a + d * (255u - a) + 127u) / 255u) << shift;
    }
    return out;
}

/* One pixel of the voxel overlay: from the bottom, the field's sprites (in
 * the swoop), the weather, the text layer, the sprites on the screen. */
static uint32_t OverlayPixel(int x, unsigned enable, bool bg0, bool objOn, uint16_t bldcnt, unsigned blendMode,
                             unsigned eva, unsigned evy)
{
    uint32_t out = 0;
    bool effects = (enable & 32) != 0;

    for (int layer = 0; layer < 4; layer++)
    {
        const Pixel *p;
        bool obj = layer != 2, target;
        unsigned a = 255;

        if (obj)
        {
            if (!objOn || !(enable & 16))
                continue;
            p = &sObjClassLine[layer == 0 ? OBJ_FIELD : layer == 1 ? OBJ_WEATHER : OBJ_SCREEN][x];
            target = (bldcnt >> 4) & 1;
        }
        else
        {
            if (!bg0 || !(enable & 1))
                continue;
            p = &sBgLine[0][x];
            target = bldcnt & 1;
        }
        if (p->prio == 4)
            continue;
        uint16_t c = p->color;
        if (obj && ((p->flags & 1) || (effects && target && blendMode == 1)))
            a = eva * 255u / 16u;
        else if (effects && target && blendMode == 2)
            c = Brighten(c, evy);
        else if (effects && target && blendMode == 3)
            c = Darken(c, evy);
        out = Over(out, c, a);
    }
    return out;
}

/* A scene pixel at GBA (gx, gy), column x of this line: whether it hides a
 * box's picture - the windows keep the sprites out of it, or a background
 * in front of the box drew there. */
static void HudCoverPixel(int gx, int gy, int x, unsigned enable, const bool *bgOn)
{
    for (unsigned r = 0; r < sHudRects; ++r)
    {
        const int *rect = sHudRect[r];

        if (gx < rect[0] || gx >= rect[2] || gy < rect[1] || gy >= rect[3])
            continue;
        if (!(enable & 16))
        {
            sHudCoveredNow = true;
            return;
        }
        for (int bg = 0; bg < 4; bg++)
            if (bgOn[bg] && ((enable >> bg) & 1) && sBgLine[bg][x].prio < (unsigned)rect[4])
            {
                sHudCoveredNow = true;
                return;
            }
    }
}

/* Renders the GBA picture of width x height into sOut at (ox, oy). */
static void RenderPicture(int width, int height, int ox, int oy)
{
    uint16_t dispcnt = Reg(0);
    unsigned mode = dispcnt & 7;
    bool forcedBlank = (dispcnt >> 7) & 1;

    for (int i = 0; i < 2; i++)
    {
        uint32_t rx = (uint32_t)Reg(0x28 + 16 * i) | ((uint32_t)Reg(0x2A + 16 * i) << 16);
        uint32_t ry = (uint32_t)Reg(0x2C + 16 * i) | ((uint32_t)Reg(0x2E + 16 * i) << 16);
        sAffX[i] = CtrVideo_AffineReference(rx);
        sAffY[i] = CtrVideo_AffineReference(ry);
    }
    for (int y = 0; y < height; y++)
    {
        uint32_t *row = sOut + (size_t)(oy + y) * TOP_W + ox;
        int gy = y + (sWide ? sOrgY : 0);

        /* Lines outside the picture take the latched registers. */
        sLine = gy >= 0 ? (unsigned)gy : 0xFFFFu;
        if (forcedBlank)
        {
            for (int x = 0; x < width; x++)
                row[x] = 0xFFFFFFFFu;
            continue;
        }
        bool bgOn[4] = { false, false, false, false };
        for (int bg = 0; bg < 4; bg++)
        {
            if (!((dispcnt >> (8 + bg)) & 1))
                continue;
            if ((sOverlay && bg != 0) || ((sExcludeLayers >> bg) & 1))
                continue;
            if (mode == 0 || (mode == 1 && bg < 2))
            {
                TextLine(bg, y, width);
                bgOn[bg] = true;
            }
            else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2))
            {
                AffineLine(bg, width);
                bgOn[bg] = true;
            }
        }
        bool objOn = (dispcnt >> 12) & 1;
        if (objOn)
            ObjLine(y, width, height);

        bool win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winObj = (dispcnt >> 15) & 1;
        bool anyWin = win0 || win1 || winObj;
        uint16_t winIn = Reg(0x48), winOut = Reg(0x4A);
        uint16_t bldcnt = Reg(0x50), bldalpha = Reg(0x52);
        unsigned blendMode = (bldcnt >> 6) & 3;
        unsigned eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = Reg(0x54) & 31;
        if (eva > 16) eva = 16;
        if (evb > 16) evb = 16;
        if (evy > 16) evy = 16;
        uint16_t backdrop = Pal(0);

        for (int x = 0; x < width; x++)
        {
            unsigned enable = 0x3F;
            if (anyWin)
            {
                if (win0 && InWindow(LReg(0x40), LReg(0x44), x, y, width, height))
                    enable = winIn & 0x3F;
                else if (win1 && InWindow(LReg(0x42), LReg(0x46), x, y, width, height))
                    enable = (winIn >> 8) & 0x3F;
                else if (winObj && objOn && sObjWin[x])
                    enable = (winOut >> 8) & 0x3F;
                else
                    enable = winOut & 0x3F;
            }
            if (sOverlay)
            {
                row[x] = OverlayPixel(x, enable, bgOn[0], objOn, bldcnt, blendMode, eva, evy);
                continue;
            }
            if (sHudRects && sBattleScene && !sHudCoveredNow)
                HudCoverPixel(x + (sWide ? sOrgX : 0), gy, x, enable, bgOn);
            /* The two front-most layers: 0-3 BGs, 4 OBJ, 5 backdrop. */
            int first = 5, second = 5;
            uint16_t back = sCornerShown && sStageWide ? CornerBackdrop(x, y, backdrop) : backdrop;
            uint16_t c1 = back, c2 = back;
            unsigned p1 = 5, p2 = 5;
            for (unsigned prio = 0; prio < 4 && second == 5; prio++)
            {
                if ((enable & 16) && objOn && sObjLine[x].prio == prio)
                {
                    if (first == 5) { first = 4; c1 = sObjLine[x].color; p1 = prio; }
                    else if (second == 5) { second = 4; c2 = sObjLine[x].color; p2 = prio; }
                }
                for (int bg = 0; bg < 4 && second == 5; bg++)
                {
                    if (!bgOn[bg] || !((enable >> bg) & 1) || sBgLine[bg][x].prio != prio)
                        continue;
                    if (first == 5) { first = bg; c1 = sBgLine[bg][x].color; p1 = prio; }
                    else { second = bg; c2 = sBgLine[bg][x].color; p2 = prio; }
                }
            }
            (void)p1; (void)p2;
            uint16_t out = c1;
            bool semi = first == 4 && (sObjLine[x].flags & 1);
            if (semi && ((bldcnt >> (8 + second)) & 1))
                out = BlendAlpha(c1, c2, eva, evb);
            else if ((enable & 32) && ((bldcnt >> first) & 1))
            {
                if (blendMode == 1 && ((bldcnt >> (8 + second)) & 1))
                    out = BlendAlpha(c1, c2, eva, evb);
                else if (blendMode == 2)
                    out = Brighten(c1, evy);
                else if (blendMode == 3)
                    out = Darken(c1, evy);
            }
            if (sWorldBehind && first == 5 && (!anyWin || (enable & 8)))
                row[x] = 0; /* the world, where BG3 was */
            else
                row[x] = ToRGBA(out);
        }
        for (int i = 0; i < 2 && gy >= 0; i++)
        {
            sAffX[i] += (int16_t)LReg(0x22 + 16 * i);
            sAffY[i] += (int16_t)LReg(0x26 + 16 * i);
        }
    }
}

/*
 * How much of a window pixel is the picture: its corners are cut as the
 * frame round it cuts its opening (3ds_bottom_shell.inc, DrawChamferFrame:
 * the 240x160 hole, a 3-pixel cut, ChamferDist and Cover in
 * 3ds_bottom_plate.inc), the rest of a corner pixel the frame's rim.
 */
static float WindowCover(int x, int y)
{
    const float hw = GBA_W * 0.5f, hh = GBA_H * 0.5f, cut = 3.0f;
    float du = fabsf(x + 0.5f - hw), dv = fabsf(y + 0.5f - hh);
    float a = du - hw, b = dv - hh, k = (du + dv - (hw + hh - cut)) * 0.70710678f;
    float d = (b > a ? b : a);

    d = k > d ? k : d;
    d = 0.5f - d;
    return d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
}

/* The GBA picture into the bottom screen's window. */
static void RenderBottomWindow(void)
{
    /* The frame's rim, light at its top and darker at its foot (its 164 rows). */
    static const float rimTop[3] = {252, 245, 234}, rimLow[3] = {236, 224, 208};

    sOut = sBottomPicture;
    RenderPicture(GBA_W, GBA_H, 0, 0);
    sOut = sTop;
    for (int x = 0; x < GBA_W; x++)
    {
        uint16_t *column = sBottom + (size_t)(CTR_BOTTOM_WIN_X + x) * CTR_BOTTOM_HEIGHT;
        bool edgeX = x < 4 || x >= GBA_W - 4;
        for (int y = 0; y < GBA_H; y++)
        {
            uint32_t c = sBottomPicture[(size_t)y * TOP_W + x];
            unsigned r = (c & 0xFF) >> 3, g = ((c >> 8) & 0xFF) >> 2, b = ((c >> 16) & 0xFF) >> 3;

            if (edgeX && (y < 4 || y >= GBA_H - 4))
            {
                float t = WindowCover(x, y), along = (y + 2.5f) / (GBA_H + 4), rim[3];

                for (int i = 0; i < 3; i++)
                    rim[i] = rimTop[i] + (rimLow[i] - rimTop[i]) * along;
                r = (unsigned)(((c & 0xFF) * t + rim[0] * (1 - t)) + 0.5f) >> 3;
                g = (unsigned)((((c >> 8) & 0xFF) * t + rim[1] * (1 - t)) + 0.5f) >> 2;
                b = (unsigned)((((c >> 16) & 0xFF) * t + rim[2] * (1 - t)) + 0.5f) >> 3;
            }

            column[CTR_BOTTOM_HEIGHT - 1 - (CTR_BOTTOM_WIN_SCREEN_Y + y)] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    sBottomDirty = true;
}

static void RenderTop(void)
{
    bool gba = sStage || sBattle || sTransition || sCentred != CTR_CENTRED_NONE;

    if (gba)
    {
        for (size_t i = 0; i < TOP_W * TOP_H; i++)
            sTop[i] = 0xFF000000u;
        RenderPicture(GBA_W, GBA_H, CTR_STAGE_X, CTR_STAGE_Y);
    }
    else
    {
        sLineRegCount = 0;
        sFieldUi = CtrGame_IsOverworld() && !sFieldBanner;
        RenderPicture(TOP_W, TOP_H, 0, 0);
        sFieldUi = false;
    }
}

/* ── Hints (3ds_video.h) ──────────────────────────────────────────────── */
void CtrVideo_Bind(CtrVideoMemory memory) { sMem = memory; }
void CtrVideo_HoldTop(bool hold) { sHoldTop = hold; }
bool CtrVideo_TopHeld(void) { return sHoldTop; }
void CtrVideo_SetStage(bool stage) { sStage = stage; }
void CtrVideo_SetStageFaded(bool faded) { (void)faded; }
void CtrVideo_SetStageZoomed(bool zoomed) { sStageZoomed = zoomed; }
bool CtrVideo_StageZoomed(void) { return sStageZoomed; }
void CtrVideo_SetFieldBanner(bool banner) { sFieldBanner = banner; }
void CtrVideo_SetCentred(unsigned screen) { sCentred = screen; }
void CtrVideo_ShowNoHome(void) {}
void CtrVideo_SetBattle(bool battle) { sBattle = battle; }
bool CtrVideo_InBattle(void) { return sBattle; }
void CtrVideo_SetTransition(bool transition) { sTransition = transition; }
void CtrVideo_SetLineRegisters(const uint16_t *regs, unsigned lines)
{
    if (!regs || lines == 0)
    {
        sLineRegCount = 0;
        return;
    }
    if (lines > GBA_H)
        lines = GBA_H;
    memcpy(sLineRegs, regs, (size_t)lines * CTR_LINE_REGS * sizeof(uint16_t));
    sLineRegCount = lines;
}
void CtrVideo_SetLineScroll(unsigned reg, bool wide, const void *values, unsigned lines)
{
    (void)reg; (void)wide; (void)values; (void)lines;
}
void CtrVideo_SetLineWindow(const uint16_t *values, unsigned lines, bool both)
{
    (void)values; (void)lines; (void)both;
}
/* The circle of light of a dark cave (3ds_video.c sFieldLight): drawn by the
 * GBA's windows on the 2D field, softly over the voxel world (VoxelFlashLight). */
static struct
{
    bool on;
    int x, y, radius;
    float shown, burst;
    uint32_t ticks;
} sFieldLight;

void CtrVideo_SetFieldLight(bool on, int x, int y, int radius)
{
    if (on && (!sFieldLight.on || sFieldLight.radius < 0))
        sFieldLight.shown = (float)radius;
    else if (on && radius > sFieldLight.radius && sFieldLight.shown < radius - 6.0f)
        sFieldLight.burst = 1.0f;
    sFieldLight.on = on;
    if (!on) { sFieldLight.burst = 0.0f; return; }
    sFieldLight.x = x;
    sFieldLight.y = y;
    sFieldLight.radius = radius;
    ++sFieldLight.ticks;
    sFieldLight.shown += ((float)radius - sFieldLight.shown) * 0.25f;
    if (radius > sFieldLight.shown - 0.5f && radius < sFieldLight.shown + 0.5f) sFieldLight.shown = (float)radius;
    sFieldLight.burst *= 0.955f;
}
void CtrVideo_ClearOamAnchors(void) { memset(sOamAnchored, 0, sizeof(sOamAnchored)); }
void CtrVideo_SetOamAnchor(unsigned first, unsigned end, int y)
{
    if (end > 128)
        end = 128;
    for (unsigned i = first; i < end; ++i)
    {
        sOamAnchored[i >> 5] |= 1u << (i & 31);
        sOamAnchor[i] = (int16_t)y;
    }
}
static void Mark(uint32_t *marks, unsigned first, unsigned end)
{
    if (end > 128) end = 128;
    for (unsigned i = first; i < end; ++i)
        marks[i >> 5] |= 1u << (i & 31);
}
void CtrVideo_ClearVoxelWeatherOam(void)
{
    memset(sVoxelWeatherOam, 0, sizeof(sVoxelWeatherOam));
    memset(sVoxelCloudOam, 0, sizeof(sVoxelCloudOam));
}
void CtrVideo_MarkVoxelWeatherOam(unsigned first, unsigned end) { Mark(sVoxelWeatherOam, first, end); }
void CtrVideo_MarkVoxelCloudOam(unsigned first, unsigned end) { Mark(sVoxelCloudOam, first, end); }
void CtrVideo_ClearFogOam(void)
{
    memset(sFogOam, 0, sizeof(sFogOam));
    memset(sLatticeOam, 0, sizeof(sLatticeOam));
    memset(sParticleOam, 0, sizeof(sParticleOam));
}
void CtrVideo_MarkFogOam(unsigned first, unsigned end) { Mark(sFogOam, first, end); }
void CtrVideo_MarkWeatherOam(unsigned first, unsigned end, bool lattice)
{
    Mark(lattice ? sLatticeOam : sParticleOam, first, end);
}
/*
 * The ext palettes (3ds_video.c CtrVideo_LatchExtPalettes): their faded
 * colours as the game last handed them over, which are in use (a bitmask),
 * and the one each OAM entry is drawn with, plus one (0: attr2's own). The
 * follower's and the visible wild Pokemon's HGSS sprites use them: the 2D
 * field's sprites (ObjLine) as the voxel world's cards do.
 */
static uint16_t sExtPalettes[CTR_EXT_PALETTES][16];
static unsigned sExtInUse;
static uint8_t sOamExt[128];
static bool sOamExtMarked;

void CtrVideo_LatchExtPalettes(const uint16_t *colors, unsigned inUse)
{
    sExtInUse = inUse & ((1u << CTR_EXT_PALETTES) - 1);
    if (colors && sExtInUse)
        memcpy(sExtPalettes, colors, sizeof(sExtPalettes));
}
const uint16_t *CtrVideo_ExtPalette(unsigned index) { return sExtPalettes[index % CTR_EXT_PALETTES]; }
void CtrVideo_ClearExtPaletteOam(void)
{
    if (!sOamExtMarked)
        return;
    memset(sOamExt, 0, sizeof(sOamExt));
    sOamExtMarked = false;
}
void CtrVideo_MarkExtPaletteOam(unsigned first, unsigned end, unsigned index)
{
    if (index >= CTR_EXT_PALETTES)
        return;
    if (end > 128)
        end = 128;
    for (unsigned i = first; i < end; ++i)
        sOamExt[i] = (uint8_t)(index + 1);
    sOamExtMarked = sOamExtMarked || first < end;
}
/* OAM entry n's ext palette while it is in use (a mark outlives a scene that
 * stops building OAM, the palettes do not), or NULL: attr2's bank. */
static const uint16_t *ObjExtPalette(unsigned n)
{
    unsigned ext = sOamExt[n];

    return ext && ((sExtInUse >> (ext - 1)) & 1) ? sExtPalettes[ext - 1] : NULL;
}
/* The tileset animations' VRAM writes, for the voxel atlases' copies of
 * those tiles (3ds_video.c). */
void CtrVideo_NotifyTilesetAnimWrite(const void *dest, unsigned bytes)
{
    uintptr_t base = (uintptr_t)sMem.vram, address = (uintptr_t)dest;

    if (base == 0 || address < base || address - base >= 0x8000 || bytes == 0)
        return;
    unsigned offset = (unsigned)(address - base);
    if (bytes > 0x8000 - offset) bytes = 0x8000 - offset;
    CtrVoxel_NotifyTilesetAnimWrite(offset / 32, (offset % 32 + bytes + 31) / 32);
}

/* The voxel module's GPU copies (3ds_video.h): the 3DS rations them by its GX
 * queue; here a copy is a memcpy and needs no ration. */
bool CtrVideo_TryVoxelUpload(void) { return true; }
bool CtrVideo_TryVoxelAnimUpload(void) { return true; }
unsigned CtrVideo_VoxelUploadsLeft(void) { return 64; }
void CtrVideo_CopiesBegin(void) {}
void CtrVideo_CopiesEnd(void) {}
void CtrVideo_SetTileBank(unsigned bank, const void *tiles, unsigned bytes, const uint16_t *palettes)
{
    (void)bank; (void)tiles; (void)bytes; (void)palettes;
}
void CtrVideo_RegisterTilemapBanks(const void *buffer, const uint8_t *banks, unsigned entries)
{
    (void)buffer; (void)banks; (void)entries;
}
void CtrVideo_UnregisterTilemapBanks(const void *buffer) { (void)buffer; }
void CtrVideo_TilemapRequested(unsigned vramOffset, const void *src, unsigned bytes)
{
    (void)vramOffset; (void)src; (void)bytes;
}
void CtrVideo_DecodeCounts(uint32_t counts[8]) { memset(counts, 0, 8 * sizeof(uint32_t)); }
void CtrVideo_NotifyWrite(const void *dest, unsigned bytes) { (void)dest; (void)bytes; }
const uint8_t *CtrVideo_GetBgVram(void) { return sMem.vram; }
void CtrVideo_RequestPlaneRelease(void) {}
/*
 * A game screen in the bottom screen's window, as 3ds_video.c draws it: the
 * PokéNav, the trainer card, and the bag, the Pokédex and the party menu when
 * the bottom screen does not draw them itself (the classic menus). Its picture
 * goes 1:1 into the window at (CTR_BOTTOM_WIN_X, CTR_BOTTOM_WIN_Y), the bottom
 * screen's canvas round it (3ds_bottom_ui.c, BlitAroundWindow), and the top
 * screen keeps the picture it had. The screens the 3DS draws over the whole
 * bottom screen (the PC's boxes, the summary...) stay on the top one here.
 */

bool CtrVideo_BottomInUse(void) { return sBottomInUse; }
void CtrVideo_SetBottomNative(bool native) { sBottomNative = native; }
void CtrVideo_SetNativeMenus(bool on) { sNativeMenus = on; }

static bool BottomWindowScreen(unsigned screen)
{
    if (sBottomNative)
        return false;
    if (screen == CTR_CENTRED_POKENAV || screen == CTR_CENTRED_CARD)
        return true;
    return !sNativeMenus && (screen == CTR_CENTRED_BAG || screen == CTR_CENTRED_POKEDEX
                             || screen == CTR_CENTRED_PARTY);
}
void CtrVideo_BottomStrips(const uint16_t *canvas) { (void)canvas; }
bool CtrVideo_BottomWhole(void) { return false; }
/* A touch's row in the window's picture, -1 off it (as 3ds_video.c). */
int CtrVideo_BottomPictureY(int y)
{
    y -= CTR_BOTTOM_WIN_SCREEN_Y;
    return y >= 0 && y < GBA_H ? y : -1;
}
void CtrVideo_ClearScreenOam(void) { memset(sScreenOam, 0, sizeof(sScreenOam)); }
void CtrVideo_MarkScreenOam(unsigned first, unsigned end) { Mark(sScreenOam, first, end); }
void CtrVideo_MarkMachineOam(unsigned first, unsigned end) { (void)first; (void)end; }
void CtrVideo_MarkFlyOam(unsigned first, unsigned end) { (void)first; (void)end; }
const CtrVideoStats *CtrVideo_GetStats(void) { return &sStats; }
unsigned char CtrBattle_Sandstorm(void) { return 0; }

/* ── Bottom canvas (3ds_bottom.h's native side) ───────────────────────── */
void CtrBottom_LastBlitTicks(uint64_t *copy, uint64_t *flush)
{
    *copy = 0;
    *flush = 0;
}

void CtrBottom_Blit(const uint16_t *canvas, int x0, int x1)
{
    if (x0 >= x1)
        return;
    memcpy(sBottom + x0 * CTR_BOTTOM_HEIGHT, canvas + x0 * CTR_BOTTOM_HEIGHT,
           (size_t)(x1 - x0) * CTR_BOTTOM_HEIGHT * sizeof(uint16_t));
    sBottomDirty = true;
}

void CtrBottom_BlitRect(const uint16_t *canvas, int x0, int y0, int x1, int y1)
{
    int first = CTR_BOTTOM_HEIGHT - y1, count = y1 - y0;

    if (x0 >= x1 || count <= 0)
        return;
    for (int x = x0; x < x1; ++x)
        memcpy(sBottom + x * CTR_BOTTOM_HEIGHT + first, canvas + x * CTR_BOTTOM_HEIGHT + first,
               (size_t)count * sizeof(uint16_t));
    sBottomDirty = true;
}

void CtrBottom_BlitDim(const uint16_t *canvas, int level)
{
    for (size_t i = 0; i < CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT; i++)
    {
        uint16_t c = canvas[i];
        unsigned r = (c >> 11) * (unsigned)level / 4, g = ((c >> 5) & 63) * (unsigned)level / 4,
                 b = (c & 31) * (unsigned)level / 4;
        sBottom[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    sBottomDirty = true;
}

/* ── EGL / GL ─────────────────────────────────────────────────────────── */
static EGLDisplay sDpy = EGL_NO_DISPLAY;
static EGLConfig sCfg;
static EGLContext sCtx = EGL_NO_CONTEXT;
static EGLSurface sPbuf = EGL_NO_SURFACE;
static EGLSurface sSurf[HOST_SCREENS] = { EGL_NO_SURFACE, EGL_NO_SURFACE };
static ANativeWindow *sWin[HOST_SCREENS];
static uint32_t sWinGen[HOST_SCREENS];
static GLuint sProgram, sTopTex, sBottomTex, sVao;
static GLint sURect, sURotate, sUTex;
static struct timespec sNextFrame;

static const char *sVertex =
    "#version 300 es\n"
    "uniform vec4 uRect;\n"            /* x0 y0 x1 y1 in clip space */
    "out vec2 vUv;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vUv = vec2(c.x, 1.0 - c.y);\n"
    "  gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);\n"
    "}\n";
static const char *sFragment =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uRotate;\n"
    "in vec2 vUv;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    /* the bottom canvas: column-major, columns bottom-to-top */
    "  vec2 uv = uRotate == 1 ? vec2(1.0 - vUv.y, vUv.x) : vUv;\n"
    "  oColor = vec4(texture(uTex, uv).rgb, 1.0);\n"
    "}\n";

static GLuint Shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[512];

    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        CtrLog_Write(CTR_LOG_ERROR, "shader: %s", log);
    }
    return s;
}

static GLuint Texture(int w, int h, GLenum internal, GLenum format, GLenum type)
{
    GLuint t;

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static bool sVoxelReady;
/* The last picture shown on the top screen came from the composite. */
static bool sTopComposed;
static void VoxelGlInit(void);

bool CtrVideo_Init(void)
{
    const EGLint attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint pbAttr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLint n = 0;

    sDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(sDpy, NULL, NULL) || !eglChooseConfig(sDpy, attr, &sCfg, 1, &n) || n < 1)
        return false;
    sCtx = eglCreateContext(sDpy, sCfg, EGL_NO_CONTEXT, ctxAttr);
    sPbuf = eglCreatePbufferSurface(sDpy, sCfg, pbAttr);
    if (sCtx == EGL_NO_CONTEXT || !eglMakeCurrent(sDpy, sPbuf, sPbuf, sCtx))
        return false;
    CtrLog_Write(CTR_LOG_VIDEO, "GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    sProgram = glCreateProgram();
    glAttachShader(sProgram, Shader(GL_VERTEX_SHADER, sVertex));
    glAttachShader(sProgram, Shader(GL_FRAGMENT_SHADER, sFragment));
    glLinkProgram(sProgram);
    sURect = glGetUniformLocation(sProgram, "uRect");
    sURotate = glGetUniformLocation(sProgram, "uRotate");
    sUTex = glGetUniformLocation(sProgram, "uTex");
    glGenVertexArrays(1, &sVao);
    sTopTex = Texture(TOP_W, TOP_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    sBottomTex = Texture(CTR_BOTTOM_HEIGHT, CTR_BOTTOM_WIDTH, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
    VoxelGlInit();
    clock_gettime(CLOCK_MONOTONIC, &sNextFrame);
    return true;
}

void CtrVideo_Shutdown(void)
{
    if (sDpy == EGL_NO_DISPLAY)
        return;
    if (sVoxelReady)
        CtrVoxel_Shutdown();
    sVoxelReady = false;
    eglMakeCurrent(sDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (int i = 0; i < HOST_SCREENS; i++)
        if (sSurf[i] != EGL_NO_SURFACE)
            eglDestroySurface(sDpy, sSurf[i]);
    eglDestroyContext(sDpy, sCtx);
    eglTerminate(sDpy);
    sDpy = EGL_NO_DISPLAY;
}

/* A new window for a screen (or none): a new surface. */
static void SyncWindows(void)
{
    for (int i = 0; i < HOST_SCREENS; i++)
    {
        uint32_t gen;
        ANativeWindow *win = AndroidHost_AcquireWindow(i, &gen);

        if (gen == sWinGen[i])
        {
            AndroidHost_ReleaseWindow(win);
            continue;
        }
        if (sSurf[i] != EGL_NO_SURFACE)
        {
            eglMakeCurrent(sDpy, sPbuf, sPbuf, sCtx);
            eglDestroySurface(sDpy, sSurf[i]);
            sSurf[i] = EGL_NO_SURFACE;
        }
        AndroidHost_ReleaseWindow(sWin[i]);
        sWin[i] = win;
        sWinGen[i] = gen;
        if (win)
        {
            sSurf[i] = eglCreateWindowSurface(sDpy, sCfg, win, NULL);
            /* The top display runs at 120 Hz: ask for 60, the game's rate. */
            ANativeWindow_setFrameRate(win, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            if (sSurf[i] != EGL_NO_SURFACE)
            {
                eglMakeCurrent(sDpy, sSurf[i], sSurf[i], sCtx);
                eglSwapInterval(sDpy, 0);
            }
            CtrLog_Write(CTR_LOG_VIDEO, "screen %d: %dx%d window", i,
                         ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
        }
        sBottomDirty = true;
        AndroidHost_WindowDone(i, gen);
    }
}

/* The smallest rectangle of the picture's aspect covering the view, centred:
 * the picture is made a little wider than the display, never letterboxed. */
static void Cover(int w, int h, int pw, int ph, int *x, int *y, int *vw, int *vh)
{
    if ((long)w * ph >= (long)h * pw)
    {
        *vw = w;
        *vh = (int)(((long)w * ph + pw - 1) / pw);
    }
    else
    {
        *vh = h;
        *vw = (int)(((long)h * pw + ph - 1) / ph);
    }
    *x = (w - *vw) / 2;
    *y = (h - *vh) / 2;
}

/* Both pictures are made in the displays' own shapes (432x240 on the 16:9
 * top, 320x280 on the bottom): they cover them, a few pixels cut at most. */
void AndroidVideo_BottomViewport(int w, int h, int *x, int *y, int *vw, int *vh)
{
    Cover(w, h, CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, x, y, vw, vh);
}

#ifndef CTR_SHOW_FPS
#define CTR_SHOW_FPS 1
#endif
#if CTR_SHOW_FPS
/*
 * The FPS counter (SHOW_FPS=1), shown when the options turn it on
 * (CtrSettings_ShowFps), as 3ds_video.c draws it: "FPS shown/game" in a 3x5
 * pixel font over a translucent box, in the top screen's top left corner -
 * the pictures a second, and the game frames a second (CtrPlatform_FrameCount),
 * which differ on a slow frame. Measured over a second, the log's over five.
 */
static const char *const sFpsGlyphs[] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111",
    "101101111001001", "111100111001111", "111100111101111", "111001001001001",
    "111101111101111", "111101111001111",
    "111100111100100", /* F */
    "111101111100100", /* P */
    "001001010100100", /* / */
};
static float sOverlayFps, sOverlayGameFps;
/* Defined with the quads, further down. */
static int sQuadW, sQuadH;
static void BlendOver(void);
static void Rect(float x0, float y0, float x1, float y1, uint32_t abgr);

static void UpdateOverlayFps(void)
{
    static uint64_t start, startGame;
    static uint32_t frames;
    uint64_t now = CtrPlatform_Milliseconds(), game = CtrPlatform_FrameCount();

    if (start == 0 || now < start)
    {
        start = now;
        startGame = game;
        frames = 0;
        return;
    }
    frames++;
    if (now - start >= 1000)
    {
        sOverlayFps = frames * 1000.0f / (float)(now - start);
        sOverlayGameFps = (float)(game - startGame) * 1000.0f / (float)(now - start);
        start = now;
        startGame = game;
        frames = 0;
    }
}

/* On the window (w x h): its top left at (x, y) from the window's top left,
 * one font pixel `px` window pixels. Quad's y counts from the bottom there. */
static void DrawFps(int w, int h, float x, float y, float px)
{
    unsigned fps = (unsigned)(sOverlayFps + 0.5f), game = (unsigned)(sOverlayGameFps + 0.5f);
    unsigned glyphs[12], count = 0;
    char digits[8];
    int length = snprintf(digits, sizeof(digits), "%u/%u", fps > 999 ? 999 : fps, game > 999 ? 999 : game);

    glyphs[count++] = 10;  /* F */
    glyphs[count++] = 11;  /* P */
    glyphs[count++] = 5;   /* S, which is a 5 */
    glyphs[count++] = ~0u; /* space */
    for (int i = 0; i < length && count < 12; ++i)
        glyphs[count++] = digits[i] == '/' ? 12u : (unsigned)(digits[i] - '0');

    /* Quads count in this window's pixels while it draws; the frame's own
     * targets set their size once and draw on (QuadTarget), so it goes back. */
    int savedW = sQuadW, savedH = sQuadH;
    sQuadW = w;
    sQuadH = h;
    BlendOver();
#define FPS_RECT(x0, y0, x1, y1, abgr) Rect((x0), h - (y1), (x1), h - (y0), (abgr))
    FPS_RECT(x + 2 * px, y + 2 * px, x + (2 + count * 4 + 3) * px, y + (2 + 5 + 4) * px, 0xA0000000u);
    for (unsigned i = 0; i < count; ++i)
    {
        if (glyphs[i] == ~0u)
            continue;
        const char *g = sFpsGlyphs[glyphs[i]];
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if (g[row * 3 + col] == '1')
                {
                    float gx = x + (4 + i * 4 + col) * px, gy = y + (4 + row) * px;
                    FPS_RECT(gx, gy, gx + px, gy + px, 0xFFFFFFFFu);
                }
    }
#undef FPS_RECT
    glDisable(GL_BLEND);
    sQuadW = savedW;
    sQuadH = savedH;
}
#endif

static void DrawScreen(int screen, GLuint tex, int pw, int ph, int rotate)
{
    EGLint w = 0, h = 0;
    int x, y, vw, vh;

    if (sSurf[screen] == EGL_NO_SURFACE)
        return;
    eglMakeCurrent(sDpy, sSurf[screen], sSurf[screen], sCtx);
    eglQuerySurface(sDpy, sSurf[screen], EGL_WIDTH, &w);
    eglQuerySurface(sDpy, sSurf[screen], EGL_HEIGHT, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    Cover(w, h, pw, ph, &x, &y, &vw, &vh);
    glUseProgram(sProgram);
    glBindVertexArray(sVao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(sUTex, 0);
    glUniform1i(sURotate, rotate);
    /* pixel rect -> clip space, y up */
    glUniform4f(sURect, 2.0f * x / w - 1.0f, 1.0f - 2.0f * (y + vh) / h,
                2.0f * (x + vw) / w - 1.0f, 1.0f - 2.0f * y / h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#if CTR_SHOW_FPS
    if (screen == HOST_SCREEN_TOP && CtrSettings_ShowFps())
        DrawFps(w, h, (float)x, (float)y, (float)vh / (float)ph);
#endif
    eglSwapBuffers(sDpy, sSurf[screen]);
}

/* The composited top screen, sampled down for capture_top.rgba: its own
 * buffer. It was written into sTop, which holds what the next frames lay over
 * the world and is not wholly redrawn: a capture stayed on the screen. */
static uint32_t sCaptureTop[TOP_W * TOP_H];
static bool sCaptureComposited;

static void Capture(void)
{
    char path[600];
    FILE *f;

    snprintf(path, sizeof(path), "%s/capture_top.rgba", AndroidHost_FilesDir());
    if ((f = fopen(path, "wb")))
    {
        fwrite(sCaptureComposited ? sCaptureTop : sTop, 4, TOP_W * TOP_H, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/capture_bottom.rgb565cm", AndroidHost_FilesDir());
    if ((f = fopen(path, "wb")))
    {
        fwrite(sBottom, 2, CTR_BOTTOM_WIDTH * CTR_BOTTOM_HEIGHT, f);
        fclose(f);
    }
    sCaptureComposited = false;
    CtrLog_Write(CTR_LOG_VIDEO, "capture written (frame %lu)", (unsigned long)sStats.frames);
}

static void Pace(void)
{
    struct timespec now;
    uint64_t start = CtrPlatform_Ticks();

    sNextFrame.tv_nsec += FRAME_NS;
    while (sNextFrame.tv_nsec >= 1000000000L)
    {
        sNextFrame.tv_nsec -= 1000000000L;
        sNextFrame.tv_sec++;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long behind = (long long)(now.tv_sec - sNextFrame.tv_sec) * 1000000000LL
                     + (now.tv_nsec - sNextFrame.tv_nsec);
    if (behind > 4 * FRAME_NS)
        sNextFrame = now;            /* a long stall: start the clock again */
    else if (behind < 0)
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sNextFrame, NULL);
    sStats.waitMs = CtrPlatform_TickMs(CtrPlatform_Ticks() - start);
}

/*
 * Where a late frame's time went: frames over 16.7 ms, summed by part over
 * the fps line's five seconds and printed beside it (port.log "late"). The
 * swap is where a busy GPU shows: eglSwapBuffers waits for it.
 */
enum { LATE_UPDATE, LATE_DRAW, LATE_TOP, LATE_BOTTOM, LATE_AFTER, LATE_MESH, LATE_DRAFT, LATE_ATLAS,
       LATE_TOTAL, LATE_COUNT };
static float sLatePart[LATE_COUNT];
static struct { unsigned frames; float sum[LATE_COUNT], max[LATE_COUNT]; } sLate;

static void LateSpan(int part, uint64_t from)
{
    sLatePart[part] += CtrPlatform_TickMs(svcGetSystemTick() - from);
}

static void LateFrame(uint64_t frameStart)
{
    const CtrVoxelStats *v = CtrVoxel_GetStats();

    sLatePart[LATE_TOTAL] = CtrPlatform_TickMs(svcGetSystemTick() - frameStart);
    if (sLatePart[LATE_TOTAL] > 1000.0f / 60.0f)
    {
        sLatePart[LATE_MESH] = v->meshMs;
        sLatePart[LATE_DRAFT] = v->draftMs;
        sLatePart[LATE_ATLAS] = v->atlasMs;
        sLate.frames++;
        for (int i = 0; i < LATE_COUNT; ++i)
        {
            sLate.sum[i] += sLatePart[i];
            if (sLatePart[i] > sLate.max[i])
                sLate.max[i] = sLatePart[i];
        }
    }
    memset(sLatePart, 0, sizeof(sLatePart));
}

static void LateLog(void)
{
    static const char *const names[LATE_COUNT] = {"update", "draw", "top", "bottom", "after",
                                                   "mesh", "draft", "atlas", "total"};
    char line[512];
    int at;

    if (sLate.frames == 0)
        return;
    at = snprintf(line, sizeof(line), "late: %u frames (ms mean/max)", sLate.frames);
    for (int i = 0; i < LATE_COUNT && at > 0 && at < (int)sizeof(line); ++i)
        at += snprintf(line + at, sizeof(line) - at, " %s %.1f/%.1f", names[i],
                       sLate.sum[i] / sLate.frames, sLate.max[i]);
    CtrLog_Write(CTR_LOG_VIDEO, "%s", line);
    memset(&sLate, 0, sizeof(sLate));
}

/* ── The voxel world (3ds_video.c RenderVoxel and what it calls) ─────── */
/*
 * The world is drawn by ctr_voxel.c through the Citro3D shim into the
 * logical surface - a 512x256 texture of which the top-left 432x240 is the
 * picture, v = 1 its top, as on the 3DS - and composed from it into the top
 * screen's picture (sComposite, row 0 its top) with the 3DS's passes: the
 * tilt-shift, the bloom, the dark of a cave, its circle of light, the 2D
 * field under the swoop's crossfade, the game's overlay and the swoop's
 * light.
 */
#define SURFACE_W 512
#define SURFACE_H 256
#define BLOOM_W (TOP_W / 4)
#define BLOOM_H (TOP_H / 4)
#define BLOOM_TEX_W 128
#define BLOOM_TEX_H 64
#define BLOOM_THRESHOLD 0.85f
#define DIORAMA_TOP 100
#define DIORAMA_BOTTOM 56
#define FLASH_LIGHT_DIM 64
#define FLASH_LIGHT_QUAD 1.25f
#define FLASH_LIGHT_INNER 0.82f
#define FLASH_LIGHT_OUTER 1.18f
#define FLASH_LIGHT_POOL 0.85f
/* How long the 2D field may wait for the voxel world's chunks (1.5 s), and
 * the most game frames one picture may step the swoop by. */
#define VOXEL_SWOOP_HOLD_FRAMES 90u
#define VOXEL_SWOOP_STEPS_MAX 4u

enum { FIELD_MODE_NONE, FIELD_MODE_PIXEL, FIELD_MODE_VOXEL };
enum { QUAD_TEXTURE, QUAD_THRESHOLD, QUAD_ALPHA, QUAD_SOLID, QUAD_MIX, QUAD_DIAMOND };

static C3D_Tex sSurfaceTex;
static C3D_RenderTarget *sLogical;
static GLuint sCompositeFbo, sCompositeTex, sBloomFbo, sBloomTex, sOverlayTex, sField2DTex;
static GLuint sFlashDarkTex, sFlashPoolTex;
static GLuint sSamplerNearest, sSamplerLinear;
static GLuint sQuadProg;
static GLint sQPos, sQSize, sQUv, sQEdge, sQMode, sQTint, sQThreshold, sQMix;
static int sQuadW, sQuadH;
static unsigned sFieldMode;
static VoxelSwoop sSwoop;
static bool sFieldVoxel;
/* The palette fade, as 3ds_video.c's DetectPaletteFade finds a uniform one. */
static float sPaletteFade;
static uint16_t sPaletteFadeColor;
extern uint16_t gPlttBufferUnfaded[];

static const char *sQuadVertex =
    "#version 300 es\n"
    "uniform vec4 uPos;\n"       /* x0 y0 x1 y1, target pixels from the top left */
    "uniform vec2 uSize;\n"
    "uniform vec4 uUv;\n"        /* at the top left, at the bottom right */
    "uniform vec4 uEdge;\n"      /* alpha at the top, bottom, left and right edges */
    "out vec2 vUv;\n"
    "out float vEdge;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = mix(uPos.xy, uPos.zw, c);\n"
    /* row 0 of the target is the picture's top: read as the screens read sTop */
    "  gl_Position = vec4(p / uSize * 2.0 - 1.0, 0.0, 1.0);\n"
    "  vUv = mix(uUv.xy, uUv.zw, c);\n"
    "  vEdge = mix(uEdge.x, uEdge.y, c.y) * mix(uEdge.z, uEdge.w, c.x);\n"
    "}\n";
static const char *sQuadFragment =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uMode;\n"
    "uniform vec4 uTint;\n"
    "uniform float uThreshold;\n"
    "uniform float uMix;\n"
    "in vec2 vUv;\n"
    "in float vEdge;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "  if (uMode == 3) { oColor = vec4(uTint.rgb, uTint.a * vEdge); return; }\n"
    "  if (uMode == 5) { oColor = vec4(uTint.rgb, uTint.a * vEdge * max(0.0, 1.0 - abs(vUv.x) - abs(vUv.y))); return; }\n"
    "  vec4 t = texture(uTex, vUv);\n"
    "  if (uMode == 1) { oColor = vec4(clamp((t.rgb - uThreshold) * 4.0, 0.0, 1.0), 1.0); return; }\n"
    "  if (uMode == 2) { oColor = vec4(uTint.rgb, uTint.a * t.a * vEdge); return; }\n"
    "  if (uMode == 4) { oColor = vec4(mix(t.rgb, uTint.rgb, uMix), t.a * uTint.a * vEdge); return; }\n"
    "  oColor = t * uTint;\n"
    "  oColor.a *= vEdge;\n"
    "}\n";

/* Draws into `fbo`, a w x h picture in coordinates, covered by the
 * viewport at `scale` times that in pixels. */
static void QuadTargetScaled(GLuint fbo, int w, int h, float scale)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, (GLsizei)(w * scale + 0.5f), (GLsizei)(h * scale + 0.5f));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    sQuadW = w;
    sQuadH = h;
}

static void QuadTarget(GLuint fbo, int w, int h) { QuadTargetScaled(fbo, w, h, 1.0f); }

/*
 * The world is drawn and composed at the top display's resolution, not the
 * 3DS's: the logical surface and the composite are sRenderScale times their
 * 432x240 picture (4.5 on the Thor's 1080 lines: 1944x1080, which covers the
 * 1920x1080 display pixel for pixel). Everything laid over them keeps the
 * picture's coordinates; the game's own pixel art (the overlay, the 2D field
 * of the swoop) is magnified onto it, sharp.
 */
static float sRenderScale;

static void HiResSurfaces(void)
{
    EGLint h = 0;
    float scale = 4.5f;

    if (sSurf[0] != EGL_NO_SURFACE && eglQuerySurface(sDpy, sSurf[0], EGL_HEIGHT, &h) && h > 0)
        scale = (float)h / TOP_H;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 8.0f) scale = 8.0f;
    if (scale == sRenderScale && sLogical)
        return;
    if (sLogical)
        C3D_RenderTargetDelete(sLogical);
    sLogical = AndroidC3D_RenderTargetScaled(&sSurfaceTex, GPU_RB_DEPTH24_STENCIL8, scale);
    glBindTexture(GL_TEXTURE_2D, sCompositeTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(TOP_W * scale + 0.5f), (GLsizei)(TOP_H * scale + 0.5f), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    sRenderScale = scale;
    CtrLog_Write(CTR_LOG_VIDEO, "voxel world drawn at %dx%d (x%.2f)", (int)(TOP_W * scale + 0.5f),
                 (int)(TOP_H * scale + 0.5f), scale);
}

/* The composite as shown, full size to capture_top_hd.rgba (its size in the
 * log) and sampled down into sTop for the usual capture. */
static void CaptureComposite(void)
{
    int w = (int)(TOP_W * sRenderScale + 0.5f), h = (int)(TOP_H * sRenderScale + 0.5f);
    uint32_t *full = (uint32_t *)malloc((size_t)w * h * 4);
    char path[600];
    FILE *f;

    if (!full)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, sCompositeFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, full);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < TOP_H; y++)
        for (int x = 0; x < TOP_W; x++)
            sCaptureTop[y * TOP_W + x] = full[(int)((y + 0.5f) * sRenderScale) * w + (int)((x + 0.5f) * sRenderScale)];
    sCaptureComposited = true;
    snprintf(path, sizeof(path), "%s/capture_top_hd.rgba", AndroidHost_FilesDir());
    if ((f = fopen(path, "wb")))
    {
        fwrite(full, 4, (size_t)w * h, f);
        fclose(f);
    }
    CtrLog_Write(CTR_LOG_VIDEO, "capture: top at %dx%d", w, h);
    free(full);
}

/* Straight alpha over what is there, keeping its alpha (C2D's blend). */
static void BlendOver(void)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

static void BlendAdd(void)
{
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
}

static void QuadEdges(GLuint tex, bool linear, int mode, float x0, float y0, float x1, float y1, float u0, float v0,
                      float u1, float v1, float r, float g, float b, float a, float edgeTop, float edgeBottom,
                      float edgeLeft, float edgeRight)
{
    glUseProgram(sQuadProg);
    glBindVertexArray(sVao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, linear ? sSamplerLinear : sSamplerNearest);
    glUniform4f(sQPos, x0, y0, x1, y1);
    glUniform2f(sQSize, (float)sQuadW, (float)sQuadH);
    glUniform4f(sQUv, u0, v0, u1, v1);
    glUniform4f(sQEdge, edgeTop, edgeBottom, edgeLeft, edgeRight);
    glUniform1i(sQMode, mode);
    glUniform4f(sQTint, r, g, b, a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindSampler(0, 0);
}

static void Quad(GLuint tex, bool linear, int mode, float x0, float y0, float x1, float y1, float u0, float v0,
                 float u1, float v1, float r, float g, float b, float a, float edgeTop, float edgeBottom)
{
    QuadEdges(tex, linear, mode, x0, y0, x1, y1, u0, v0, u1, v1, r, g, b, a, edgeTop, edgeBottom, 1, 1);
}

/* A piece of the logical surface: picture pixels from the top left. */
static void SurfaceQuad(bool linear, int mode, float x0, float y0, float x1, float y1, float sx, float sy, float sw,
                        float sh, float a, float edgeTop, float edgeBottom)
{
    Quad(AndroidC3D_GLTexture(&sSurfaceTex), linear, mode, x0, y0, x1, y1, sx / SURFACE_W, 1.0f - sy / SURFACE_H,
         (sx + sw) / SURFACE_W, 1.0f - (sy + sh) / SURFACE_H, 1, 1, 1, a, edgeTop, edgeBottom);
}

static void Rect(float x0, float y0, float x1, float y1, uint32_t abgr)
{
    Quad(0, false, QUAD_SOLID, x0, y0, x1, y1, 0, 0, 1, 1, (abgr & 255) / 255.0f, ((abgr >> 8) & 255) / 255.0f,
         ((abgr >> 16) & 255) / 255.0f, (abgr >> 24) / 255.0f, 1, 1);
}

static GLuint FboTexture(GLuint *fbo, int w, int h)
{
    GLuint tex = Texture(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return tex;
}

static void MakeFlashLight(void)
{
    static uint8_t dark[FLASH_LIGHT_DIM * FLASH_LIGHT_DIM * 4], pool[FLASH_LIGHT_DIM * FLASH_LIGHT_DIM * 4];
    const float inner = FLASH_LIGHT_INNER / FLASH_LIGHT_QUAD, outer = FLASH_LIGHT_OUTER / FLASH_LIGHT_QUAD;
    const float reach = FLASH_LIGHT_POOL / FLASH_LIGHT_QUAD;

    for (unsigned y = 0; y < FLASH_LIGHT_DIM; ++y)
        for (unsigned x = 0; x < FLASH_LIGHT_DIM; ++x)
        {
            float dx = ((float)x + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float dy = ((float)y + 0.5f) / (FLASH_LIGHT_DIM * 0.5f) - 1.0f;
            float d = sqrtf(dx * dx + dy * dy);
            float t = (d - inner) / (outer - inner), p = 1.0f - d / reach, warm;
            uint8_t *o = dark + (y * FLASH_LIGHT_DIM + x) * 4, *q = pool + (y * FLASH_LIGHT_DIM + x) * 4;

            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            t = t * t * (3.0f - 2.0f * t);
            /* Amber where the edge is half dark, black beyond. */
            warm = 0.72f * (1.0f - t);
            o[0] = (uint8_t)(warm * 255.0f);
            o[1] = (uint8_t)(warm * 150.0f);
            o[2] = (uint8_t)(warm * 60.0f);
            o[3] = (uint8_t)(t * 255.0f + 0.5f);
            p = p < 0.0f ? 0.0f : p;
            q[0] = q[1] = q[2] = 255;
            q[3] = (uint8_t)(p * p * 255.0f + 0.5f);
        }
    sFlashDarkTex = Texture(FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GL_RGBA, GL_UNSIGNED_BYTE, dark);
    sFlashPoolTex = Texture(FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FLASH_LIGHT_DIM, FLASH_LIGHT_DIM, GL_RGBA, GL_UNSIGNED_BYTE, pool);
}

/* The GL side of the voxel world, after the context is current. */
static void VoxelGlInit(void)
{
    if (!AndroidC3D_Init())
    {
        CtrLog_Write(CTR_LOG_ERROR, "voxel: the Citro3D shim did not start");
        return;
    }
    sQuadProg = glCreateProgram();
    glAttachShader(sQuadProg, Shader(GL_VERTEX_SHADER, sQuadVertex));
    glAttachShader(sQuadProg, Shader(GL_FRAGMENT_SHADER, sQuadFragment));
    glLinkProgram(sQuadProg);
    sQPos = glGetUniformLocation(sQuadProg, "uPos");
    sQSize = glGetUniformLocation(sQuadProg, "uSize");
    sQUv = glGetUniformLocation(sQuadProg, "uUv");
    sQEdge = glGetUniformLocation(sQuadProg, "uEdge");
    sQMode = glGetUniformLocation(sQuadProg, "uMode");
    sQTint = glGetUniformLocation(sQuadProg, "uTint");
    sQThreshold = glGetUniformLocation(sQuadProg, "uThreshold");
    sQMix = glGetUniformLocation(sQuadProg, "uMix");
    glUseProgram(sQuadProg);
    glUniform1i(glGetUniformLocation(sQuadProg, "uTex"), 0);
    glGenSamplers(1, &sSamplerNearest);
    glGenSamplers(1, &sSamplerLinear);
    glSamplerParameteri(sSamplerNearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sSamplerNearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(sSamplerLinear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sSamplerLinear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (int i = 0; i < 2; i++)
    {
        GLuint sm = i ? sSamplerLinear : sSamplerNearest;
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    sCompositeTex = FboTexture(&sCompositeFbo, TOP_W, TOP_H);
    sBloomTex = FboTexture(&sBloomFbo, BLOOM_TEX_W, BLOOM_TEX_H);
    sOverlayTex = Texture(TOP_W, TOP_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    sField2DTex = Texture(TOP_W, TOP_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    MakeFlashLight();
    if (!C3D_TexInitVRAM(&sSurfaceTex, SURFACE_W, SURFACE_H, GPU_RGBA8)
        || (HiResSurfaces(), sLogical == NULL))
    {
        CtrLog_Write(CTR_LOG_ERROR, "voxel: no logical surface");
        return;
    }
    sVoxelReady = CtrVoxel_Init();
    AndroidC3D_ResetState();
    CtrLog_Write(CTR_LOG_VIDEO, "voxel world %s", sVoxelReady ? "ready" : "unavailable");
}

static uint16_t FadeColor(unsigned color, unsigned to, unsigned y)
{
    int r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    int tr = to & 31, tg = (to >> 5) & 31, tb = (to >> 10) & 31;

    r += ((tr - r) * (int)y) >> 4;
    g += ((tg - g) * (int)y) >> 4;
    b += ((tb - b) * (int)y) >> 4;
    return (uint16_t)(r | g << 5 | b << 10);
}

/* A uniform fade of the backgrounds to black or white (3ds_video.c), 0..1. */
static void DetectPaletteFade(void)
{
    const uint16_t *now = sMem.palette, *unfaded = gPlttBufferUnfaded;

    sPaletteFade = 0.0f;
    if (!now || !memcmp(now, unfaded, 512))
        return;
    for (unsigned w = 0; w < 2; ++w)
        for (unsigned y = 1; y <= 16; ++y)
        {
            unsigned i = 0;

            while (i < 256 && FadeColor(unfaded[i], w ? 0x7fff : 0, y) == (now[i] & 0x7fff)) ++i;
            if (i == 256)
            {
                sPaletteFadeColor = w ? 0x7fff : 0;
                sPaletteFade = y / 16.0f;
                return;
            }
        }
}

static bool FieldLightShown(void)
{
    return sFieldLight.on && !sStage && !sBattle && sCentred == CTR_CENTRED_NONE;
}

static void VoxelDiorama(void)
{
    float strength = CtrSettings_VoxelBlurStrength() / 100.0f * (1.0f - CtrVoxel_SwoopNeutral());
    const struct { int y0, rows; bool top; float d, a; } taps[4] = {
        {0, DIORAMA_TOP, true, 0.5f, 0.67f}, {0, DIORAMA_TOP, true, -0.5f, 0.50f},
        {TOP_H - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, 0.5f, 0.60f},
        {TOP_H - DIORAMA_BOTTOM, DIORAMA_BOTTOM, false, -0.5f, 0.45f}};

    if (strength <= 0.005f)
        return;
    BlendOver();
    /* Half a texel either way, bilinear: two 2x2 averages either side of
     * each pixel, faded in from the focus band to the edge. */
    for (int i = 0; i < 4; i++)
    {
        float a = taps[i].a * strength;
        SurfaceQuad(true, QUAD_TEXTURE, 1, (float)taps[i].y0, TOP_W - 1, (float)(taps[i].y0 + taps[i].rows),
                    1.0f + taps[i].d, taps[i].y0 + taps[i].d, TOP_W - 2, (float)taps[i].rows, 1.0f,
                    taps[i].top ? a : 0.0f, taps[i].top ? 0.0f : a);
    }
}

static void VoxelBloomPrepare(void)
{
    QuadTarget(sBloomFbo, BLOOM_TEX_W, BLOOM_TEX_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glUseProgram(sQuadProg);
    glUniform1f(sQThreshold, BLOOM_THRESHOLD);
    SurfaceQuad(true, QUAD_THRESHOLD, 0, 0, BLOOM_W, BLOOM_H, 0, 0, TOP_W, TOP_H, 1, 1, 1);
}

static void VoxelBloomCompose(float strength)
{
    BlendAdd();
    Quad(sBloomTex, true, QUAD_TEXTURE, 0, 0, TOP_W, TOP_H, 0, 0, (float)BLOOM_W / BLOOM_TEX_W,
         (float)BLOOM_H / BLOOM_TEX_H, 1, 1, 1, strength, 1, 1);
}

static void VoxelGloom(void)
{
    float x, y, size, amount;
    const C3D_Tex *tex = CtrVoxel_Gloom(&x, &y, &size, &amount);

    if (tex == NULL || amount <= 0.0f)
        return;
    GLuint gl = AndroidC3D_GLTexture((C3D_Tex *)tex);
    BlendOver();
    Quad(gl, true, QUAD_ALPHA, x - size * 0.5f, y - size * 0.5f, x + size * 0.5f, y + size * 0.5f, 0, 1, 1, 0, 0, 0,
         0, amount > 1.0f ? 1.0f : amount, 1, 1);
}

static void VoxelFlashLight(void)
{
    float x = TOP_W * 0.5f, y = TOP_H * 0.5f, sx = 1.0f, sy = 1.0f;
    float t = (float)sFieldLight.ticks, radius, w, h, x0, y0, x1, y1, fade = sPaletteFade;
    uint32_t to = ToRGBA(sPaletteFadeColor);
    float dr = (to & 255) / 255.0f * fade, dg = ((to >> 8) & 255) / 255.0f * fade, db = ((to >> 16) & 255) / 255.0f * fade;
    uint32_t dark = 0xFF000000u | (uint32_t)(dr * 255) | (uint32_t)(dg * 255) << 8 | (uint32_t)(db * 255) << 16;

    if (!FieldLightShown())
        return;
    CtrVoxel_PlayerLightSpot(&x, &y, &sx, &sy);
    radius = sFieldLight.shown * (1.0f + 0.010f * sinf(t * 0.071f) + 0.006f * sinf(t * 0.173f + 1.3f));
    w = radius * FLASH_LIGHT_QUAD * sx;
    h = radius * FLASH_LIGHT_QUAD * sy;
    x0 = x - w; x1 = x + w;
    y0 = y - h; y1 = y + h;
    if (radius > 0.5f)
    {
        float glow = (0.10f + 0.03f * sinf(t * 0.21f) + 0.55f * sFieldLight.burst) * (1.0f - fade);

        if (glow > 0.004f)
        {
            BlendAdd();
            Quad(sFlashPoolTex, true, QUAD_ALPHA, x0, y0, x1, y1, 0, 0, 1, 1, 1.0f, 196 / 255.0f, 128 / 255.0f,
                 glow > 1.0f ? 1.0f : glow, 1, 1);
        }
        BlendOver();
        glUseProgram(sQuadProg);
        glUniform1f(sQMix, fade);
        Quad(sFlashDarkTex, true, QUAD_MIX, x0, y0, x1, y1, 0, 0, 1, 1, dr, dg, db, 1, 1, 1);
    }
    else
        x0 = x1 = x, y0 = y1 = y;
    BlendOver();
    if (y0 > 0.0f) Rect(0, 0, TOP_W, y0, dark);
    if (y1 < TOP_H) Rect(0, y1, TOP_W, TOP_H, dark);
    if (x0 > 0.0f) Rect(0, y0, x0, y1, dark);
    if (x1 < TOP_W) Rect(x1, y0, TOP_W, y1, dark);
}

static void DrawSwoopLight(const VoxelSwoopFrame *fx)
{
    const float cx = TOP_W * 0.5f, cy = TOP_H * 0.5f;
    float r = (fx->colour & 255) / 255.0f, g = ((fx->colour >> 8) & 255) / 255.0f, b = ((fx->colour >> 16) & 255) / 255.0f;

    BlendOver();
    if (fx->bloom > 0.005f)
    {
        /* A diamond, bright in the middle and gone at the screen's edges. */
        float rx = TOP_W * 0.75f, ry = TOP_H * 0.9f;
        Quad(0, false, QUAD_DIAMOND, cx - rx, cy - ry, cx + rx, cy + ry, -1, -1, 1, 1, r, g, b, fx->bloom * 0.9f, 1, 1);
    }
    if (fx->flash > 0.005f)
        Quad(0, false, QUAD_SOLID, 0, 0, TOP_W, TOP_H, 0, 0, 1, 1, r, g, b, fminf(fx->flash, 1.0f), 1, 1);
}

/* The CPU picture `pixels` (row 0 the top) over the composite at `alpha`;
 * premultiplied when it is the overlay. */
static void PictureOver(GLuint tex, const uint32_t *pixels, bool premultiplied, float alpha)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_W, TOP_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    if (premultiplied)
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    else
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    Quad(tex, false, QUAD_TEXTURE, 0, 0, TOP_W, TOP_H, 0, 0, 1, 1, 1, 1, 1, alpha, 1, 1);
}

/* The world drawn into the logical surface and laid into sComposite with its
 * tilt-shift; the bloom prepared at `bloom` (returned, 0: none). */
static float WorldBase(float bloomBurst)
{
    uint16_t backdrop = (Reg(0) & 128) ? 0x7fff : (sMem.palette[0] & 0x7fff);
    uint32_t rgba = ToRGBA(backdrop);
    /* 0xRRGGBBAA, as C3D_RenderTargetClear takes it */
    uint32_t clear = (rgba & 255) << 24 | ((rgba >> 8) & 255) << 16 | ((rgba >> 16) & 255) << 8 | 255;
    float bloom;

    HiResSurfaces();
    AndroidC3D_FlushLinear();
    C3D_RenderTargetClear(sLogical, C3D_CLEAR_ALL, clear, 0);
    CtrVoxel_Draw(sLogical, -1);
    AndroidC3D_ResetState();

    bloom = fminf(CtrVoxel_Bloom() + bloomBurst, 1.0f);
    if (bloom > 0.005f)
        VoxelBloomPrepare();
    QuadTargetScaled(sCompositeFbo, TOP_W, TOP_H, sRenderScale);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    BlendOver();
    SurfaceQuad(false, QUAD_TEXTURE, 0, 0, TOP_W, TOP_H, 0, 0, TOP_W, TOP_H, 1, 1, 1);
    if (CtrSettings_VoxelBlur())
        VoxelDiorama();
    return bloom;
}

/* The brightness effect (BLDY), 0..1 when it is on. */
static float Brightness(void)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3, evy = Reg(0x54) & 31;
    return effect >= 2 ? (evy > 16 ? 16 : evy) / 16.0f : 0.0f;
}

/* The world and everything over it, into sComposite (RenderVoxelTo). */
static void ComposeVoxel(float fade2d)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    float bright = Brightness(), bloom;
    VoxelSwoopFrame swoop;

    VoxelSwoop_Frame(&sSwoop, &swoop);
    /* Arriving from the 2D field, the world's light rings down from white -
     * unless the game has an effect of its own on. */
    if (swoop.whiteout > 0.0f && bright <= 0.0f)
        CtrVoxel_SetBrightness(swoop.whiteout, swoop.whiteout, true);
    else
        CtrVoxel_SetBrightness((control & 0x0e) ? bright : 0.0f, (control & 0x10) ? bright : 0.0f, effect == 2);
    bloom = WorldBase(0.0f);
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    VoxelGloom();
    VoxelFlashLight();
    /* The swoop's crossfade: the 2D field, without its text, over the world. */
    if (fade2d > 0.004f)
        PictureOver(sField2DTex, sField2D, false, fade2d > 1.0f ? 1.0f : fade2d);
    if (!(Reg(0) & 128))
        PictureOver(sOverlayTex, sTop, true, 1.0f);
}


/* ── The 3D battle (3ds_video.c RenderBattleWorld) ─────────────────────── */
/*
 * The battle in front of the voxel world: the world is the battle's scenery,
 * drawn where the GBA draws BG3 from the stage voxel_battle.c chose near the
 * player, and the battle's own picture - composed by the CPU renderer as the
 * 2D battle is, BG3 left out and its backdrop transparent - over it.
 */
#define BATTLE_BAND_TOP 112
/*
 * The battle laid out as on the 3DS: the scene (the GBA's rows above the
 * text box) magnified CTR_BATTLE_ZOOM, the middle of its bottom edge on the
 * top of the text box, and the text box at 1:1 on the screen's bottom
 * lines, centred. The battle's picture is rendered 240x160 into the top-left
 * of sTop and laid out from there (BattlePictureOver).
 */
#define BATTLE_BOX_Y (TOP_H - (GBA_H - BATTLE_BAND_TOP))
#define BATTLE_SCENE_X (TOP_W * 0.5f - 120.0f * CTR_BATTLE_ZOOM)
#define BATTLE_SCENE_Y (BATTLE_BOX_Y - BATTLE_BAND_TOP * CTR_BATTLE_ZOOM)
#define BATTLE_INTRO_FLOOR BATTLE_BOX_Y
/* The GBA pixels the view reaches past the picture's left and top edges
 * (sOrgX, sOrgY), and so the size of the battle's render in sTop. */
#define BATTLE_MARGIN_X ((int)(BATTLE_SCENE_X / CTR_BATTLE_ZOOM) + 2)
#define BATTLE_MARGIN_Y ((int)(BATTLE_SCENE_Y / CTR_BATTLE_ZOOM) + 2)
#define BATTLE_VIEW_W (GBA_W + 2 * BATTLE_MARGIN_X)
#define BATTLE_VIEW_H (BATTLE_MARGIN_Y + GBA_H)
#define BATTLE_INTRO_BAR 22.0f
#define BATTLE_INTRO_BAR_EDGE 10.0f
#define BATTLE_INTRO_STREAKS 12u
#define BATTLE_SHADOW_W 64
#define BATTLE_SHADOW_H 16
#define BATTLE_SHADOW_ALPHA 0.50f

static unsigned sWorldLayers;
static GLuint sBattleShadowTex;

static GLuint BattleShadowTexture(void)
{
    static uint8_t texels[BATTLE_SHADOW_W * BATTLE_SHADOW_H * 4];

    if (sBattleShadowTex)
        return sBattleShadowTex;
    for (unsigned y = 0; y < BATTLE_SHADOW_H; ++y)
        for (unsigned x = 0; x < BATTLE_SHADOW_W; ++x)
        {
            float u = (x + 0.5f) / (BATTLE_SHADOW_W / 2) - 1.0f, v = (y + 0.5f) / (BATTLE_SHADOW_H / 2) - 1.0f;
            float d = 1.0f - (u * u + v * v), a = d > 0.0f ? BATTLE_SHADOW_ALPHA * d * sqrtf(d) : 0.0f;
            uint8_t *o = texels + (y * BATTLE_SHADOW_W + x) * 4;

            o[0] = 8;
            o[1] = 20;
            o[2] = 40;
            o[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    sBattleShadowTex = Texture(BATTLE_SHADOW_W, BATTLE_SHADOW_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BATTLE_SHADOW_W, BATTLE_SHADOW_H, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    return sBattleShadowTex;
}

/* A soft shadow on the ground under each battler and trainer. */
static void BattleWorldShadows(void)
{
    VoxelBattleShadow shadows[4];
    unsigned count = VoxelBattle_Shadows(shadows, 4);
    GLuint tex = count ? BattleShadowTexture() : 0;

    for (unsigned i = 0; i < count; ++i)
    {
        float x = shadows[i].x * CTR_BATTLE_ZOOM + BATTLE_SCENE_X, y = shadows[i].y * CTR_BATTLE_ZOOM + BATTLE_SCENE_Y;
        float rx = shadows[i].rx * CTR_BATTLE_ZOOM, ry = shadows[i].ry * CTR_BATTLE_ZOOM;
        Quad(tex, true, QUAD_TEXTURE, x - rx, y - ry, x + rx, y + ry, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1);
    }
}

/* BG3's scroll from rest, GBA pixels, either way round its 256-pixel turn. */
static float BattleScenerySway(unsigned reg)
{
    return (float)((int)((Reg(reg) + 128) & 255) - 128);
}

/* The entrance's zoom blur: two copies of the world magnified about the
 * screen's middle, while the camera travels. */
static void BattleIntroBlur(const CtrVoxelBattleIntro *intro)
{
    const float cx = TOP_W * 0.5f, cy = TOP_H * 0.5f;
    const float taps[2][2] = {{0.035f, 0.50f}, {0.080f, 0.32f}};
    float s = intro->speed;

    if (s < 0.04f || (Reg(0) & 128))
        return;
    BlendOver();
    for (int i = 0; i < 2; i++)
    {
        float scale = 1.0f + taps[i][0] * s, x0 = cx + (1.0f - cx) * scale, y0 = cy - cy * scale;
        SurfaceQuad(true, QUAD_TEXTURE, x0, y0, x0 + (TOP_W - 2) * scale, y0 + TOP_H * scale, 1, 0, TOP_W - 2,
                    TOP_H, taps[i][1] * s, 1, 1);
    }
}

/* Over the world, under the battle: the speed lines and the vignette. */
static void BattleIntroAtmosphere(const CtrVoxelBattleIntro *intro)
{
    const float w = TOP_W, bottom = BATTLE_INTRO_FLOOR;
    float shade = fmaxf(0.60f * intro->flight, 0.30f * intro->bars);

    BlendOver();
    if (intro->speed > 0.04f)
        for (unsigned i = 0; i < BATTLE_INTRO_STREAKS; ++i)
        {
            uint32_t h = (i + 1) * 2654435761u;
            float y = 6.0f + (float)((h >> 8) % (unsigned)(bottom - 12.0f));
            float length = 70.0f + (float)((h >> 16) % 110u);
            float pace = 26.0f + (float)((h >> 20) % 18u);
            float x = w - fmodf(intro->time * pace + (float)(h % 400u), w + length);
            float alpha = intro->speed * (0.35f + 0.35f * (float)((h >> 4) & 3) / 3.0f);

            if (fabsf(y - bottom * 0.55f) < 24.0f)
                y = y < bottom * 0.55f ? y - 24.0f : y + 24.0f;
            QuadEdges(0, false, QUAD_SOLID, x, y, x + length, y + ((h & 16) ? 2.0f : 1.0f), 0, 0, 1, 1,
                      225 / 255.0f, 236 / 255.0f, 1.0f, alpha, 1, 1, 1, 0);
        }
    if (shade > 0.01f)
    {
        /* Down to the screen's edge: the text box is narrower than the
         * screen here, and a vignette ending at its top showed its cut
         * either side of it. */
        float a = fminf(shade, 1.0f) * 230.0f / 255.0f, band = 64.0f, side = 96.0f, low = TOP_H;
        float r = 8 / 255.0f, g = 6 / 255.0f, b = 34 / 255.0f;

        QuadEdges(0, false, QUAD_SOLID, 0, 0, w, band, 0, 0, 1, 1, r, g, b, a, 1, 0, 1, 1);
        QuadEdges(0, false, QUAD_SOLID, 0, low - band, w, low, 0, 0, 1, 1, r, g, b, a, 0, 1, 1, 1);
        QuadEdges(0, false, QUAD_SOLID, 0, 0, side, low, 0, 0, 1, 1, r, g, b, a, 1, 1, 1, 0);
        QuadEdges(0, false, QUAD_SOLID, w - side, 0, w, low, 0, 0, 1, 1, r, g, b, a, 1, 1, 0, 1);
    }
}

/* The cinema bars, over everything. */
static void BattleIntroBars(const CtrVoxelBattleIntro *intro)
{
    float bar, edge;

    if (intro->bars <= 0.01f || (Reg(0) & 128))
        return;
    bar = BATTLE_INTRO_BAR * intro->bars;
    edge = BATTLE_INTRO_BAR_EDGE * intro->bars;
    BlendOver();
    Rect(0, 0, TOP_W, bar, 0xFF000000u);
    Quad(0, false, QUAD_SOLID, 0, bar, TOP_W, bar + edge, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0);
    Rect(0, TOP_H - bar, TOP_W, TOP_H, 0xFF000000u);
    Quad(0, false, QUAD_SOLID, 0, TOP_H - bar - edge, TOP_W, TOP_H - bar, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1);
}

/*
 * The info boxes (3ds_battle_hud.c): each box's picture, made for the
 * screen's own pixels, where its sprite's left half is in the magnified scene
 * - the GBA's order kept, the lower OAM entry in front - under the text box.
 */
static GLuint sHudTex[CTR_HUD_BOXES];
static uint32_t sHudVersion[CTR_HUD_BOXES];
static bool sHudReady[CTR_HUD_BOXES];

static const uint8_t sObjDims[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};

/* This frame's boxes, their pictures uploaded, and the OAM entries that are
 * theirs (before the battle's picture is rendered). */
/* 0: the game's own boxes, magnified with the scene, always. */
#ifndef BATTLE_HUD_CRISP
#define BATTLE_HUD_CRISP 1
#endif

/* A visible OAM entry's place in GBA pixels, as the battle reads it: drawn,
 * or with `window` one in the OBJ window's mode (3ds_video.c OamPlace). */
static bool OamPlace(unsigned i, bool window, unsigned *priority, int *x, int *y, int *w, int *h)
{
    const uint16_t *obj = sMem.oam + i * 4;
    unsigned attr0 = obj[0], attr1 = obj[1], attr2 = obj[2], shape = attr0 >> 14, mode = (attr0 >> 10) & 3;
    bool affine = (attr0 & 0x100) != 0;

    if ((!affine && (attr0 & 0x200)) || shape == 3 || (mode == 2) != window || mode == 3)
        return false;
    *w = sObjDims[shape][attr1 >> 14][0] * (affine && (attr0 & 0x200) ? 2 : 1);
    *h = sObjDims[shape][attr1 >> 14][1] * (affine && (attr0 & 0x200) ? 2 : 1);
    *x = attr1 & 511;
    *y = attr0 & 255;
    if (*x + *w > 512) *x -= 512;
    if (*y + *h > 256) *y -= 256;
    /* Where the GBA parks unused entries (y 160), tile 0 - an info box's
     * first tile as often as not. */
    if (*y >= GBA_H || *y + *h <= 0)
        return false;
    *priority = (attr2 >> 10) & 3;
    return true;
}

static int HudLeftHalf(unsigned i)
{
    int k = sHudOamBox[i];

    return k >= 0 && (sMem.oam[i * 4 + 2] & 1023) == sHud[k].boxTile ? k : -1;
}

/*
 * Whether the boxes' pictures can go over the scene (3ds_video.c
 * HudCrispPossible): nothing the scene drew last frame covered one, no
 * sprite in front of a box overlaps its picture, and no sprite in the OBJ
 * window's mode (a stat change's pattern) shapes the sprites over it. Also
 * notes each picture's place for this frame's render.
 */
static bool HudCrispPossible(void)
{
    bool possible = !sHudCovered;
    bool objWindow = (Reg(0) & 0x8000) && !((Reg(0x4A) >> 8) & 0x10);

    sHudRects = 0;
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned p;
        int x, y, w, h, k = HudLeftHalf(i);

        if (k < 0 || !OamPlace(i, false, &p, &x, &y, &w, &h))
            continue;
        /* The picture's extent in GBA pixels. */
        w = (int)ceilf(CTR_HUD_W / CTR_BATTLE_ZOOM);
        h = (int)ceilf(CTR_HUD_H / CTR_BATTLE_ZOOM);
        if (sHudRects < CTR_HUD_BOXES)
        {
            int *rect = sHudRect[sHudRects++];
            rect[0] = x; rect[1] = y; rect[2] = x + w; rect[3] = y + h; rect[4] = (int)p;
        }
        for (unsigned j = 0; j < 128 && possible; ++j)
        {
            unsigned q;
            int ox, oy, ow, oh;

            if (j == i || sHudOamBox[j] >= 0)
                continue;
            if (objWindow && OamPlace(j, true, &q, &ox, &oy, &ow, &oh)
                && ox < x + w && ox + ow > x && oy < y + h && oy + oh > y)
                possible = false;
            if (!OamPlace(j, false, &q, &ox, &oy, &ow, &oh) || q > p || (q == p && j > i))
                continue;
            if (ox < x + w && ox + ow > x && oy < y + h && oy + oh > y)
                possible = false;
        }
    }
    return possible;
}

static void HudFrame(void)
{
    static uint8_t rgba[CTR_HUD_W * CTR_HUD_H * 4];
    static int lastCrisp = -1;
    bool crisp;

    /* What last frame's render found (HudCoverPixel). */
    sHudCovered = sHudCoveredNow;
    sHudCoveredNow = false;
    sHud = NULL;
    sHudRects = 0;
    memset(sHudOamBox, -1, sizeof(sHudOamBox));
    if (!BATTLE_HUD_CRISP || !sBattle || !sVoxelReady || !CtrBattleHud_Ready())
    {
        lastCrisp = -1;
        return;
    }
    sHud = CtrBattleHud_Boxes();
    if (!sHud)
        return;
    for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
    {
        if (!sHud[k].used || !sHud[k].pixels)
        {
            sHudReady[k] = false;
            continue;
        }
        if (sHudReady[k] && sHudVersion[k] == sHud[k].version)
            continue;
        if (!sHudTex[k])
            sHudTex[k] = Texture(CTR_HUD_W, CTR_HUD_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        for (unsigned i = 0; i < CTR_HUD_W * CTR_HUD_H; ++i)
        {
            uint32_t c = sHud[k].pixels[i]; /* 0xRRGGBBAA */
            rgba[i * 4] = (uint8_t)(c >> 24);
            rgba[i * 4 + 1] = (uint8_t)(c >> 16);
            rgba[i * 4 + 2] = (uint8_t)(c >> 8);
            rgba[i * 4 + 3] = (uint8_t)c;
        }
        glBindTexture(GL_TEXTURE_2D, sHudTex[k]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CTR_HUD_W, CTR_HUD_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        sHudVersion[k] = sHud[k].version;
        sHudReady[k] = true;
        CtrBattleHud_Uploaded(k);
    }
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned tile = sMem.oam[i * 4 + 2] & 1023;
        for (unsigned k = 0; k < CTR_HUD_BOXES; ++k)
        {
            const CtrHudBox *box = &sHud[k];
            if (sHudReady[k] && ((tile >= box->boxTile && tile < (unsigned)box->boxTile + box->boxTiles)
                                 || (box->barTiles && tile >= box->barTile && tile < (unsigned)box->barTile + box->barTiles)))
                sHudOamBox[i] = (int8_t)k;
        }
    }
    /* Covered, or likely to be: the game's own boxes this frame, in the
     * scene with whatever is in front of them. The places stay noted, so
     * the render tells when they are clear again. */
    crisp = HudCrispPossible();
    if (crisp != lastCrisp)
        CtrLog_Write(CTR_LOG_VIDEO, "HUD boxes %s (covered %d)", crisp ? "in HD" : "the game's own", sHudCovered);
    lastCrisp = crisp;
    if (!crisp)
    {
        sHud = NULL;
        memset(sHudOamBox, -1, sizeof(sHudOamBox));
    }
}

static void HudDraw(void)
{
    if (!sHud)
        return;
    for (int i = 127; i >= 0; --i)
    {
        const uint16_t *obj = sMem.oam + i * 4;
        unsigned attr0 = obj[0], attr1 = obj[1], shape = attr0 >> 14;
        int k = sHudOamBox[i], x, y, h;
        bool affine = (attr0 & 0x100) != 0;

        if (k < 0 || (obj[2] & 1023) != sHud[k].boxTile)
            continue;
        if ((!affine && (attr0 & 0x200)) || shape == 3 || ((attr0 >> 10) & 3) >= 2)
            continue;
        h = sObjDims[shape][attr1 >> 14][1];
        x = attr1 & 511;
        y = attr0 & 255;
        if (x >= 256) x -= 512;
        if (y + h > 256) y -= 256;
        if (y >= GBA_H || y + h <= 0)
            continue;
        float sx = roundf(x * CTR_BATTLE_ZOOM + BATTLE_SCENE_X), sy = roundf(y * CTR_BATTLE_ZOOM + BATTLE_SCENE_Y);
        uint32_t fc = sHud[k].fadeColour; /* 0xRRGGBB00 */

        glUseProgram(sQuadProg);
        glUniform1f(sQMix, sHud[k].fade);
        Quad(sHudTex[k], false, QUAD_MIX, sx, sy, sx + CTR_HUD_W, sy + CTR_HUD_H, 0, 0, 1, 1, (fc >> 24) / 255.0f,
             ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, 1, 1, 1);
    }
}


/*
 * The battle text box in HD (3ds_video.c DrawBattleTextHd): a rounded panel
 * (gen_battle_hud.py's art: a left cap, a middle column, the cap mirrored)
 * across the screen's bottom with the scene showing round it, and the game's
 * message window over it 1:1 - its text, the window's fill left out. Only
 * while BG0 shows the message box; anything else is the GBA's box. Over the
 * voxel world it waits out of sight while the camera flies in and slides in
 * as it lands, the player's trainer lifted with it.
 */
#define BOX_TEXT_SHIFT 4
#define BOX_SLIDE_FRAMES 32
#define BOX_HIDDEN 60.0f
#define BOX_TRAINER_HIDDEN 90.0f
#define BOX_BARS_LEAVING 0.35f
#define BOX_TEXT_W 208
#define BOX_TEXT_H 32

static GLuint sBoxCapTex, sBoxMiddleTex, sBoxTextTex;
static bool sBoxTexFailed;
static int sBoxCapW, sBoxArtH, sBoxArtY;
static bool sBoxText, sBoxHidden, sBoxDecided, sBoxBarsUp;
static int sBoxSlide = -1;
static uint32_t sBoxBattleFrames;
static uint32_t sBoxTextPixels[BOX_TEXT_W * BOX_TEXT_H];

static GLuint ArtTexture(const uint32_t *art, int w, int h, int stride)
{
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    GLuint tex;

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint32_t c = art[y * stride + x]; /* 0xRRGGBBAA */
            uint8_t *o = rgba + (y * w + x) * 4;
            o[0] = (uint8_t)(c >> 24);
            o[1] = (uint8_t)(c >> 16);
            o[2] = (uint8_t)(c >> 8);
            o[3] = (uint8_t)c;
        }
    tex = Texture(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return tex;
}

static bool BoxTexture(void)
{
    const uint32_t *cap, *middle;
    int cx, cy, cw, ch, mx, my, mw, mh;

    if (sBoxCapTex) return true;
    if (sBoxTexFailed || !CtrBattleHud_Ready()) return false;
    cap = CtrBattleHud_BoxArt(false, &cx, &cy, &cw, &ch);
    middle = CtrBattleHud_BoxArt(true, &mx, &my, &mw, &mh);
    if (!cap || !middle || mh != ch || cw <= 0 || ch <= 0)
    {
        sBoxTexFailed = true;
        return false;
    }
    sBoxCapTex = ArtTexture(cap, cw, ch, cw);
    sBoxMiddleTex = ArtTexture(middle, 1, ch, mw);
    sBoxTextTex = Texture(BOX_TEXT_W, BOX_TEXT_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    sBoxCapW = cw;
    sBoxArtH = ch;
    sBoxArtY = cy;
    return true;
}

static unsigned MapEntry(unsigned map, unsigned size, unsigned column, unsigned row)
{
    unsigned at;

    column &= (size & 1) ? 63u : 31u;
    at = map + CtrVideo_TextMapOffset(0, row, size) + (column & 31) * 2 + ((column >> 5) & 1) * 2048;
    return at + 1 < 0x10000 ? (unsigned)(sMem.vram[at] | sMem.vram[at + 1] << 8) : 0;
}

/* The message window's 26x4 cells: its text over transparency. */
static void BoxWindowRead(unsigned map, unsigned size, unsigned chars)
{
    sBoxText = false;
    for (unsigned i = 0; i < 26 * 4; ++i)
    {
        unsigned entry = MapEntry(map, size, 2 + i % 26, 15 + i / 26);
        unsigned address = chars + (entry & 1023) * 32, bank = entry >> 12;
        unsigned cx = (i % 26) * 8, cy = (i / 26) * 8;

        if (address + 32 > 0x10000) address = 0;
        for (unsigned p = 0; p < 64; ++p)
        {
            unsigned px = p & 7, py = p >> 3;
            unsigned sx = (entry & 1024) ? 7 - px : px, sy = (entry & 2048) ? 7 - py : py;
            unsigned index = (sMem.vram[address + sy * 4 + sx / 2] >> ((sx & 1) * 4)) & 15;
            uint32_t texel = 0;

            /* The window's fill (index 15) and nothing (0) are the panel. */
            if (index && index != 15)
            {
                texel = ToRGBA(Pal(bank * 16 + index));
                sBoxText = true;
            }
            sBoxTextPixels[(cy + py) * BOX_TEXT_W + cx + px] = texel;
        }
    }
}

/* Whether BG0 shows the battle's message box (read into the text), and how
 * far it is scrolled down. */
static bool BoxShown(int *down)
{
    unsigned control = Reg(8), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    unsigned scrollX = Reg(0x10) & 511, scrollY = Reg(0x12) & 511;

    *down = scrollY >= 256 ? 512 - (int)scrollY : -(int)scrollY;
    if (!(Reg(0) & 0x100) || scrollX != 0 || *down < 0 || *down > 64 || (control & 128))
        return false;
    if ((MapEntry(map, size, 2, 15) & 1023) == 0x90 && (MapEntry(map, size, 27, 18) & 1023) == 0x90 + 103)
        BoxWindowRead(map, size, chars);
    else if ((MapEntry(map, size, 2, 15) & 1023) == 0x0a && (MapEntry(map, size, 27, 18) & 1023) == 0x0a
             && (MapEntry(map, size, 0, 14) & 1023) == 0x03 && (MapEntry(map, size, 29, 19) & 1023) == 0x11)
    {
        memset(sBoxTextPixels, 0, sizeof(sBoxTextPixels));
        sBoxText = false;
    }
    else
        return false;
    return BoxTexture();
}

static float BoxOffset(void)
{
    float t;

    if (sBoxHidden) return BOX_HIDDEN;
    if (sBoxSlide < 0) return 0;
    t = 1.0f - (float)sBoxSlide / BOX_SLIDE_FRAMES;
    return BOX_HIDDEN * t * t * t;
}

static bool sBattleWorld;

/* Once a frame: whether the box waits out of sight, and its slide. */
static void BoxFrame(void)
{
    static bool sliding;
    bool slidingNow;

    if (!sBattle)
    {
        sBoxHidden = sliding = false;
        if (!sBoxDecided || !(sVoxelReady && CtrVoxel_InBattle()))
            sBoxDecided = sBoxBarsUp = false;
        sBoxSlide = -1;
        sBoxBattleFrames = 0;
        sBoxText = false;
        return;
    }
    ++sBoxBattleFrames;
    if (sBoxSlide >= 0 && ++sBoxSlide > BOX_SLIDE_FRAMES) sBoxSlide = -1;
    if (!sBoxDecided)
    {
        if (sBattleWorld && !sBoxText) sBoxHidden = true;
        if (sBattleWorld || sBoxText) sBoxDecided = true;
    }
    if (sBoxHidden)
    {
        CtrVoxelBattleIntro intro;
        bool barsGone;

        slidingNow = CtrBattleIntro_Sliding() != 0;
        CtrVoxel_BattleIntro(&intro);
        if (intro.bars > 0.9f) sBoxBarsUp = true;
        barsGone = sBoxBarsUp && intro.bars < BOX_BARS_LEAVING;
        /* In as the camera lands, the first message begun, or after ten seconds. */
        if (barsGone || sBoxText || (sliding && !slidingNow) || sBoxBattleFrames > 600)
        {
            sBoxHidden = false;
            sBoxSlide = 0;
        }
        sliding = slidingNow;
    }
    {
        float offset = BoxOffset();
        CtrBattleHud_LiftPlayerTrainer((int)lroundf(offset > 0 ? offset * (BOX_TRAINER_HIDDEN / BOX_HIDDEN)
                                                               : offset / CTR_BATTLE_ZOOM));
    }
}

/* The panel and its text over the composite, `down` lines scrolled. */
static void BoxDraw(int down)
{
    float y0 = sBoxArtY + down + BoxOffset(), cap = (float)sBoxCapW, h = (float)sBoxArtH;
    float to = 0.0f;
    uint32_t c = ToRGBA(sPaletteFadeColor);

    if (y0 >= TOP_H)
        return;
    BlendOver();
    /* The panel is unfaded art: the screen's fade as a tint. */
    to = sPaletteFade;
    glUseProgram(sQuadProg);
    glUniform1f(sQMix, to);
    Quad(sBoxCapTex, false, QUAD_MIX, 0, y0, cap, y0 + h, 0, 0, 1, 1, (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f,
         ((c >> 16) & 255) / 255.0f, 1, 1, 1);
    Quad(sBoxMiddleTex, false, QUAD_MIX, cap, y0, TOP_W - cap, y0 + h, 0, 0, 1, 1, (c & 255) / 255.0f,
         ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, 1, 1, 1);
    Quad(sBoxCapTex, false, QUAD_MIX, TOP_W - cap, y0, TOP_W, y0 + h, 1, 0, 0, 1, (c & 255) / 255.0f,
         ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, 1, 1, 1);
    /* The message window: GBA (16, 120), where the band puts it. */
    glBindTexture(GL_TEXTURE_2D, sBoxTextTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BOX_TEXT_W, BOX_TEXT_H, GL_RGBA, GL_UNSIGNED_BYTE, sBoxTextPixels);
    {
        float tx = 16 + BOX_TEXT_SHIFT, ty = (TOP_H - 48) + 8 + down + BoxOffset();
        Quad(sBoxTextTex, false, QUAD_TEXTURE, tx, ty, tx + BOX_TEXT_W, ty + BOX_TEXT_H, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1);
    }
}

/* The battle's picture (240x160 at sTop's top left, premultiplied) laid out
 * over the composite: the scene magnified, the info boxes, the text box
 * under them. */
static void BattlePictureOver(void)
{
    const float mx = BATTLE_MARGIN_X, my = BATTLE_MARGIN_Y;
    const float u1 = (float)BATTLE_VIEW_W / TOP_W, v1 = (float)BATTLE_VIEW_H / TOP_H;

    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_W, TOP_H, GL_RGBA, GL_UNSIGNED_BYTE, sTop);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    /* The scene from the view's top left corner, GBA (-mx, -my). */
    /* Down to GBA line 160, past the screen's bottom: the scene goes on
     * behind the text box (its terrain's last lines repeated). */
    Quad(sOverlayTex, false, QUAD_TEXTURE, BATTLE_SCENE_X - mx * CTR_BATTLE_ZOOM, BATTLE_SCENE_Y - my * CTR_BATTLE_ZOOM,
         BATTLE_SCENE_X + (GBA_W + mx) * CTR_BATTLE_ZOOM, BATTLE_SCENE_Y + GBA_H * CTR_BATTLE_ZOOM, 0, 0, u1, v1,
         1, 1, 1, 1, 1, 1);
    BlendOver();
    HudDraw();
    {
        int down;

        if (BoxShown(&down))
        {
            BoxDraw(down);
            return;
        }
    }
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    Quad(sOverlayTex, false, QUAD_TEXTURE, (TOP_W - GBA_W) / 2, BATTLE_BOX_Y, (TOP_W + GBA_W) / 2, TOP_H,
         0, v1, (float)GBA_W / TOP_W, (float)(BATTLE_VIEW_H + GBA_H - BATTLE_BAND_TOP) / TOP_H, 1, 1, 1, 1, 1, 1);
}

/* The 2D battle laid out the same way, on black. */
static void ComposeBattle2D(void)
{
    HiResSurfaces();
    QuadTargetScaled(sCompositeFbo, TOP_W, TOP_H, sRenderScale);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    BattlePictureOver();
}

/* The battle's world, then its picture (in sTop) over it, into sComposite. */
static void ComposeBattleWorld(void)
{
    unsigned control = Reg(0x50), effect = (control >> 6) & 3;
    CtrVoxelBattleIntro intro;
    float bloom;

    /* The world is BG3: its brightness is BG3's. */
    CtrVoxel_SetBrightness((control & 0x08) ? Brightness() : 0.0f, 0.0f, effect == 2);
    CtrVoxel_BattleIntro(&intro);
    /* The landing glows: a burst of the bloom dying away with its flash. */
    bloom = WorldBase(intro.impact > 0.01f ? 0.7f * intro.impact : 0.0f);
    BattleIntroBlur(&intro);
    if (bloom > 0.005f)
        VoxelBloomCompose(bloom);
    if (!(Reg(0) & 128))
    {
        BlendOver();
        BattleWorldShadows();
        BattleIntroAtmosphere(&intro);
        if (intro.impact > 0.01f)
            Quad(0, false, QUAD_SOLID, 0, 0, TOP_W, TOP_H, 0, 0, 1, 1, 1.0f, 250 / 255.0f, 235 / 255.0f,
                 fminf(intro.impact, 1.0f) * 170.0f / 255.0f, 1, 1);
    }
    BattlePictureOver();
    BattleIntroBars(&intro);
}

/* The battle's view, GBA (-BATTLE_MARGIN_X, -BATTLE_MARGIN_Y) on, into sTop. */
static void BattleRender(void)
{
    sBattleSliding = CtrBattleIntro_Sliding() != 0;
    for (unsigned bg = 0; bg < 4; ++bg)
        sBattlePicture[bg] = BattlePictureLayer(bg);
    sBattleWide = true;
    /* The scene, all of the view, the text box left out ... */
    sBattleScene = true;
    sOrgX = -BATTLE_MARGIN_X;
    sOrgY = -BATTLE_MARGIN_Y;
    RenderPicture(BATTLE_VIEW_W, BATTLE_VIEW_H, 0, 0);
    sBattleScene = false;
    /* ... and the GBA's text box band, lines 112-160, under it in sTop. */
    sOrgX = 0;
    sOrgY = BATTLE_BAND_TOP;
    RenderPicture(GBA_W, GBA_H - BATTLE_BAND_TOP, 0, BATTLE_VIEW_H);
    sBattleWide = false;
    sOrgX = sOrgY = 0;
}

/*
 * Whether a stage background's map carries its art on past the picture, so
 * that wrapping it fills the margins with more of the scene: the tile
 * columns the margins would show hold about as much as the picture's own
 * edge columns (the title's clouds, the bike ride). Empty ones (a still
 * picture) are mirrored instead. The scenes whose hidden columns hold
 * unrelated tiles (Rayquaza's clouds) are letterboxed, and drawn without
 * margins (StageLetterbox).
 */
/*
 * Which backgrounds scroll sideways (3ds_video.c RecordScroll): a pan of more
 * than STILL_SPAN pixels within the last STILL_FRAMES frames, held for
 * SCROLL_HOLD_FRAMES after it stops; a new control register starts over. A
 * layer that scrolls is art made to wrap (the title's clouds), whatever its
 * hidden columns hold at the moment.
 */
#define STILL_FRAMES 32
/* Any pan: the title's clouds drift a pixel at a time, and the scenes that
 * shake (the legendaries') are letterboxed, drawn without margins. */
#define STILL_SPAN 0
#define SCROLL_HOLD_FRAMES 120
static uint16_t sScrollHistory[4][STILL_FRAMES];
static uint32_t sSceneKey[4], sMovedFrame[4], sScrollFrame;
static bool sScrolls[4];

static void RecordScroll(void)
{
    ++sScrollFrame;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        uint32_t key = Reg(8 + bg * 2) | ((uint32_t)(Reg(0) & 7) << 16) | ((uint32_t)((Reg(0) >> (8 + bg)) & 1) << 20);
        unsigned scroll = Reg(0x10 + bg * 4) & 511;
        int low = 0, high = 0;

        if (key != sSceneKey[bg])
        {
            sSceneKey[bg] = key;
            sScrolls[bg] = false;
            for (unsigned i = 0; i < STILL_FRAMES; ++i)
                sScrollHistory[bg][i] = (uint16_t)scroll;
        }
        sScrollHistory[bg][sScrollFrame % STILL_FRAMES] = (uint16_t)scroll;
        for (unsigned i = 0; i < STILL_FRAMES; ++i)
        {
            int delta = (((int)sScrollHistory[bg][i] - (int)scroll + 256) & 511) - 256;

            if (delta < low) low = delta;
            if (delta > high) high = delta;
        }
        if (high - low > STILL_SPAN)
        {
            sScrolls[bg] = true;
            sMovedFrame[bg] = sScrollFrame;
        }
        else if (sScrolls[bg] && sScrollFrame - sMovedFrame[bg] > SCROLL_HOLD_FRAMES)
            sScrolls[bg] = false;
    }
}

static bool StageLayerWraps(unsigned bg)
{
    static uint32_t used[1024 / 32];
    unsigned control = Reg(8 + bg * 2), size = control >> 14;
    unsigned map = ((control >> 8) & 31) * 0x800, chars = ((control >> 2) & 3) * 0x4000;
    bool color256 = (control & 128) != 0;
    unsigned cols = (size & 1) ? 64 : 32, rows = (size & 2) ? 64 : 32;
    unsigned c0 = (Reg(0x10 + bg * 4) & 511) >> 3, r0 = (Reg(0x12 + bg * 4) & 511) >> 3;
    unsigned edge = 0, beyond = 0, match = 0;
    static const int hidden[6] = {-3, -2, -1, 30, 31, 32};

    if ((Reg(0) & 7) != 0 && !((Reg(0) & 7) == 1 && bg < 2))
        return false;
    memset(used, 0, sizeof(used));
    for (unsigned r = 0; r < 20; ++r)
        for (unsigned c = 0; c < 30; ++c)
        {
            unsigned entry = BgMapEntry(map, size, (c0 + c) & (cols - 1), (r0 + r) & (rows - 1));
            bool shown = TileShown(chars, entry, color256);

            if (shown)
                used[(entry & 1023) >> 5] |= 1u << (entry & 31);
            if (shown && (c < 3 || c >= 27))
                ++edge;
        }
    for (unsigned r = 0; r < 20; ++r)
        for (unsigned i = 0; i < 6; ++i)
        {
            unsigned entry = BgMapEntry(map, size, (unsigned)((int)c0 + hidden[i]) & (cols - 1),
                                        (r0 + r) & (rows - 1));

            if (!TileShown(chars, entry, color256))
                continue;
            ++beyond;
            if (used[(entry & 1023) >> 5] & (1u << (entry & 31)))
                ++match;
        }
    (void)match;
    sStageLayerEmpty[bg] = edge == 0 && beyond == 0;
    if (sStageLayerEmpty[bg])
        return true; /* nothing either way */
    return sScrolls[bg] || beyond * 4 >= edge * 3;
}

/* pokenav_list.c, pokedex.c: the layer a PokéNav list scrolls on, and the
 * tile the Pokédex's screen on show carries out (3ds_video.c). */
int CtrPokenavList_Bg(void);
int CtrPokedex_Backdrop(void);

/* The 3DS's margin fills (3ds_video.c, sCentredFills), for its side margins:
 * above and below there are none, the picture filling the screen's height. */
static const StageFill sStageFills[CTR_CENTRED_SCREENS] =
{
    [CTR_CENTRED_MAIN_MENU] = {.layers = 1u << 1},
    /* Inside the yellow border; the title bar is its top row. */
    [CTR_CENTRED_NAMING] = {.layers = 1u << 3, .insetL = 2, .insetR = 2},
    [CTR_CENTRED_CLOCK] = {.layers = 1u << 3},
    /* The meadow round the bag. */
    [CTR_CENTRED_STARTER] = {.layers = 1u << 2},
    [CTR_CENTRED_POKENAV] = {.backmost = true},
    [CTR_CENTRED_STORAGE] = {.layers = 1u << 3, .wrap = true},
    [CTR_CENTRED_POKEDEX] = {.backmost = true},
    [CTR_CENTRED_POKEDEX_CAUGHT] = {.layers = 1u << 3, .tile = true, .tileColumn = 0, .tileRow = 2},
    [CTR_CENTRED_TRADE_MENU] = {.layers = (1u << 2) | (1u << 3), .wrap = true},
};

/* The backgrounds a backmost fill carries out: the visible one at the back
 * (3ds_video.c, CentredLayers). */
static uint8_t StageBackmost(void)
{
    unsigned display = Reg(0), best = 4, priority = 0;
    int list;

    if ((display & 7) > 2)
        return 0;
    list = CtrPokenavList_Bg();
    for (unsigned bg = 0; bg < 4; ++bg)
        if ((display & (0x100u << bg)) && (Reg(8 + bg * 2) & 3) >= priority && (int)bg != list
            && !((display & 7) == 1 && bg == 3) && !((display & 7) == 2 && bg < 2))
        {
            priority = Reg(8 + bg * 2) & 3;
            best = bg;
        }
    return best < 4 ? (uint8_t)(1u << best) : 0;
}

/* The Game Corner's backdrop at view pixel (x, y) of a stage render: the
 * pattern repeated from the view's corner. The felt under the whole
 * roulette; the wall beside the slot machine, the machine in the picture. */
static uint16_t CornerBackdrop(int x, int y, uint16_t backdrop)
{
    unsigned dim = CTR_CORNER_PATTERN_DIM, c;
    int lx = x + sOrgX;

    if (sCornerShown == CTR_CENTRED_SLOTS && lx >= 0 && lx < GBA_W)
        return backdrop;
    c = sCornerPattern[CtrVideo_Texel((unsigned)x % dim, (unsigned)y % dim, dim)];
    return (uint16_t)((c >> 11) | (((c >> 6) & 31) << 5) | ((c & 31) << 10));
}

static void CornerPrepare(void)
{
    static uint16_t trim[CTR_CORNER_TRIM_DIM * CTR_CORNER_TRIM_DIM];
    static unsigned made;
    unsigned screen = sStageMenu && (sCentred == CTR_CENTRED_ROULETTE || sCentred == CTR_CENTRED_SLOTS)
                    ? sCentred : CTR_CENTRED_NONE;

    sCornerShown = screen;
    if (screen == CTR_CENTRED_NONE || screen == made)
        return;
    /* Green at either table (roulette.c, bgColors[1]), as the 3DS. */
    if (screen == CTR_CENTRED_ROULETTE)
        CtrCornerArt_Felt(sCornerPattern, trim, 10 | 19 << 5 | 6 << 10);
    else
        CtrCornerArt_Wall(sCornerPattern, trim);
    made = screen;
}

/* Any centred screen on the top screen is magnified as a stage; this sets
 * how its margins are filled (sStageFill). False when none is on show. */
static bool StageCentredFill(void)
{
    int backdrop;

    if (sCentred == CTR_CENTRED_NONE || sCentred >= CTR_CENTRED_SCREENS)
        return false;
    sStageFill = sStageFills[sCentred];
    if (sCentred == CTR_CENTRED_POKEDEX && (backdrop = CtrPokedex_Backdrop()) >= 0)
    {
        sStageFill = (StageFill){.layers = (uint8_t)(1u << ((backdrop >> 16) & 3)), .tile = true,
                                 .tileColumn = (uint8_t)(backdrop >> 8), .tileRow = (uint8_t)backdrop};
        if (backdrop & (1 << 20))
            sStageFill = (StageFill){.layers = (uint8_t)(1u << ((backdrop >> 16) & 3)), .entry = true,
                                     .tileEntry = (uint16_t)backdrop};
    }
    else if (sStageFill.backmost)
        sStageFill.layers = StageBackmost();
    sStageAffineWrap = sCentred != CTR_CENTRED_BLENDER && sCentred != CTR_CENTRED_ROULETTE;
    return true;
}

/* A stage on screen (the intro, the title, the credits...): magnified to the
 * screen's height, its margins from its own picture (sStageWide). */
static bool StageLaidOut(void)
{
    /* A centred screen (the title menu, the professor's speech, the naming
     * screen, the clock, the Town Map...) is one GBA picture too: laid out
     * the same way, its margins filled as the 3DS fills them (sStageFill)
     * or left black - over a battle too, as the party menu opened in one. */
    if (StageCentredFill())
        return !sTransition && sVoxelReady;
    return sStage && !sBattle && !sTransition && sVoxelReady;
}

/* Once a stage frame: which sprites stand still outside the picture. */
static float sObjPace[128];

static void StageSprites(void)
{
    for (unsigned i = 0; i < 128; ++i)
    {
        const uint16_t *obj = sMem.oam + i * 4;
        unsigned a0 = obj[0], a1 = obj[1], shape = a0 >> 14;
        bool affine = (a0 >> 8) & 1, dbl = affine && ((a0 >> 9) & 1);
        int w, x = a1 & 511, dx;

        sObjParked[i] = false;
        sObjDrift[i] = 0;
        if ((!affine && ((a0 >> 9) & 1)) || shape == 3)
        {
            sObjStill[i] = 0;
            sObjPace[i] = 0.0f;
            continue;
        }
        w = sObjW[shape][a1 >> 14] * (dbl ? 2 : 1);
        if (x + STAGE_MARGIN_X >= STAGE_VIEW_W)
            x -= 512;
        dx = x - sObjLastX[i];
        if (dx != 0)
        {
            /* Its pace: the step over the frames it took (a slow sprite
             * steps every few frames); a jump is a new sprite in the slot. */
            sObjPace[i] = dx > -32 && dx < 32 ? (float)dx / (float)(sObjStill[i] + 1) : 0.0f;
            sObjStill[i] = 0;
        }
        else if (sObjStill[i] < 0xFFFF)
            ++sObjStill[i];
        sObjLastX[i] = (int16_t)x;
        if (!(x + w <= 0 || x >= GBA_W) || sObjStill[i] < 4)
            continue;
        /* Standing outside the picture: on off the screen the way it went,
         * or not drawn if it was not going that way. */
        if ((x + w <= 0 && sObjPace[i] < 0.0f) || (x >= GBA_W && sObjPace[i] > 0.0f))
        {
            float drift = sObjPace[i] * (float)sObjStill[i];

            if (drift < -400.0f || drift > 400.0f)
                sObjParked[i] = true;
            else
                sObjDrift[i] = (int16_t)drift;
        }
        else
            sObjParked[i] = true;
    }
}

/*
 * A stage shown through window 0 alone, nothing outside it (the legendaries'
 * scenes, letterboxed): magnified to the screen's width, the bars' outer
 * lines cut off; one whose window is open at an edge shows the window's
 * lines alone, covering the screen (3ds_video.c, the zoomed stage). No
 * margins to make up. False for any other stage.
 */
static float sStageScale;
static int sStageY0, sStageY1;

static bool StageLetterbox(void)
{
    unsigned v;
    int top, bottom;

    if ((Reg(0) & 0xE000) != 0x2000 || (Reg(0x4a) & 31))
        return false;
    v = Reg(0x44);
    top = (int)(v >> 8);
    bottom = (int)(v & 255);
    if (bottom > GBA_H || top > bottom) bottom = GBA_H;
    if (top == 0 && bottom == GBA_H)
        return false; /* a window over all of it: no bars */
    sStageScale = (float)TOP_W / GBA_W;
    sStageY0 = 0;
    sStageY1 = GBA_H;
    if ((top == 0) != (bottom == GBA_H) && bottom - top >= 16)
    {
        sStageY0 = top;
        sStageY1 = bottom;
        if ((float)TOP_H / (bottom - top) > sStageScale)
            sStageScale = (float)TOP_H / (bottom - top);
    }
    return true;
}

static void StageRender(void)
{
    bool leaves = sCentred == CTR_CENTRED_NONE && LeavesScene();

    sStageMenu = StageCentredFill();
    CornerPrepare();

    if (!sStageMenu && (leaves || StageLetterbox()))
    {
        if (leaves)
        {
            /* The width filled, a few rows of leaves and grass cut off. */
            sStageScale = (float)TOP_W / GBA_W;
            sStageY0 = 0;
            sStageY1 = GBA_H;
        }
        RenderPicture(GBA_W, GBA_H, 0, 0);
        return;
    }
    sStageScale = 0.0f;
    StageSprites();
    sStageAnyWraps = false;
    for (unsigned bg = 0; bg < 4; ++bg)
    {
        sStageWraps[bg] = StageLayerWraps(bg);
        if (sStageWraps[bg] && !sStageLayerEmpty[bg] && (Reg(0) & (0x100u << bg)))
            sStageAnyWraps = true;
    }
    sStageWide = true;
    sOrgX = -STAGE_MARGIN_X;
    sOrgY = 0;
    RenderPicture(STAGE_VIEW_W, GBA_H, 0, 0);
    sStageWide = false;
    sOrgX = 0;
}

static void ComposeStage(void)
{
    const float w = STAGE_VIEW_W * STAGE_ZOOM, x0 = (TOP_W - w) * 0.5f;

    if (sStageScale > 0.0f)
    {
        /* Letterboxed: the picture's rows [y0, y1) magnified and centred. */
        float k = sStageScale, h = (sStageY1 - sStageY0) * k, y = (TOP_H - h) * 0.5f, x = (TOP_W - GBA_W * k) * 0.5f;

        HiResSurfaces();
        QuadTargetScaled(sCompositeFbo, TOP_W, TOP_H, sRenderScale);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, sOverlayTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_W, TOP_H, GL_RGBA, GL_UNSIGNED_BYTE, sTop);
        glDisable(GL_BLEND);
        Quad(sOverlayTex, false, QUAD_TEXTURE, x, y, x + GBA_W * k, y + h, 0, (float)sStageY0 / TOP_H,
             (float)GBA_W / TOP_W, (float)sStageY1 / TOP_H, 1, 1, 1, 1, 1, 1);
        return;
    }
    HiResSurfaces();
    QuadTargetScaled(sCompositeFbo, TOP_W, TOP_H, sRenderScale);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_W, TOP_H, GL_RGBA, GL_UNSIGNED_BYTE, sTop);
    glDisable(GL_BLEND);
    Quad(sOverlayTex, false, QUAD_TEXTURE, x0, 0, x0 + w, TOP_H, 0, 0, (float)STAGE_VIEW_W / TOP_W,
         (float)GBA_H / TOP_H, 1, 1, 1, 1, 1, 1);
}

/* A battle on screen as itself: laid out as the 3DS lays it out. */
static bool BattleLaidOut(void)
{
    return sBattle && !sTransition && !sStage && sCentred == CTR_CENTRED_NONE;
}

/* The world as the battle's scenery this frame: updated whenever the option
 * is on, drawn while BG3 shows the scenery - a move's background on BG3 is
 * the 2D battle's, whole. */
static bool BattleWorldFrame(void)
{
    sBattleWorld = false;
    sWorldLayers = 0;
    if (!sVoxelReady)
        return false;
    if (!sBattle)
    {
        if (CtrVoxel_InBattle())
            CtrVoxel_EndBattle();
        return false;
    }
    if (!CtrSettings_Voxel() || !CtrSettings_VoxelBattle() || !CtrVoxel_IsAvailableForBattle())
        return false;
    bool sliding = CtrBattleIntro_Sliding() != 0, moveBg = CtrBattleBg_MoveBgShown() != 0;

    if (!CtrVoxel_InBattle())
        CtrVoxel_BeginBattle();
    /* The intro slides the scenery in line by line: no shake there. */
    CtrVoxel_SetSwoop(0.0f, 1.0f, true, true);
    CtrVoxel_SetStereo(0.0f);
    CtrVoxel_SetBattleFrame(sliding, sliding || moveBg ? 0.0f : BattleScenerySway(0x1c),
                            sliding || moveBg ? 0.0f : BattleScenerySway(0x1e));
    bool updated = CtrVoxel_Update();
    sBattleWorld = updated && !moveBg && (Reg(0) & 0x800) && !(Reg(0) & 128);
    if (sBattleWorld)
        sWorldLayers = (1u << 3) | (sliding && !VoxelBattle_IsLink() ? (1u << 1) | (1u << 2) : 0u);
    return sBattleWorld;
}

/*
 * Which renderer draws the field this frame, and the swoop between them
 * (3ds_video.c, CtrVideo_Present): the voxel world when its option is on and
 * the map has one, the 2D field otherwise, black while a map just entered is
 * still being built. True when the world is drawn.
 */
static bool VoxelFrame(bool *blankOut, float *fade2d)
{
    bool field = !sStage && sCentred == CTR_CENTRED_NONE && !sBattle && CtrGame_IsOverworld();
    bool voxelMap = sVoxelReady && field && CtrVoxel_IsAvailable();
    bool wantVoxel = voxelMap && CtrSettings_Voxel();
    VoxelSwoopFrame swoop;

    *blankOut = false;
    *fade2d = 0.0f;
    if (!sVoxelReady)
        return false;
    if (!VoxelSwoop_Active(&sSwoop))
    {
        if (sFieldMode == FIELD_MODE_VOXEL && field && !wantVoxel && voxelMap)
            VoxelSwoop_Begin(&sSwoop, false, true);
        else if (sFieldMode == FIELD_MODE_PIXEL && wantVoxel)
            VoxelSwoop_Begin(&sSwoop, true, true);
    }
    bool overworld = wantVoxel || (voxelMap && VoxelSwoop_ShowsVoxel(&sSwoop));
    VoxelSwoop_Frame(&sSwoop, &swoop);
    CtrVoxel_SetSwoop(swoop.camera, swoop.height, swoop.cards, swoop.shadows);
    sOverlayFieldObjs = !swoop.cards;
    CtrVoxel_SetStereo(0.0f);
    uint64_t updateStart = svcGetSystemTick();
    bool voxel = overworld && CtrVoxel_Update();
    LateSpan(LATE_UPDATE, updateStart);
    bool blank = overworld && !voxel && CtrVoxel_IsWarmingUp() && sFieldMode != FIELD_MODE_PIXEL;
    unsigned mode = !field ? FIELD_MODE_NONE : voxel ? FIELD_MODE_VOXEL : blank ? sFieldMode : FIELD_MODE_PIXEL;

    if (mode == FIELD_MODE_NONE)
        VoxelSwoop_Cancel(&sSwoop);
    else if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && sSwoop.frame == 0 && mode != FIELD_MODE_VOXEL)
        VoxelSwoop_Cancel(&sSwoop);
    else if (VoxelSwoop_ShowsVoxel(&sSwoop) && !voxel)
        VoxelSwoop_LoseVoxel(&sSwoop);
    else if (!VoxelSwoop_Active(&sSwoop) && sFieldMode == FIELD_MODE_VOXEL && mode == FIELD_MODE_PIXEL)
        VoxelSwoop_Begin(&sSwoop, false, false);
    sFieldMode = mode;
    if (voxel)
    {
        VoxelSwoop_Frame(&sSwoop, &swoop);
        *fade2d = swoop.fade2d;
    }
    *blankOut = blank;
    return voxel;
}

/* After the picture: the swoop on by the game's frames, held while the
 * world it rises into still has holes. */
static void VoxelSwoopStep(bool voxel)
{
    static unsigned held;
    static uint64_t lastGameFrame;
    uint64_t gameFrame = CtrPlatform_FrameCount();
    unsigned steps = lastGameFrame != 0 && gameFrame > lastGameFrame ? (unsigned)(gameFrame - lastGameFrame) : 1u;

    if (!VoxelSwoop_Active(&sSwoop))
        return;
    if (steps > VOXEL_SWOOP_STEPS_MAX) steps = VOXEL_SWOOP_STEPS_MAX;
    lastGameFrame = gameFrame;
    if (sSwoop.phase == VOXEL_SWOOP_FLASH_IN && voxel && CtrVoxel_GetStats()->chunksMissing != 0
        && held < VOXEL_SWOOP_HOLD_FRAMES)
        ++held;
    else
    {
        held = 0;
        while (steps-- != 0 && VoxelSwoop_Active(&sSwoop))
            VoxelSwoop_Step(&sSwoop);
    }
}

void CtrVideo_Present(void)
{
    static uint64_t sFpsStart;
    static uint32_t sFpsFrames;

    uint64_t frameStart = svcGetSystemTick();
    bool voxel = false, blank = false, composed = false, battleWorld = false, battle2d = false, stage = false;
    float fade2d = 0.0f;
    GLuint topTex = sTopTex;

    sHud = NULL;
    sBattleWorld = false;
    SyncWindows();
    /* The day/night light, sun and moonlight for this frame, as 3ds_video.c
     * does before its palette: the voxel world (CtrVoxel_Update, _Draw) only
     * reads them (CtrDayNight_Get, _GetSun). Without it they moved only when
     * something else asked for the clock, such as the bottom screen. */
    CtrDayNight_Update();
    if (sSurf[0] != EGL_NO_SURFACE)
        eglMakeCurrent(sDpy, sSurf[0], sSurf[0], sCtx);
    else if (sSurf[1] != EGL_NO_SURFACE)
        eglMakeCurrent(sDpy, sSurf[1], sSurf[1], sCtx);
    sBottomInUse = sMem.regs && !sStage && BottomWindowScreen(sCentred);
    if (sBottomInUse)
        RenderBottomWindow();
    /* The top screen keeps what it showed while the game's screen is below. */
    bool holdTop = sHoldTop || sBottomInUse;
    if (!holdTop && sMem.regs)
    {
        DetectPaletteFade();
        RecordScroll();
        voxel = VoxelFrame(&blank, &fade2d);
        sFieldVoxel = voxel;
        if (voxel)
        {
            /* The game's overlay into sTop, the 2D field for the crossfade
             * into sField2D; the world itself on the GPU (ComposeVoxel). */
            sLineRegCount = 0;
            sFieldUi = !sFieldBanner;
            sOverlay = true;
            RenderPicture(TOP_W, TOP_H, 0, 0);
            sOverlay = false;
            sFieldUi = false;
            if (fade2d > 0.004f)
            {
                sOut = sField2D;
                sExcludeLayers = 1;
                RenderPicture(TOP_W, TOP_H, 0, 0);
                sExcludeLayers = 0;
                sOut = sTop;
            }
        }
        else if (blank)
            for (size_t i = 0; i < TOP_W * TOP_H; i++)
                sTop[i] = 0xFF000000u;
        else if ((HudFrame(), BattleLaidOut()) && BattleWorldFrame())
        {
            /* The battle's picture with the world behind it (sWorldBehind):
             * transparent where BG3 was. */
            memset(sTop, 0, sizeof(sTop));
            sExcludeLayers = sWorldLayers;
            sWorldBehind = true;
            BattleRender();
            sWorldBehind = false;
            sExcludeLayers = 0;
            battleWorld = true;
        }
        else if (BattleLaidOut() && sVoxelReady)
        {
            memset(sTop, 0, sizeof(sTop));
            BattleRender();
            battle2d = true;
        }
        else if (StageLaidOut())
        {
            StageRender();
            stage = true;
        }
        else
        {
            sHud = NULL;
            RenderTop();
        }
    }
    if (!holdTop && sVoxelReady)
        BoxFrame();
    if (AndroidLog_FatalReason())
    {
        voxel = battleWorld = battle2d = stage = false;
        for (size_t i = 0; i < TOP_W * TOP_H; i++)
            sTop[i] = 0xFF2020A0u;
    }
    if (sSurf[0] != EGL_NO_SURFACE || sSurf[1] != EGL_NO_SURFACE)
    {
        VoxelSwoopFrame swoop;

        VoxelSwoop_Frame(&sSwoop, &swoop);
        if (voxel)
        {
            ComposeVoxel(fade2d);
            composed = true;
        }
        else if (battleWorld)
        {
            ComposeBattleWorld();
            composed = true;
        }
        else if (battle2d)
        {
            ComposeBattle2D();
            composed = true;
        }
        else if (stage)
        {
            ComposeStage();
            composed = true;
        }
        if (!holdTop && VoxelSwoop_Active(&sSwoop) && (swoop.flash > 0.005f || swoop.bloom > 0.005f) && sVoxelReady)
        {
            /* The swoop's light over whichever field is drawn. */
            if (!composed)
            {
                HiResSurfaces();
                QuadTargetScaled(sCompositeFbo, TOP_W, TOP_H, sRenderScale);
                glDisable(GL_BLEND);
                PictureOver(sTopTex, sTop, false, 1.0f);
                composed = true;
            }
            DrawSwoopLight(&swoop);
        }
        /* A held top screen (CtrVideo_HoldTop: a game screen open on the
         * bottom) keeps the picture it showed last: the composite, when
         * that was the voxel world or a laid-out battle - sTop then holds
         * only what was laid over it. */
        if (holdTop && sTopComposed && sVoxelReady)
            composed = true;
        else if (!holdTop)
            sTopComposed = composed;
        if (composed)
        {
            topTex = sCompositeTex;
            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        else
        {
            glBindTexture(GL_TEXTURE_2D, sTopTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_W, TOP_H, GL_RGBA, GL_UNSIGNED_BYTE, sTop);
        }
        if (sBottomDirty)
        {
            glBindTexture(GL_TEXTURE_2D, sBottomTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CTR_BOTTOM_HEIGHT, CTR_BOTTOM_WIDTH, GL_RGB,
                            GL_UNSIGNED_SHORT_5_6_5, sBottom);
            sBottomDirty = false;
        }
        if (AndroidHost_TakeCaptureRequest())
        {
            if (composed)
            {
                /* The picture as shown: read back from the composite. */
                CaptureComposite();
            }
            Capture();
        }
        uint64_t screenStart = svcGetSystemTick();
        /* Up to here: the update and the world's and screens' draws issued. */
        sLatePart[LATE_DRAW] = CtrPlatform_TickMs(screenStart - frameStart) - sLatePart[LATE_UPDATE];
        DrawScreen(HOST_SCREEN_TOP, topTex, TOP_W, TOP_H, 0);
        LateSpan(LATE_TOP, screenStart);
        screenStart = svcGetSystemTick();
        DrawScreen(HOST_SCREEN_BOTTOM, sBottomTex, CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, 1);
        LateSpan(LATE_BOTTOM, screenStart);
    }
    else if (AndroidHost_TakeCaptureRequest())
        Capture();
    if (!holdTop)
        VoxelSwoopStep(voxel);
    /* The voxel world builds what comes next in what is left of the frame. */
    if (sVoxelReady)
    {
        if (voxel || battleWorld)
        {
            uint64_t afterStart = svcGetSystemTick();
            CtrVoxel_SetFrameReserve(AndroidPlatform_FrameReserveMs());
            CtrVoxel_AfterSubmit(frameStart);
            LateSpan(LATE_AFTER, afterStart);
        }
        else
            CtrVoxel_Idle();
    }
    LateFrame(frameStart);
    sStats.frames++;
    sFpsFrames++;
#if CTR_SHOW_FPS
    UpdateOverlayFps();
#endif
    uint64_t now = CtrPlatform_Milliseconds();
    if (!sFpsStart)
        sFpsStart = now;
    else if (now - sFpsStart >= 5000)
    {
        sStats.fps = sFpsFrames * 1000.0f / (float)(now - sFpsStart);
        CtrLog_Write(CTR_LOG_VIDEO, "fps %.1f, game %.1f ms, wait %.1f ms", sStats.fps,
                     CtrPlatform_GetTiming()->gameMs, sStats.waitMs);
        LateLog();
        if (voxel)
        {
            const CtrVoxelStats *v = CtrVoxel_GetStats();
            unsigned draws, bytes, texels;

            AndroidC3D_FrameStats(&draws, &bytes, &texels);
            CtrLog_Write(CTR_LOG_VIDEO, "voxel: %u chunks (%u visible, %u missing), %u vertices, %u draws, "
                         "%u KiB streamed, update %.1f ms (peak %.1f), mesh %.1f ms",
                         v->chunks, v->visibleChunks, v->chunksMissing, v->vertices, draws, bytes >> 10,
                         v->updateMs, v->updatePeakMs, v->meshMs);
        }
        sFpsStart = now;
        sFpsFrames = 0;
    }
    if (sVoxelReady)
        CtrVoxel_NoteFrameWait(svcGetSystemTick());
    Pace();
}
