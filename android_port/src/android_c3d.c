/*
 * Citro3D on OpenGL ES 3: see include/citro3d.h for what is kept of the PICA.
 *
 * Everything runs on the render thread, the one with the EGL context
 * (android_video.c); the pools may be used from any thread.
 */
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "citro3d.h"
#include "3ds_log.h"

#define MAX_TEXTURES 1024
#define MAX_PROGRAMS 128
#define UNIFORMS 96
#define STREAM_BYTES (32u << 20)
#define INDEX_BYTES (4u << 20)

/* ── Memory pools ──────────────────────────────────────────────────────── */
/* Budgets far above the 3DS's (6 MiB of VRAM): the voxel module sizes its
 * caches by what is free, and on the Thor memory is not what limits it. */
#define LINEAR_BUDGET (96u << 20)
#define VRAM_BUDGET (48u << 20)
#define POOL_HEADER 128u
#define POOL_MAGIC 0xC3D0A110u

typedef struct
{
    u32 magic;
    u32 size;
    u32 pool;
} PoolHeader;

static pthread_mutex_t sPoolLock = PTHREAD_MUTEX_INITIALIZER;
static size_t sPoolUsed[2];
static void TexturesForget(const void *mem, size_t size);

static void *PoolAlloc(unsigned pool, size_t size, size_t align)
{
    void *raw = NULL;
    size_t budget = pool ? VRAM_BUDGET : LINEAR_BUDGET;

    if (align < POOL_HEADER)
        align = POOL_HEADER;
    pthread_mutex_lock(&sPoolLock);
    if (sPoolUsed[pool] + size > budget)
    {
        pthread_mutex_unlock(&sPoolLock);
        return NULL;
    }
    sPoolUsed[pool] += size;
    pthread_mutex_unlock(&sPoolLock);
    if (posix_memalign(&raw, align, size + align) != 0)
    {
        pthread_mutex_lock(&sPoolLock);
        sPoolUsed[pool] -= size;
        pthread_mutex_unlock(&sPoolLock);
        return NULL;
    }
    uint8_t *mem = (uint8_t *)raw + align;
    PoolHeader *h = (PoolHeader *)(mem - sizeof(PoolHeader));
    h->magic = POOL_MAGIC;
    h->size = (u32)size;
    h->pool = pool | ((u32)align << 1);
    /* Where the raw block starts, for free(). */
    ((void **)(mem - sizeof(PoolHeader)))[-1] = raw;
    return mem;
}

static void PoolFree(void *mem)
{
    if (mem == NULL)
        return;
    PoolHeader *h = (PoolHeader *)((uint8_t *)mem - sizeof(PoolHeader));
    if (h->magic != POOL_MAGIC)
        return;
    void *raw = ((void **)h)[-1];
    unsigned pool = h->pool & 1;
    size_t size = h->size;

    TexturesForget(mem, size);
    h->magic = 0;
    pthread_mutex_lock(&sPoolLock);
    sPoolUsed[pool] -= size;
    pthread_mutex_unlock(&sPoolLock);
    free(raw);
}

void *linearAlloc(size_t size) { return PoolAlloc(0, size, 0x80); }
void *linearMemAlign(size_t size, size_t alignment) { return PoolAlloc(0, size, alignment); }
void linearFree(void *mem) { PoolFree(mem); }
u32 linearSpaceFree(void) { return (u32)(LINEAR_BUDGET - sPoolUsed[0]); }
void *vramAlloc(size_t size) { return PoolAlloc(1, size, 0x80); }
void vramFree(void *mem) { PoolFree(mem); }
u32 vramSpaceFree(void) { return (u32)(VRAM_BUDGET - sPoolUsed[1]); }
uint32_t CtrGpuMem_LargestVram(void) { return vramSpaceFree(); }

/* ── Texture cache ─────────────────────────────────────────────────────── */
/*
 * A GL texture for each texture memory drawn from, keyed by its address and
 * shape. Writes are announced by address range (flushes and copies); what
 * they reach is detiled again, in whole rows of 8x8 blocks, at the next draw.
 */
typedef struct
{
    const uint8_t *data;
    size_t size;
    u16 width, height;
    GPU_TEXCOLOR fmt;
    GLuint gl;
    size_t dirtyLo, dirtyHi; /* bytes, dirtyHi > dirtyLo when written */
    bool target;             /* a render target's colour: the GPU owns it */
    u16 glWidth, glHeight;   /* the GL texture's size: a render target's may be larger */
    uint8_t *shadow;         /* linear-heap textures: the bytes last uploaded */
    bool used;
    u32 param;               /* the sampler state last applied */
    u32 lastUse;
} TexEntry;

static TexEntry sTex[MAX_TEXTURES];
static unsigned sTexCount;
static u32 sUseClock;
static pthread_mutex_t sTexLock = PTHREAD_MUTEX_INITIALIZER;
/* GL textures to delete on the render thread (a pool free may come from
 * another thread). */
static GLuint sTexGraveyard[MAX_TEXTURES];
static unsigned sTexGraveyardCount;
static unsigned sTexelsUploaded;

unsigned C3D_TexFmtBits(GPU_TEXCOLOR fmt)
{
    static const u8 bits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0};
    return bits[fmt & 15];
}

static void Bury(TexEntry *e)
{
    if (e->gl && sTexGraveyardCount < MAX_TEXTURES)
        sTexGraveyard[sTexGraveyardCount++] = e->gl;
    free(e->shadow);
    memset(e, 0, sizeof(*e));
}

static void TexturesForget(const void *mem, size_t size)
{
    const uint8_t *lo = (const uint8_t *)mem, *hi = lo + size;

    pthread_mutex_lock(&sTexLock);
    for (unsigned i = 0; i < sTexCount; ++i)
        if (sTex[i].used && sTex[i].data >= lo && sTex[i].data < hi)
            Bury(&sTex[i]);
    pthread_mutex_unlock(&sTexLock);
}

static void MarkWritten(const void *addr, size_t size)
{
    const uint8_t *lo = (const uint8_t *)addr, *hi = lo + size;

    if (size == 0)
        return;
    pthread_mutex_lock(&sTexLock);
    for (unsigned i = 0; i < sTexCount; ++i)
    {
        TexEntry *e = &sTex[i];
        if (!e->used || e->target || hi <= e->data || lo >= e->data + e->size)
            continue;
        size_t a = lo > e->data ? (size_t)(lo - e->data) : 0;
        size_t b = (size_t)((hi < e->data + e->size ? hi : e->data + e->size) - e->data);
        if (e->dirtyHi <= e->dirtyLo)
        {
            e->dirtyLo = a;
            e->dirtyHi = b;
        }
        else
        {
            if (a < e->dirtyLo) e->dirtyLo = a;
            if (b > e->dirtyHi) e->dirtyHi = b;
        }
    }
    pthread_mutex_unlock(&sTexLock);
}

Result GSPGPU_FlushDataCache(const void *addr, u32 size)
{
    MarkWritten(addr, size);
    return 0;
}

Result GSPGPU_InvalidateDataCache(const void *addr, u32 size)
{
    (void)addr;
    (void)size;
    return 0;
}

static TexEntry *FindEntry(const C3D_Tex *tex, bool create)
{
    TexEntry *free = NULL, *oldest = NULL;

    for (unsigned i = 0; i < sTexCount; ++i)
    {
        TexEntry *e = &sTex[i];
        if (!e->used)
        {
            if (!free) free = e;
            continue;
        }
        if (e->data == tex->data)
        {
            if (e->width == tex->width && e->height == tex->height && e->fmt == tex->fmt)
                return e;
            /* The same memory as another shape: the old one is gone. */
            Bury(e);
            if (!free) free = e;
            continue;
        }
        if (!e->target && (!oldest || e->lastUse < oldest->lastUse))
            oldest = e;
    }
    if (!create)
        return NULL;
    if (!free)
    {
        if (sTexCount < MAX_TEXTURES)
            free = &sTex[sTexCount++];
        else
        {
            free = oldest;
            Bury(free);
        }
    }
    memset(free, 0, sizeof(*free));
    free->used = true;
    free->data = (const uint8_t *)tex->data;
    free->width = tex->width;
    free->height = tex->height;
    free->fmt = tex->fmt;
    free->size = (size_t)tex->width * tex->height * C3D_TexFmtBits(tex->fmt) / 8;
    free->param = 0xFFFFFFFFu;
    {
        /* In the linear heap: written by the CPU without a flush of its own
         * (C3D_FrameEnd flushes the whole heap on the 3DS), so compared with
         * what was uploaded every frame (AndroidC3D_FlushLinear). */
        const PoolHeader *h = (const PoolHeader *)(free->data - sizeof(PoolHeader));
        if (h->magic == POOL_MAGIC && (h->pool & 1) == 0 && free->size)
            free->shadow = (uint8_t *)malloc(free->size);
    }
    return free;
}

typedef struct
{
    GLint internal;
    GLenum format, type;
    unsigned outBytes; /* per texel, uploaded */
    GLint swizzle[4];
} GlFormat;

static GlFormat FormatOf(GPU_TEXCOLOR fmt)
{
    switch (fmt)
    {
    case GPU_RGBA8: return (GlFormat){GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
    case GPU_RGB8: return (GlFormat){GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
    case GPU_RGBA5551: return (GlFormat){GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
    case GPU_RGB565: return (GlFormat){GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
    case GPU_RGBA4: return (GlFormat){GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
    case GPU_LA8:
    case GPU_LA4: return (GlFormat){GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, {GL_RED, GL_RED, GL_RED, GL_GREEN}};
    case GPU_HILO8: return (GlFormat){GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, {GL_RED, GL_GREEN, GL_ZERO, GL_ONE}};
    case GPU_L8:
    case GPU_L4: return (GlFormat){GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, {GL_RED, GL_RED, GL_RED, GL_ONE}};
    case GPU_A8:
    case GPU_A4: return (GlFormat){GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED}};
    default: return (GlFormat){GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
    }
}

/* The PICA's texel order inside an 8x8 block, (x, y) from the block's top. */
static inline unsigned Morton(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static uint8_t *sDetile;
static size_t sDetileSize;

/* Memory rows [y0, y1) of the texture, detiled and uploaded. The first row of
 * memory is the top of the picture (v = 1), the GL texture's last row. */
/*
 * What the GL context was last set to by the draws, so that a draw sets only
 * what differs from the one before. The voxel world makes some 15,000 draws a
 * frame in a town, nearly all alike, and setting all of their state again
 * took a whole frame's time there (port.log "late", 2026-11-13: draw 17 ms in
 * Rustboro). Forgotten wherever else GL state may change: a clear, a target
 * switch, AndroidC3D_ResetState after the world, and (only the textures) a
 * texture made or uploaded outside a draw.
 */
static struct
{
    bool valid;            /* the fixed-function state below is what GL has */
    bool texValid;         /* tex[] and activeUnit are */
    GLuint program, vao, arrayBuffer;
    GLint viewport[4];
    bool scissorOn;
    GLint scissor[4];
    u8 colorMask;
    bool depthTest, depthMask;
    GLenum depthFunc, cull;            /* cull 0: off */
    bool blend;
    GLenum blendEq[2], blendFactor[4];
    u32 blendColor;
    unsigned activeUnit;
    GLuint tex[3];
    bool constKnown[16];
    float constValue[16][4];
} sGl;
/* The VAO's own state, kept across everything but AndroidC3D_Init: only the
 * draws use their VAO. */
static u16 sAttribOn;

static void GlForget(void)
{
    memset(&sGl, 0, sizeof(sGl));
}

static void GlForgetTextures(void)
{
    sGl.texValid = false;
}

static void UploadRows(TexEntry *e, unsigned y0, unsigned y1)
{
    GlFormat f = FormatOf(e->fmt);
    unsigned w = e->width, h = e->height, rows = y1 - y0;
    unsigned bits = C3D_TexFmtBits(e->fmt);
    size_t need = (size_t)w * rows * f.outBytes;

    if (need > sDetileSize)
    {
        free(sDetile);
        sDetile = (uint8_t *)malloc(need);
        sDetileSize = sDetile ? need : 0;
        if (!sDetile)
            return;
    }
    for (unsigned y = y0; y < y1; ++y)
    {
        uint8_t *out = sDetile + (size_t)(y1 - 1 - y) * w * f.outBytes;
        size_t rowBlocks = (size_t)(y >> 3) * (w >> 3);
        unsigned py = y & 7;

        for (unsigned x = 0; x < w; ++x, out += f.outBytes)
        {
            size_t texel = (rowBlocks + (x >> 3)) * 64u + Morton(x & 7, py);
            if (bits == 4)
            {
                uint8_t b = e->data[texel >> 1];
                uint8_t v = (texel & 1) ? (b >> 4) : (b & 15);
                out[0] = (uint8_t)(v * 17);
                continue;
            }
            const uint8_t *in = e->data + texel * (bits / 8);
            switch (e->fmt)
            {
            case GPU_RGBA8: out[0] = in[3]; out[1] = in[2]; out[2] = in[1]; out[3] = in[0]; break;
            case GPU_RGB8: out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; break;
            case GPU_LA8: out[0] = in[1]; out[1] = in[0]; break;
            case GPU_HILO8: out[0] = in[1]; out[1] = in[0]; break;
            case GPU_LA4: out[0] = (uint8_t)((in[0] >> 4) * 17); out[1] = (uint8_t)((in[0] & 15) * 17); break;
            case GPU_L8:
            case GPU_A8: out[0] = in[0]; break;
            default: out[0] = in[0]; out[1] = in[1]; break; /* 16-bit, as the GL types take them */
            }
        }
    }
    GlForgetTextures();
    glBindTexture(GL_TEXTURE_2D, e->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)(h - y1), (GLsizei)w, (GLsizei)rows, f.format, f.type, sDetile);
    sTexelsUploaded += w * rows;
}

static void ApplyParam(TexEntry *e, u32 param)
{
    static const GLint wraps[4] = {GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};

    if (e->param == param)
        return;
    e->param = param;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (param & GPU_TEXTURE_MAG_FILTER(1)) ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (param & GPU_TEXTURE_MIN_FILTER(1)) ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[(param >> 12) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[(param >> 8) & 3]);
}

static void CreateGlSized(TexEntry *e, int width, int height)
{
    GlFormat f = FormatOf(e->fmt);

    e->glWidth = (u16)width;
    e->glHeight = (u16)height;
    glGenTextures(1, &e->gl);
    GlForgetTextures();
    glBindTexture(GL_TEXTURE_2D, e->gl);
    glTexImage2D(GL_TEXTURE_2D, 0, f.internal, width, height, 0, f.format, f.type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, f.swizzle[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, f.swizzle[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, f.swizzle[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, f.swizzle[3]);
}

static void CreateGl(TexEntry *e) { CreateGlSized(e, e->width, e->height); }

static void BuryPending(void)
{
    pthread_mutex_lock(&sTexLock);
    if (sTexGraveyardCount)
        glDeleteTextures((GLsizei)sTexGraveyardCount, sTexGraveyard);
    sTexGraveyardCount = 0;
    pthread_mutex_unlock(&sTexLock);
}

static void ActiveUnit(unsigned unit)
{
    if (!sGl.texValid || sGl.activeUnit != unit)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        if (!sGl.texValid)
        {
            memset(sGl.tex, 0, sizeof(sGl.tex));
            sGl.texValid = true;
        }
        sGl.activeUnit = unit;
    }
}

/* The GL texture of `tex`, current with what was written to it, bound to
 * texture unit `unit` with its sampler state (and that unit left active). */
static GLuint PrepareTextureOn(C3D_Tex *tex, unsigned unit)
{
    TexEntry *e;
    size_t lo, hi;

    if (!tex || !tex->data || !tex->width || !tex->height)
        return 0;
    ActiveUnit(unit);
    pthread_mutex_lock(&sTexLock);
    e = FindEntry(tex, true);
    e->lastUse = ++sUseClock;
    bool fresh = e->gl == 0;
    if (fresh)
        e->dirtyLo = 0, e->dirtyHi = e->size;
    lo = e->dirtyLo;
    hi = e->dirtyHi;
    e->dirtyLo = e->dirtyHi = 0;
    pthread_mutex_unlock(&sTexLock);
    if (fresh)
        CreateGl(e);
    if (fresh && e->shadow)
        memcpy(e->shadow, e->data, e->size);
    if (!e->target && hi > lo && e->width >= 8 && e->height >= 8)
    {
        size_t blockRow = (size_t)e->width * 8u * C3D_TexFmtBits(e->fmt) / 8u;
        unsigned y0 = (unsigned)(lo / blockRow) * 8u, y1 = (unsigned)((hi + blockRow - 1) / blockRow) * 8u;
        if (y1 > e->height) y1 = e->height;
        if (y1 > y0) UploadRows(e, y0, y1);
    }
    /* An upload above bound it, and forgot the units: on this one again. */
    ActiveUnit(unit);
    if (sGl.tex[unit] != e->gl)
    {
        glBindTexture(GL_TEXTURE_2D, e->gl);
        sGl.tex[unit] = e->gl;
    }
    ApplyParam(e, tex->param);
    return e->gl;
}

static GLuint PrepareTexture(C3D_Tex *tex)
{
    return PrepareTextureOn(tex, 0);
}

/* What C3D_FrameEnd's flush of the linear heap does for the GPU: every
 * linear-heap texture's block rows that changed since they were uploaded
 * are marked written. */
void AndroidC3D_FlushLinear(void)
{
    pthread_mutex_lock(&sTexLock);
    for (unsigned i = 0; i < sTexCount; ++i)
    {
        TexEntry *e = &sTex[i];
        if (!e->used || !e->shadow || !e->gl || e->width < 8)
            continue;
        size_t row = (size_t)e->width * 8u * C3D_TexFmtBits(e->fmt) / 8u;
        for (size_t at = 0; at + row <= e->size; at += row)
        {
            if (memcmp(e->shadow + at, e->data + at, row) == 0)
                continue;
            memcpy(e->shadow + at, e->data + at, row);
            if (e->dirtyHi <= e->dirtyLo)
                e->dirtyLo = at, e->dirtyHi = at + row;
            else
            {
                if (at < e->dirtyLo) e->dirtyLo = at;
                if (at + row > e->dirtyHi) e->dirtyHi = at + row;
            }
        }
    }
    pthread_mutex_unlock(&sTexLock);
}

unsigned AndroidC3D_GLTexture(C3D_Tex *tex)
{
    /* For the video's own draws, which bind textures of their own after. */
    GLuint gl = PrepareTexture(tex);

    GlForgetTextures();
    return gl;
}

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
    memset(tex, 0, sizeof(*tex));
    tex->size = (size_t)width * height * C3D_TexFmtBits(format) / 8;
    tex->data = linearAlloc(tex->size);
    if (!tex->data)
        return false;
    tex->fmt = format;
    tex->width = width;
    tex->height = height;
    tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D) | GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST);
    return true;
}

bool C3D_TexInitVRAM(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
    memset(tex, 0, sizeof(*tex));
    tex->size = (size_t)width * height * C3D_TexFmtBits(format) / 8;
    tex->data = vramAlloc(tex->size);
    if (!tex->data)
        return false;
    tex->fmt = format;
    tex->width = width;
    tex->height = height;
    tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D) | GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST);
    return true;
}

void C3D_TexDelete(C3D_Tex *tex)
{
    if (!tex->data)
        return;
    TexturesForget(tex->data, 1);
    PoolFree(tex->data);
    tex->data = NULL;
}

void C3D_TexFlush(C3D_Tex *tex)
{
    if (tex->data)
        MarkWritten(tex->data, tex->size);
}

void C3D_TexUpload(C3D_Tex *tex, const void *data)
{
    if (tex->data && data)
        memcpy(tex->data, data, tex->size);
    C3D_TexFlush(tex);
}

void C3D_SyncTextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags)
{
    GX_TextureCopy(inadr, indim, outadr, outdim, size, (u8)flags);
}

Result GX_TextureCopy(u32 *src, u32 srcDim, u32 *dst, u32 dstDim, u32 size, u8 flags)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst, *outStart = out;
    unsigned inLine = (srcDim & 0xFFFF) * 16u, inGap = (srcDim >> 16) * 16u;
    unsigned outLine = (dstDim & 0xFFFF) * 16u, outGap = (dstDim >> 16) * 16u;
    unsigned inLeft, outLeft;

    (void)flags;
    if (inLine == 0 || outLine == 0 || (inGap == 0 && outGap == 0 && inLine == outLine))
    {
        memmove(out, in, size);
        MarkWritten(out, size);
        return 0;
    }
    inLeft = inLine;
    outLeft = outLine;
    while (size)
    {
        unsigned n = inLeft < outLeft ? inLeft : outLeft;
        if (n > size) n = size;
        memmove(out, in, n);
        in += n;
        out += n;
        size -= n;
        if ((inLeft -= n) == 0) { in += inGap; inLeft = inLine; }
        if ((outLeft -= n) == 0) { out += outGap; outLeft = outLine; }
    }
    MarkWritten(outStart, (size_t)(out - outStart));
    return 0;
}

/* ── Maths ─────────────────────────────────────────────────────────────── */
void Mtx_Identity(C3D_Mtx *out)
{
    Mtx_Zeros(out);
    out->r[0].x = out->r[1].y = out->r[2].z = out->r[3].w = 1.0f;
}

void Mtx_Multiply(C3D_Mtx *out, const C3D_Mtx *a, const C3D_Mtx *b)
{
    C3D_Mtx t;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            /* column j of b: b->r[k].c[3 - j] is element (k, j) */
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a->r[i].c[3 - k] * b->r[k].c[3 - j];
            t.r[i].c[3 - j] = s;
        }
    *out = t;
}

C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx *mtx, C3D_FVec v)
{
    return FVec4_New(FVec4_Dot(mtx->r[0], v), FVec4_Dot(mtx->r[1], v), FVec4_Dot(mtx->r[2], v),
                     FVec4_Dot(mtx->r[3], v));
}

C3D_FVec Mtx_MultiplyFVec3(const C3D_Mtx *mtx, C3D_FVec v)
{
    return FVec3_New(FVec3_Dot(mtx->r[0], v), FVec3_Dot(mtx->r[1], v), FVec3_Dot(mtx->r[2], v));
}

void Mtx_Translate(C3D_Mtx *mtx, float x, float y, float z, bool bRightSide)
{
    C3D_FVec v = FVec4_New(x, y, z, 1.0f);

    if (bRightSide)
    {
        for (int i = 0; i < 4; ++i)
            mtx->r[i].w = FVec4_Dot(mtx->r[i], v);
    }
    else
    {
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 4; ++i)
                mtx->r[j].c[i] += mtx->r[3].c[i] * v.c[3 - j];
    }
}

void Mtx_Scale(C3D_Mtx *mtx, float x, float y, float z)
{
    for (int i = 0; i < 4; ++i)
    {
        mtx->r[i].x *= x;
        mtx->r[i].y *= y;
        mtx->r[i].z *= z;
    }
}

static void Rotate(C3D_Mtx *mtx, int axis, float angle, bool bRightSide)
{
    C3D_Mtx rot, out;
    float s = sinf(angle), c = cosf(angle);

    Mtx_Identity(&rot);
    if (axis == 0) { rot.r[1].y = c; rot.r[1].z = -s; rot.r[2].y = s; rot.r[2].z = c; }
    if (axis == 1) { rot.r[0].x = c; rot.r[0].z = s; rot.r[2].x = -s; rot.r[2].z = c; }
    if (axis == 2) { rot.r[0].x = c; rot.r[0].y = -s; rot.r[1].x = s; rot.r[1].y = c; }
    if (bRightSide)
        Mtx_Multiply(&out, mtx, &rot);
    else
        Mtx_Multiply(&out, &rot, mtx);
    *mtx = out;
}
void Mtx_RotateX(C3D_Mtx *mtx, float angle, bool bRightSide) { Rotate(mtx, 0, angle, bRightSide); }
void Mtx_RotateY(C3D_Mtx *mtx, float angle, bool bRightSide) { Rotate(mtx, 1, angle, bRightSide); }
void Mtx_RotateZ(C3D_Mtx *mtx, float angle, bool bRightSide) { Rotate(mtx, 2, angle, bRightSide); }

/* Clip z from 0 (far) to -w (near), as citro3d makes it for the PICA. */
void Mtx_Ortho(C3D_Mtx *mtx, float left, float right, float bottom, float top, float near, float far,
               bool isLeftHanded)
{
    Mtx_Zeros(mtx);
    mtx->r[0].x = 2.0f / (right - left);
    mtx->r[0].w = (left + right) / (left - right);
    mtx->r[1].y = 2.0f / (top - bottom);
    mtx->r[1].w = (bottom + top) / (bottom - top);
    mtx->r[2].z = isLeftHanded ? 1.0f / (far - near) : 1.0f / (near - far);
    mtx->r[2].w = 0.5f * (near + far) / (near - far) - 0.5f;
    mtx->r[3].w = 1.0f;
}

void Mtx_Persp(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, bool isLeftHanded)
{
    float fovyTan = tanf(fovy / 2.0f);

    Mtx_Zeros(mtx);
    mtx->r[0].x = 1.0f / (aspect * fovyTan);
    mtx->r[1].y = 1.0f / fovyTan;
    mtx->r[2].w = far * near / (near - far);
    mtx->r[3].z = isLeftHanded ? 1.0f : -1.0f;
    mtx->r[2].z = -mtx->r[3].z * near / (near - far);
}

void Mtx_PerspStereo(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, float iod,
                     float screen, bool isLeftHanded)
{
    float fovyTan = tanf(fovy / 2.0f), fovyTanAspect = fovyTan * aspect;
    float shift = iod / (2.0f * screen);

    Mtx_Zeros(mtx);
    mtx->r[0].x = 1.0f / fovyTanAspect;
    mtx->r[0].w = -iod / 2.0f;
    mtx->r[1].y = 1.0f / fovyTan;
    mtx->r[2].w = near * far / (near - far);
    mtx->r[3].z = isLeftHanded ? 1.0f : -1.0f;
    mtx->r[0].z = mtx->r[3].z * shift / fovyTanAspect;
    mtx->r[2].z = -mtx->r[3].z * near / (near - far);
}

void Mtx_LookAt(C3D_Mtx *out, C3D_FVec cameraPosition, C3D_FVec cameraTarget, C3D_FVec cameraUpVector,
                bool isLeftHanded)
{
    C3D_FVec x, y, z;

    if (isLeftHanded)
        z = FVec3_Normalize(FVec3_Subtract(cameraTarget, cameraPosition));
    else
        z = FVec3_Normalize(FVec3_Subtract(cameraPosition, cameraTarget));
    x = FVec3_Normalize(FVec3_Cross(cameraUpVector, z));
    y = FVec3_Cross(z, x);
    out->r[0] = FVec4_New(x.x, x.y, x.z, -FVec3_Dot(x, cameraPosition));
    out->r[1] = FVec4_New(y.x, y.y, y.z, -FVec3_Dot(y, cameraPosition));
    out->r[2] = FVec4_New(z.x, z.y, z.z, -FVec3_Dot(z, cameraPosition));
    out->r[3] = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
}

/* ── Vertex programs ───────────────────────────────────────────────────── */
/*
 * GLSL translations of the port's .v.pica programs. Uniforms are the PICA's
 * float registers c[], at the registers picasso gives them (declaration
 * order from c0); the outputs are the PICA's colour and three texture
 * coordinates. Pica() takes the PICA's clip z (0 to -w) and depth map to GL's.
 */
typedef struct
{
    const char *name;
    const char *uniforms[48]; /* name, then its register as text, pairs */
    const char *body;
} VertexProgram;

static const char kVertexPrologue[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 c[96];\n"
    "uniform vec2 uDepth;\n"
    "out vec4 vColor;\n"
    "out vec2 vTc0;\n"
    "out vec2 vTc1;\n"
    "out vec2 vTc2;\n"
    "vec4 Pica(vec4 p) { return vec4(p.x, p.y, 2.0 * (p.z * uDepth.x + uDepth.y * p.w) - p.w, p.w); }\n";

static const VertexProgram kPrograms[] = {
    {
        /* voxel.v.pica */
        "voxel",
        {"projection", "0", "modelView", "4", "origin", "8", "shadeTint", "9", "tintDiff", "10",
         "grade", "11", "fog", "12", "dappleU", "13", "dappleV", "14", NULL},
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "void main() {\n"
        "  vec4 r0 = vec4(v0.xyz * 0.001953125, 1.0);\n"
        "  vec4 r7 = c[8] + r0;\n"
        "  vec4 r1 = vec4(dot(c[4], r7), dot(c[5], r7), dot(c[6], r7), dot(c[7], r7));\n"
        "  gl_Position = Pica(vec4(dot(c[0], r1), dot(c[1], r1), dot(c[2], r1), dot(c[3], r1)));\n"
        "  vTc0 = v1.xy;\n"
        "  vTc2 = v1.xy;\n"
        "  vTc1 = vec2(dot(c[13], r0), dot(c[14], r0));\n"
        "  float shade = 0.00006103515625 * v0.w;\n"
        "  float t = min(max(c[11].x * shade + c[11].y, 0.0), 1.0);\n"
        "  vec4 r4 = shade * (c[9] + c[10] * t);\n"
        "  float haze = min(max(c[12].x * r1.z + c[12].y, 0.0), c[12].z);\n"
        "  vColor = vec4(r4.xyz, 1.0 - haze);\n"
        "}\n",
    },
    {
        /* voxel_weather.v.pica */
        "weather",
        {"projection", "0", "modelView", "4", "origin", "8", "shadeTint", "9", "tintDiff", "10",
         "grade", "11", "fog", "12", "dappleU", "13", "dappleV", "14", "boxOffset", "15",
         "boxSize", "16", "boxBase", "17", "across", "18", "along", "19", NULL},
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "void main() {\n"
        "  vec4 r0 = c[15] + 0.000030517578125 * v0;\n"
        "  r0 = r0 - floor(r0);\n"
        "  vec4 r2 = r0 - 0.5;\n"
        "  r2 = 0.5 - max(r2, -r2);\n"
        "  float f = min(c[16].w * r2.x, c[15].w * r2.z);\n"
        "  f = min(f, c[17].w * r2.y);\n"
        "  f = min(1.0, f);\n"
        "  vec4 r4 = c[17] + c[16] * r0;\n"
        "  vec4 r5 = 0.015625 * v1;\n"
        "  r4 = r4 + c[18] * r5.x + c[19] * r5.y;\n"
        "  r4.w = 1.0;\n"
        "  vec4 r1 = vec4(dot(c[4], r4), dot(c[5], r4), dot(c[6], r4), dot(c[7], r4));\n"
        "  gl_Position = Pica(vec4(dot(c[0], r1), dot(c[1], r1), dot(c[2], r1), dot(c[3], r1)));\n"
        "  float haze = 1.0 - min(max(c[12].x * r1.z + c[12].y, 0.0), c[12].z);\n"
        "  float lens = min(max(c[19].w * (-c[18].w - r1.z), 0.0), 1.0);\n"
        "  float a = 0.000030517578125 * v0.w * f * lens * haze;\n"
        "  vColor = vec4(a, a, a, haze);\n"
        "  vTc0 = vec2(0.0); vTc1 = vec2(0.0); vTc2 = vec2(0.0);\n"
        "}\n",
    },
};
#define PROGRAM_COUNT (sizeof(kPrograms) / sizeof(kPrograms[0]))

static DVLE_s sDvle[PROGRAM_COUNT];
static shaderInstance_s sInstances[8];
static unsigned sInstanceCount;

DVLB_s *DVLB_ParseFile(u32 *shbinData, u32 shbinSize)
{
    char text[256];
    DVLB_s *dvlb;
    unsigned n = 0;

    if (shbinSize >= sizeof(text) || shbinSize < 4)
        return NULL;
    memcpy(text, shbinData, shbinSize);
    text[shbinSize] = 0;
    if (strncmp(text, "GLES", 4) != 0)
        return NULL;
    dvlb = (DVLB_s *)calloc(1, sizeof(*dvlb));
    dvlb->DVLE = (DVLE_s *)calloc(PROGRAM_COUNT, sizeof(DVLE_s));
    for (char *line = strtok(text + 4, " \r\n"); line && n < PROGRAM_COUNT; line = strtok(NULL, " \r\n"))
        for (unsigned p = 0; p < PROGRAM_COUNT; ++p)
            if (strcmp(line, kPrograms[p].name) == 0)
                dvlb->DVLE[n++].program = (int)p;
    dvlb->numDVLE = n;
    return dvlb;
}

void DVLB_Free(DVLB_s *dvlb)
{
    if (!dvlb)
        return;
    free(dvlb->DVLE);
    free(dvlb);
}

Result shaderProgramInit(shaderProgram_s *sp)
{
    memset(sp, 0, sizeof(*sp));
    return 0;
}

Result shaderProgramFree(shaderProgram_s *sp)
{
    memset(sp, 0, sizeof(*sp));
    return 0;
}

Result shaderProgramSetVsh(shaderProgram_s *sp, DVLE_s *dvle)
{
    if (!dvle || dvle->program < 0 || (unsigned)dvle->program >= PROGRAM_COUNT)
        return -1;
    for (unsigned i = 0; i < sInstanceCount; ++i)
        if (sInstances[i].dvle->program == dvle->program)
        {
            sp->vertexShader = &sInstances[i];
            return 0;
        }
    if (sInstanceCount >= sizeof(sInstances) / sizeof(sInstances[0]))
        return -1;
    sDvle[dvle->program] = *dvle;
    sInstances[sInstanceCount].dvle = &sDvle[dvle->program];
    sp->vertexShader = &sInstances[sInstanceCount++];
    return 0;
}

s8 shaderInstanceGetUniformLocation(shaderInstance_s *si, const char *name)
{
    if (!si || !si->dvle)
        return -1;
    const VertexProgram *p = &kPrograms[si->dvle->program];
    for (unsigned i = 0; p->uniforms[i]; i += 2)
        if (strcmp(p->uniforms[i], name) == 0)
            return (s8)atoi(p->uniforms[i + 1]);
    return -1;
}

/* ── State ─────────────────────────────────────────────────────────────── */
/* citro3d's context, as far as code reaches into it (ctr_voxel.c's
 * ReleaseDappleUnit: the flags at 0x20, the bound textures at 0x118). */
uint8_t __C3D_Context[0x200] __attribute__((aligned(8)));
#define BOUND ((C3D_Tex **)(void *)(__C3D_Context + 0x118))

typedef struct
{
    C3D_TexEnv env[6];
    int bufUpdateRgb, bufUpdateAlpha;
    u32 bufColor;
    bool alphaTest;
    int alphaFunc, alphaRef;
    bool depthTest;
    int depthFunc;
    int writeMask;
    float depthScale, depthOffset;
    int cull;
    bool blend;
    int blendEq[2], blendFactor[4];
    u32 blendColor;
    int scissorMode;
    u32 scissor[4];
    u32 viewport[4];
    C3D_AttrInfo attr;
    C3D_BufInfo buf;
    C3D_FVec fixed[12];
    C3D_FVec unif[UNIFORMS];
    shaderProgram_s *program;
} C3DState;

static C3DState sState;

C3D_TexEnv *C3D_GetTexEnv(int id) { return &sState.env[id]; }
void C3D_SetTexEnv(int id, C3D_TexEnv *env) { sState.env[id] = *env; }
void C3D_DirtyTexEnv(C3D_TexEnv *env) { (void)env; }
void C3D_TexEnvBufUpdate(int mode, int mask)
{
    if (mode & C3D_RGB) sState.bufUpdateRgb = mask;
    if (mode & C3D_Alpha) sState.bufUpdateAlpha = mask;
}
void C3D_TexEnvBufColor(u32 color) { sState.bufColor = color; }

void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset)
{
    (void)bIsZBuffer;
    sState.depthScale = zScale;
    sState.depthOffset = zOffset;
}
void C3D_CullFace(GPU_CULLMODE mode) { sState.cull = mode; }
void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask)
{
    (void)enable; (void)function; (void)ref; (void)inputMask; (void)writeMask;
}
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass) { (void)sfail; (void)dfail; (void)pass; }
void C3D_BlendingColor(u32 color) { sState.blendColor = color; }
void C3D_EarlyDepthTest(bool enable, GPU_EARLYDEPTHFUNC function, u32 ref) { (void)enable; (void)function; (void)ref; }
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask)
{
    sState.depthTest = enable;
    sState.depthFunc = function;
    sState.writeMask = writemask;
}
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref)
{
    sState.alphaTest = enable;
    sState.alphaFunc = function;
    sState.alphaRef = ref;
}
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr,
                    GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha)
{
    sState.blend = true;
    sState.blendEq[0] = colorEq;
    sState.blendEq[1] = alphaEq;
    sState.blendFactor[0] = srcClr;
    sState.blendFactor[1] = dstClr;
    sState.blendFactor[2] = srcAlpha;
    sState.blendFactor[3] = dstAlpha;
}
void C3D_ColorLogicOp(GPU_LOGICOP op)
{
    (void)op; /* only COPY is used: blending off */
    sState.blend = false;
}

void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h)
{
    sState.viewport[0] = x;
    sState.viewport[1] = y;
    sState.viewport[2] = w;
    sState.viewport[3] = h;
}
void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom)
{
    sState.scissorMode = mode;
    sState.scissor[0] = left;
    sState.scissor[1] = top;
    sState.scissor[2] = right;
    sState.scissor[3] = bottom;
}

void AttrInfo_Init(C3D_AttrInfo *info) { memset(info, 0, sizeof(*info)); }
int AttrInfo_AddLoader(C3D_AttrInfo *info, int regId, GPU_FORMATS format, int count)
{
    if (info->attrCount >= 12)
        return -1;
    int id = info->attrCount++;
    info->reg[id] = (u8)regId;
    info->format[id] = (u8)format;
    info->count[id] = (u8)count;
    info->fixed[id] = false;
    return id;
}
int AttrInfo_AddFixed(C3D_AttrInfo *info, int regId)
{
    if (info->attrCount >= 12)
        return -1;
    int id = info->attrCount++;
    info->reg[id] = (u8)regId;
    info->fixed[id] = true;
    return id;
}
C3D_AttrInfo *C3D_GetAttrInfo(void) { return &sState.attr; }
void C3D_SetAttrInfo(C3D_AttrInfo *info) { sState.attr = *info; }
void BufInfo_Init(C3D_BufInfo *info) { memset(info, 0, sizeof(*info)); }
int BufInfo_Add(C3D_BufInfo *info, const void *data, ptrdiff_t stride, int attribCount, u64 permutation)
{
    if (info->bufCount >= 12)
        return -1;
    C3D_BufCfg *b = &info->buffers[info->bufCount];
    b->data = data;
    b->stride = (u32)stride;
    b->attribCount = (u32)attribCount;
    b->permutation = permutation;
    return info->bufCount++;
}
C3D_BufInfo *C3D_GetBufInfo(void) { return &sState.buf; }
void C3D_SetBufInfo(C3D_BufInfo *info) { sState.buf = *info; }
/* Indexed by input register, as AttrInfo_AddFixed's callers set them. */
C3D_FVec *C3D_FixedAttribGetWritePtr(int id) { return &sState.fixed[(unsigned)id % 12u]; }

C3D_FVec *C3D_FVUnifWritePtr(GPU_SHADER_TYPE type, int id, int size)
{
    (void)type;
    (void)size;
    if (id < 0 || id >= UNIFORMS)
        id = 0;
    return &sState.unif[id];
}

void C3D_BindProgram(shaderProgram_s *program) { sState.program = program; }

void C3D_TexBind(int unitId, C3D_Tex *tex)
{
    if (unitId >= 0 && unitId < 3)
        BOUND[unitId] = tex;
}

/* ── Render targets ────────────────────────────────────────────────────── */
struct C3D_RenderTarget_tag
{
    GLuint fbo, color, depth;
    int width, height;
    C3D_Tex *tex;
    const void *texData;
};

static C3D_RenderTarget *sDrawOn;

static GLuint DepthBuffer(int width, int height, int depthFmt)
{
    GLuint rb = 0;

    if (depthFmt < 0)
        return 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    return rb;
}

C3D_RenderTarget *C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, int depthFmt)
{
    C3D_RenderTarget *t = (C3D_RenderTarget *)calloc(1, sizeof(*t));
    GlForget(); /* binds a framebuffer, its renderbuffers or texture */

    (void)colorFmt;
    t->width = width;
    t->height = height;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glGenRenderbuffers(1, &t->color);
    glBindRenderbuffer(GL_RENDERBUFFER, t->color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->color);
    t->depth = DepthBuffer(width, height, depthFmt);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
}

C3D_RenderTarget *C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level, int depthFmt)
{
    (void)face;
    (void)level;
    return AndroidC3D_RenderTargetScaled(tex, depthFmt, 1.0f);
}

C3D_RenderTarget *AndroidC3D_RenderTargetScaled(C3D_Tex *tex, int depthFmt, float scale)
{
    C3D_RenderTarget *t;
    TexEntry *e;
    GlForget(); /* binds a framebuffer, its renderbuffers or texture */

    if (!tex || !tex->data)
        return NULL;
    t = (C3D_RenderTarget *)calloc(1, sizeof(*t));
    /* The target is the texture's picture at `scale` times its size: the
     * viewport covers it whole, as on the PICA, so every draw lands on the
     * same fraction of it and sampling by texture coordinate is unchanged. */
    t->width = (int)(tex->width * scale + 0.5f);
    t->height = (int)(tex->height * scale + 0.5f);
    t->tex = tex;
    t->texData = tex->data;
    pthread_mutex_lock(&sTexLock);
    e = FindEntry(tex, true);
    e->target = true;
    pthread_mutex_unlock(&sTexLock);
    if (e->gl && (e->glWidth != t->width || e->glHeight != t->height))
    {
        glDeleteTextures(1, &e->gl);
        e->gl = 0;
    }
    if (!e->gl)
        CreateGlSized(e, t->width, t->height);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->gl, 0);
    t->depth = DepthBuffer(t->width, t->height, depthFmt);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        CtrLog_Write(CTR_LOG_ERROR, "C3D: render target %dx%d fmt %d incomplete", t->width, t->height, tex->fmt);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget *target)
{
    if (!target)
        return;
    if (sDrawOn == target)
        sDrawOn = NULL;
    glDeleteFramebuffers(1, &target->fbo);
    if (target->color) glDeleteRenderbuffers(1, &target->color);
    if (target->depth) glDeleteRenderbuffers(1, &target->depth);
    free(target);
}

unsigned AndroidC3D_TargetFramebuffer(C3D_RenderTarget *target) { return target ? target->fbo : 0; }

void C3D_RenderTargetClear(C3D_RenderTarget *target, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth)
{
    GLbitfield bits = 0;

    if (!target)
        return;
    GlForget();
    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    glDisable(GL_SCISSOR_TEST);
    if (clearBits & C3D_CLEAR_COLOR)
    {
        /* 0xRRGGBBAA */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((clearColor >> 24) / 255.0f, ((clearColor >> 16) & 255) / 255.0f,
                     ((clearColor >> 8) & 255) / 255.0f, (clearColor & 255) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((clearBits & C3D_CLEAR_DEPTH) && target->depth)
    {
        glDepthMask(GL_TRUE);
        glClearDepthf((clearDepth & 0xFFFFFF) / 16777215.0f);
        bits |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    }
    if (bits)
        glClear(bits);
    if (sDrawOn)
        glBindFramebuffer(GL_FRAMEBUFFER, sDrawOn->fbo);
}

bool C3D_FrameDrawOn(C3D_RenderTarget *target)
{
    if (!target)
        return false;
    sDrawOn = target;
    GlForget();
    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    C3D_SetViewport(0, 0, (u32)target->width, (u32)target->height);
    return true;
}

/* ── Fragment programs: the texture environment ────────────────────────── */
typedef struct
{
    u16 src[6][2], op[6][2], func[6][2], scale[6][2];
    u8 bufRgb, bufAlpha;
    u8 alphaTest, alphaFunc;
    u8 textures; /* units with a texture bound */
    u8 pad[3];
} FragKey;

typedef struct
{
    int vertex;
    FragKey key;
    GLuint program;
    GLint uC, uDepth, uConst, uBufColor, uAlphaRef, uTex[3];
    /* Its uniforms as last uploaded (a program keeps them): only what
     * changed is sent again. */
    bool uploaded;
    float unif[96][4], depth[2], consts[6][4], buf[4], alphaRef;
} Program;

static Program sPrograms[MAX_PROGRAMS];
static unsigned sProgramCount, sProgramNext;
static GLuint sVertexShaders[PROGRAM_COUNT];

static const char *SrcName(unsigned src, char *tmp, int stage)
{
    switch (src)
    {
    case GPU_PRIMARY_COLOR: return "prim";
    case GPU_TEXTURE0: return "t0";
    case GPU_TEXTURE1: return "t1";
    case GPU_TEXTURE2: return "t2";
    case GPU_PREVIOUS_BUFFER: return "buf";
    case GPU_CONSTANT: snprintf(tmp, 24, "uConst[%d]", stage); return tmp;
    case GPU_PREVIOUS: return "prev";
    default: return "vec4(0.0)";
    }
}

static void RgbOperand(char *out, size_t n, const char *s, unsigned op)
{
    switch (op)
    {
    case 1: snprintf(out, n, "(1.0 - %s.rgb)", s); break;
    case 2: snprintf(out, n, "vec3(%s.a)", s); break;
    case 3: snprintf(out, n, "vec3(1.0 - %s.a)", s); break;
    case 4: snprintf(out, n, "vec3(%s.r)", s); break;
    case 5: snprintf(out, n, "vec3(1.0 - %s.r)", s); break;
    case 8: snprintf(out, n, "vec3(%s.g)", s); break;
    case 9: snprintf(out, n, "vec3(1.0 - %s.g)", s); break;
    case 12: snprintf(out, n, "vec3(%s.b)", s); break;
    case 13: snprintf(out, n, "vec3(1.0 - %s.b)", s); break;
    default: snprintf(out, n, "%s.rgb", s); break;
    }
}

static void AlphaOperand(char *out, size_t n, const char *s, unsigned op)
{
    static const char *const comp[4] = {"a", "r", "g", "b"};
    if (op & 1)
        snprintf(out, n, "(1.0 - %s.%s)", s, comp[(op >> 1) & 3]);
    else
        snprintf(out, n, "%s.%s", s, comp[(op >> 1) & 3]);
}

static void Combine(char *out, size_t n, unsigned func, const char *a, const char *b, const char *c, bool rgb)
{
    switch (func)
    {
    case GPU_MODULATE: snprintf(out, n, "%s * %s", a, b); break;
    case GPU_ADD: snprintf(out, n, "%s + %s", a, b); break;
    case GPU_ADD_SIGNED: snprintf(out, n, "%s + %s - 0.5", a, b); break;
    case GPU_INTERPOLATE: snprintf(out, n, "mix(%s, %s, %s)", b, a, c); break;
    case GPU_SUBTRACT: snprintf(out, n, "%s - %s", a, b); break;
    case GPU_DOT3_RGB:
    case GPU_DOT3_RGBA:
        if (rgb) snprintf(out, n, "vec3(4.0 * dot(%s - 0.5, %s - 0.5))", a, b);
        else snprintf(out, n, "%s", a);
        break;
    case GPU_MULTIPLY_ADD: snprintf(out, n, "%s * %s + %s", a, b, c); break;
    case GPU_ADD_MULTIPLY:
        snprintf(out, n, rgb ? "min(%s + %s, vec3(1.0)) * %s" : "min(%s + %s, 1.0) * %s", a, b, c);
        break;
    default: snprintf(out, n, "%s", a); break;
    }
}

static char *AppendF(char *at, char *end, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static char *AppendF(char *at, char *end, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(at, (size_t)(end - at), fmt, ap);
    va_end(ap);
    if (n < 0) return at;
    return at + n < end ? at + n : end - 1;
}

static void FragmentSource(const FragKey *k, char *src, size_t size)
{
    static const float scales[4] = {1.0f, 2.0f, 4.0f, 1.0f};
    char *at = src, *end = src + size;

    at = AppendF(at, end,
                 "#version 300 es\n"
                 "precision highp float;\n"
                 "in vec4 vColor;\n"
                 "in vec2 vTc0;\n"
                 "in vec2 vTc1;\n"
                 "in vec2 vTc2;\n"
                 "uniform sampler2D uTex0, uTex1, uTex2;\n"
                 "uniform vec4 uConst[6];\n"
                 "uniform vec4 uBufColor;\n"
                 "uniform float uAlphaRef;\n"
                 "out vec4 oColor;\n"
                 "void main() {\n"
                 "  vec4 prim = clamp(vColor, 0.0, 1.0);\n");
    for (int u = 0; u < 3; ++u)
    {
        if (k->textures & (1 << u))
            at = AppendF(at, end, "  vec4 t%d = texture(uTex%d, vTc%d);\n", u, u, u);
        else
            at = AppendF(at, end, "  vec4 t%d = vec4(0.0);\n", u);
    }
    at = AppendF(at, end, "  vec4 prev = vec4(0.0);\n  vec4 buf = vec4(0.0);\n  vec4 nextBuf = uBufColor;\n");
    for (int s = 0; s < 6; ++s)
    {
        char names[3][24], tmp[3][24], a[3][48], rgb[200], alpha[200];
        bool passthrough = k->src[s][0] == GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0) && k->src[s][1] == GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0)
                           && k->func[s][0] == GPU_REPLACE && k->func[s][1] == GPU_REPLACE
                           && (k->op[s][0] & 15) == 0 && (k->op[s][1] & 15) == 0
                           && k->scale[s][0] == 0 && k->scale[s][1] == 0;

        if (!passthrough)
        {
            for (int i = 0; i < 3; ++i)
            {
                const char *n = SrcName((k->src[s][0] >> (4 * i)) & 15, tmp[i], s);
                snprintf(names[i], sizeof(names[i]), "%s", n);
                RgbOperand(a[i], sizeof(a[i]), names[i], (k->op[s][0] >> (4 * i)) & 15);
            }
            Combine(rgb, sizeof(rgb), k->func[s][0], a[0], a[1], a[2], true);
            for (int i = 0; i < 3; ++i)
            {
                const char *n = SrcName((k->src[s][1] >> (4 * i)) & 15, tmp[i], s);
                snprintf(names[i], sizeof(names[i]), "%s", n);
                AlphaOperand(a[i], sizeof(a[i]), names[i], (k->op[s][1] >> (4 * i)) & 15);
            }
            Combine(alpha, sizeof(alpha), k->func[s][1], a[0], a[1], a[2], false);
            at = AppendF(at, end, "  {\n    vec3 c = clamp((%s) * %.1f, 0.0, 1.0);\n", rgb, scales[k->scale[s][0] & 3]);
            if (k->func[s][0] == GPU_DOT3_RGBA)
                at = AppendF(at, end, "    float a = c.r;\n");
            else
                at = AppendF(at, end, "    float a = clamp((%s) * %.1f, 0.0, 1.0);\n", alpha, scales[k->scale[s][1] & 3]);
            at = AppendF(at, end, "    prev = vec4(c, a);\n  }\n");
        }
        /* The combiner buffer: what stage s+1 reads, as the PICA updates it. */
        if (s < 5)
        {
            at = AppendF(at, end, "  buf = nextBuf;\n");
            if (s < 4 && (k->bufRgb & (1 << s)))
                at = AppendF(at, end, "  nextBuf.rgb = prev.rgb;\n");
            if (s < 4 && (k->bufAlpha & (1 << s)))
                at = AppendF(at, end, "  nextBuf.a = prev.a;\n");
        }
    }
    if (k->alphaTest)
    {
        static const char *const tests[8] = {"true", "false", "a != r", "a == r", "a >= r", "a > r", "a <= r", "a < r"};
        at = AppendF(at, end, "  { int a = int(prev.a * 255.0 + 0.5), r = int(uAlphaRef);\n    if (%s) discard; }\n",
                     tests[k->alphaFunc & 7]);
    }
    at = AppendF(at, end, "  oColor = prev;\n}\n");
}

static GLuint Compile(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        CtrLog_Write(CTR_LOG_ERROR, "C3D: shader does not compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint VertexShader(int index)
{
    if (!sVertexShaders[index])
    {
        static char source[4096];
        snprintf(source, sizeof(source), "%s%s", kVertexPrologue, kPrograms[index].body);
        sVertexShaders[index] = Compile(GL_VERTEX_SHADER, source);
    }
    return sVertexShaders[index];
}

static Program *ProgramFor(int vertex, const FragKey *key)
{
    static char source[16384];
    Program *p;
    GLuint fs, prog;
    GLint ok = 0;

    for (unsigned i = 0; i < sProgramCount; ++i)
        if (sPrograms[i].vertex == vertex && memcmp(&sPrograms[i].key, key, sizeof(*key)) == 0)
            return &sPrograms[i];
    FragmentSource(key, source, sizeof(source));
    fs = Compile(GL_FRAGMENT_SHADER, source);
    if (!fs || !VertexShader(vertex))
        return NULL;
    prog = glCreateProgram();
    glAttachShader(prog, VertexShader(vertex));
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        CtrLog_Write(CTR_LOG_ERROR, "C3D: program does not link: %s", log);
        glDeleteProgram(prog);
        return NULL;
    }
    if (sProgramCount < MAX_PROGRAMS)
        p = &sPrograms[sProgramCount++];
    else
    {
        p = &sPrograms[sProgramNext];
        sProgramNext = (sProgramNext + 1) % MAX_PROGRAMS;
        glDeleteProgram(p->program);
    }
    p->vertex = vertex;
    p->key = *key;
    p->program = prog;
    p->uploaded = false;
    sGl.program = 0; /* linking used it below: glUseProgram */
    p->uC = glGetUniformLocation(prog, "c");
    p->uDepth = glGetUniformLocation(prog, "uDepth");
    p->uConst = glGetUniformLocation(prog, "uConst");
    p->uBufColor = glGetUniformLocation(prog, "uBufColor");
    p->uAlphaRef = glGetUniformLocation(prog, "uAlphaRef");
    glUseProgram(prog);
    for (int u = 0; u < 3; ++u)
    {
        char name[8] = {'u', 'T', 'e', 'x', (char)('0' + u), 0};
        p->uTex[u] = glGetUniformLocation(prog, name);
        if (p->uTex[u] >= 0)
            glUniform1i(p->uTex[u], u);
    }
    CtrLog_Write(CTR_LOG_VIDEO, "C3D: program %u made (%s)", sProgramCount, kPrograms[vertex].name);
    return p;
}

/* ── Drawing ───────────────────────────────────────────────────────────── */
static GLuint sVao, sStream, sIndexStream;
static size_t sStreamAt, sIndexAt;
static unsigned sDraws, sStreamBytes;

/*
 * The streams as rings mapped once (GL_EXT_buffer_storage), where the
 * drivers have it: mapping a buffer for every draw, as below, cost as much as
 * the draw itself. A ring is four segments, each fenced as it is left and
 * waited for when it comes round again, so nothing the GPU has yet to read is
 * written over (a lap is some 30 frames of a town).
 */
#define RING_SEGMENTS 4
typedef struct
{
    GLuint buffer;
    uint8_t *base;
    size_t capacity, at;
    unsigned segment;
    GLsync fence[RING_SEGMENTS];
} Ring;
static Ring sVertexRing, sIndexRing;
static PFNGLBUFFERSTORAGEEXTPROC sBufferStorage;

static bool RingInit(Ring *r, GLenum target, GLuint buffer, size_t capacity)
{
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;

    memset(r, 0, sizeof(*r));
    if (!sBufferStorage)
        return false;
    glBindBuffer(target, buffer);
    sBufferStorage(target, (GLsizeiptr)capacity, NULL, flags);
    r->base = glMapBufferRange(target, 0, (GLsizeiptr)capacity, flags);
    if (!r->base || glGetError() != GL_NO_ERROR)
    {
        r->base = NULL;
        return false;
    }
    r->buffer = buffer;
    r->capacity = capacity;
    return true;
}

static void RingLeave(Ring *r, unsigned segment)
{
    if (r->fence[segment])
        glDeleteSync(r->fence[segment]);
    r->fence[segment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

static void RingEnter(Ring *r, unsigned segment)
{
    if (!r->fence[segment])
        return;
    glClientWaitSync(r->fence[segment], GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull);
    glDeleteSync(r->fence[segment]);
    r->fence[segment] = 0;
}

static void *RingAlloc(Ring *r, size_t bytes, size_t *offset)
{
    size_t size = r->capacity / RING_SEGMENTS;
    size_t at = (r->at + 63) & ~(size_t)63;
    unsigned last;

    if (bytes > size)
        return NULL;
    if (at + bytes > r->capacity)
    {
        RingLeave(r, r->segment);
        at = 0;
        r->segment = 0;
        RingEnter(r, 0);
    }
    last = (unsigned)((at + bytes - 1) / size);
    while (r->segment < last)
    {
        RingLeave(r, r->segment);
        RingEnter(r, ++r->segment);
    }
    *offset = at;
    r->at = at + bytes;
    return r->base + at;
}

static void *StreamAlloc(GLenum target, GLuint buffer, size_t *at, size_t capacity, size_t bytes, size_t *offset)
{
    glBindBuffer(target, buffer);
    *at = (*at + 63) & ~(size_t)63;
    if (*at + bytes > capacity)
    {
        glBufferData(target, (GLsizeiptr)capacity, NULL, GL_STREAM_DRAW);
        *at = 0;
    }
    *offset = *at;
    void *p = glMapBufferRange(target, (GLintptr)*at, (GLsizeiptr)bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    *at += bytes;
    return p;
}

static GLenum GlBlendFactor(int f)
{
    static const GLenum factors[15] = {
        GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR,
        GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE};
    return f >= 0 && f < 15 ? factors[f] : GL_ONE;
}

static GLenum GlEquation(int e)
{
    static const GLenum eqs[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    return e >= 0 && e < 5 ? eqs[e] : GL_FUNC_ADD;
}

static GLenum GlTest(int f)
{
    static const GLenum tests[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return tests[f & 7];
}

static void Unpack(u32 abgr, float *out)
{
    out[0] = (abgr & 255) / 255.0f;
    out[1] = ((abgr >> 8) & 255) / 255.0f;
    out[2] = ((abgr >> 16) & 255) / 255.0f;
    out[3] = (abgr >> 24) / 255.0f;
}

/* GL state, the program and the textures for a draw: false when it cannot be made. */
static bool PrepareDraw(void)
{
    FragKey key;
    Program *p;
    float consts[6][4], buf[4], unif[UNIFORMS][4];

    if (!sDrawOn || !sState.program || !sState.program->vertexShader)
        return false;
    BuryPending();
    memset(&key, 0, sizeof(key));
    for (int s = 0; s < 6; ++s)
    {
        const C3D_TexEnv *e = &sState.env[s];
        key.src[s][0] = e->srcRgb;
        key.src[s][1] = e->srcAlpha;
        key.op[s][0] = e->opRgb;
        key.op[s][1] = e->opAlpha;
        key.func[s][0] = e->funcRgb;
        key.func[s][1] = e->funcAlpha;
        key.scale[s][0] = e->scaleRgb;
        key.scale[s][1] = e->scaleAlpha;
        Unpack(e->color, consts[s]);
    }
    key.bufRgb = (u8)sState.bufUpdateRgb;
    key.bufAlpha = (u8)sState.bufUpdateAlpha;
    key.alphaTest = sState.alphaTest;
    key.alphaFunc = sState.alphaTest ? (u8)sState.alphaFunc : 0;
    for (int u = 0; u < 3; ++u)
        if (BOUND[u] && BOUND[u]->data)
            key.textures |= (u8)(1 << u);
    {
        /* The draw before's program, most of the time. */
        static Program *last;
        int vertex = sState.program->vertexShader->dvle->program;

        if (last != NULL && last->vertex == vertex && memcmp(&last->key, &key, sizeof(key)) == 0)
            p = last;
        else
            p = last = ProgramFor(vertex, &key);
    }
    if (!p)
        return false;
    if (sGl.program != p->program)
    {
        glUseProgram(p->program);
        sGl.program = p->program;
    }
    for (int i = 0; i < UNIFORMS; ++i)
    {
        const C3D_FVec *v = &sState.unif[i];
        unif[i][0] = v->x;
        unif[i][1] = v->y;
        unif[i][2] = v->z;
        unif[i][3] = v->w;
    }
    if (!p->uploaded || memcmp(p->unif, unif, sizeof(unif)) != 0)
    {
        glUniform4fv(p->uC, UNIFORMS, &unif[0][0]);
        memcpy(p->unif, unif, sizeof(unif));
    }
    if (!p->uploaded || p->depth[0] != sState.depthScale || p->depth[1] != sState.depthOffset)
    {
        glUniform2f(p->uDepth, sState.depthScale, sState.depthOffset);
        p->depth[0] = sState.depthScale;
        p->depth[1] = sState.depthOffset;
    }
    if (p->uConst >= 0 && (!p->uploaded || memcmp(p->consts, consts, sizeof(consts)) != 0))
    {
        glUniform4fv(p->uConst, 6, &consts[0][0]);
        memcpy(p->consts, consts, sizeof(consts));
    }
    Unpack(sState.bufColor, buf);
    if (p->uBufColor >= 0 && (!p->uploaded || memcmp(p->buf, buf, sizeof(buf)) != 0))
    {
        glUniform4fv(p->uBufColor, 1, buf);
        memcpy(p->buf, buf, sizeof(buf));
    }
    if (p->uAlphaRef >= 0 && (!p->uploaded || p->alphaRef != (float)sState.alphaRef))
    {
        glUniform1f(p->uAlphaRef, (float)sState.alphaRef);
        p->alphaRef = (float)sState.alphaRef;
    }
    p->uploaded = true;
    for (int u = 0; u < 3; ++u)
        if (key.textures & (1 << u))
            PrepareTextureOn(BOUND[u], (unsigned)u);
    /* Unit 0 left active, as the video's own draws expect. */
    ActiveUnit(0);

    {
        GLint vp[4] = {(GLint)sState.viewport[0], (GLint)sState.viewport[1], (GLint)sState.viewport[2],
                       (GLint)sState.viewport[3]};
        if (!sGl.valid || memcmp(sGl.viewport, vp, sizeof(vp)) != 0)
        {
            glViewport(vp[0], vp[1], vp[2], vp[3]);
            memcpy(sGl.viewport, vp, sizeof(vp));
        }
    }
    if (sState.scissorMode == GPU_SCISSOR_NORMAL)
    {
        /* The PICA's scissor is in the framebuffer's own rows, as GL's. */
        GLint sc[4] = {(GLint)sState.scissor[0], (GLint)sState.scissor[1],
                       (GLint)(sState.scissor[2] - sState.scissor[0]), (GLint)(sState.scissor[3] - sState.scissor[1])};
        if (!sGl.valid || !sGl.scissorOn)
            glEnable(GL_SCISSOR_TEST);
        if (!sGl.valid || !sGl.scissorOn || memcmp(sGl.scissor, sc, sizeof(sc)) != 0)
        {
            glScissor(sc[0], sc[1], sc[2], sc[3]);
            memcpy(sGl.scissor, sc, sizeof(sc));
        }
        sGl.scissorOn = true;
    }
    else if (!sGl.valid || sGl.scissorOn)
    {
        glDisable(GL_SCISSOR_TEST);
        sGl.scissorOn = false;
    }
    if (!sGl.valid || sGl.colorMask != (u8)(sState.writeMask & 15))
    {
        glColorMask((sState.writeMask & 1) != 0, (sState.writeMask & 2) != 0, (sState.writeMask & 4) != 0,
                    (sState.writeMask & 8) != 0);
        sGl.colorMask = (u8)(sState.writeMask & 15);
    }
    if (sState.depthTest)
    {
        GLenum func = GlTest(sState.depthFunc);
        bool mask = (sState.writeMask & GPU_WRITE_DEPTH) != 0;

        if (!sGl.valid || !sGl.depthTest)
            glEnable(GL_DEPTH_TEST);
        if (!sGl.valid || !sGl.depthTest || sGl.depthFunc != func)
            glDepthFunc(func);
        if (!sGl.valid || !sGl.depthTest || sGl.depthMask != mask)
            glDepthMask(mask);
        sGl.depthTest = true;
        sGl.depthFunc = func;
        sGl.depthMask = mask;
    }
    else if (!sGl.valid || sGl.depthTest)
    {
        glDisable(GL_DEPTH_TEST);
        sGl.depthTest = false;
    }
    {
        GLenum cull = sState.cull == GPU_CULL_NONE ? 0 : sState.cull == GPU_CULL_FRONT_CCW ? GL_FRONT : GL_BACK;

        if (!sGl.valid || sGl.cull != cull)
        {
            if (cull == 0)
                glDisable(GL_CULL_FACE);
            else
            {
                if (!sGl.valid || sGl.cull == 0)
                {
                    glEnable(GL_CULL_FACE);
                    glFrontFace(GL_CCW);
                }
                glCullFace(cull);
            }
            sGl.cull = cull;
        }
    }
    if (sState.blend)
    {
        GLenum eq[2] = {GlEquation(sState.blendEq[0]), GlEquation(sState.blendEq[1])};
        GLenum f[4] = {GlBlendFactor(sState.blendFactor[0]), GlBlendFactor(sState.blendFactor[1]),
                       GlBlendFactor(sState.blendFactor[2]), GlBlendFactor(sState.blendFactor[3])};
        bool was = sGl.valid && sGl.blend;

        if (!was)
            glEnable(GL_BLEND);
        if (!was || memcmp(sGl.blendEq, eq, sizeof(eq)) != 0)
        {
            glBlendEquationSeparate(eq[0], eq[1]);
            memcpy(sGl.blendEq, eq, sizeof(eq));
        }
        if (!was || memcmp(sGl.blendFactor, f, sizeof(f)) != 0)
        {
            glBlendFuncSeparate(f[0], f[1], f[2], f[3]);
            memcpy(sGl.blendFactor, f, sizeof(f));
        }
        if (!was || sGl.blendColor != sState.blendColor)
        {
            float bc[4];
            Unpack(sState.blendColor, bc);
            glBlendColor(bc[0], bc[1], bc[2], bc[3]);
            sGl.blendColor = sState.blendColor;
        }
        sGl.blend = true;
    }
    else if (!sGl.valid || sGl.blend)
    {
        glDisable(GL_BLEND);
        sGl.blend = false;
    }
    sGl.valid = true;
    return true;
}

static unsigned FormatBytes(int format)
{
    static const u8 bytes[4] = {1, 1, 2, 4};
    return bytes[format & 3];
}

static GLenum FormatGl(int format)
{
    static const GLenum types[4] = {GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT};
    return types[format & 3];
}

/* Vertices [first, first + count) of every buffer to the stream, and the
 * attributes pointed at them. */
static bool BindVertices(unsigned first, unsigned count)
{
    const C3D_AttrInfo *attr = &sState.attr;
    u16 on = 0;

    if (sGl.vao != sVao)
    {
        glBindVertexArray(sVao);
        sGl.vao = sVao;
    }
    for (int b = 0; b < sState.buf.bufCount; ++b)
    {
        const C3D_BufCfg *cfg = &sState.buf.buffers[b];
        size_t offset = 0, bytes = (size_t)count * cfg->stride, at;
        void *dst;

        if (!cfg->data || bytes == 0)
            continue;
        if (sGl.arrayBuffer != sStream)
        {
            glBindBuffer(GL_ARRAY_BUFFER, sStream);
            sGl.arrayBuffer = sStream;
        }
        if (sVertexRing.base)
        {
            dst = RingAlloc(&sVertexRing, bytes, &at);
            if (!dst)
                return false;
            memcpy(dst, (const uint8_t *)cfg->data + (size_t)first * cfg->stride, bytes);
        }
        else
        {
            dst = StreamAlloc(GL_ARRAY_BUFFER, sStream, &sStreamAt, STREAM_BYTES, bytes, &at);
            if (!dst)
                return false;
            memcpy(dst, (const uint8_t *)cfg->data + (size_t)first * cfg->stride, bytes);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        sStreamBytes += (unsigned)bytes;
        for (u32 i = 0; i < cfg->attribCount; ++i)
        {
            unsigned code = (unsigned)(cfg->permutation >> (4 * i)) & 15;
            if (code >= 0xC)
            {
                offset += (code - 0xB) * 4u;
                continue;
            }
            if ((int)code >= attr->attrCount || attr->fixed[code])
                continue;
            unsigned elem = FormatBytes(attr->format[code]);
            offset = (offset + elem - 1) & ~(size_t)(elem - 1);
            unsigned reg = attr->reg[code];
            if (!(sAttribOn & (1u << reg)))
                glEnableVertexAttribArray(reg);
            glVertexAttribPointer(reg, attr->count[code], FormatGl(attr->format[code]), GL_FALSE,
                                  (GLsizei)cfg->stride, (const void *)(at + offset));
            on |= (u16)(1u << reg);
            offset += (size_t)elem * attr->count[code];
        }
    }
    for (unsigned reg = 0; reg < 12; ++reg)
        if (!(on & (1u << reg)))
        {
            static const float zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};

            if (sAttribOn & (1u << reg))
                glDisableVertexAttribArray(reg);
            if (!sGl.constKnown[reg] || memcmp(sGl.constValue[reg], zero, sizeof(zero)) != 0)
            {
                glVertexAttrib4f(reg, 0.0f, 0.0f, 0.0f, 1.0f);
                memcpy(sGl.constValue[reg], zero, sizeof(zero));
                sGl.constKnown[reg] = true;
            }
        }
    sAttribOn = on;
    for (int i = 0; i < attr->attrCount; ++i)
        if (attr->fixed[i])
        {
            unsigned reg = attr->reg[i] % 12u;
            const C3D_FVec *v = &sState.fixed[reg];
            float value[4] = {v->x, v->y, v->z, v->w};

            if (!sGl.constKnown[reg] || memcmp(sGl.constValue[reg], value, sizeof(value)) != 0)
            {
                glVertexAttrib4f(attr->reg[i], v->x, v->y, v->z, v->w);
                memcpy(sGl.constValue[reg], value, sizeof(value));
                sGl.constKnown[reg] = true;
            }
        }
    return true;
}

static GLenum Primitive(GPU_Primitive_t p)
{
    switch (p)
    {
    case GPU_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case GPU_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
    if (size <= 0 || !PrepareDraw() || !BindVertices((unsigned)first, (unsigned)size))
        return;
    glDrawArrays(Primitive(primitive), 0, size);
    ++sDraws;
}

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices)
{
    unsigned last = 0, bytes = (unsigned)count * (type == C3D_UNSIGNED_SHORT ? 2u : 1u);
    size_t at;
    void *dst;

    if (count <= 0 || !indices)
        return;
    for (int i = 0; i < count; ++i)
    {
        unsigned v = type == C3D_UNSIGNED_SHORT ? ((const u16 *)indices)[i] : ((const u8 *)indices)[i];
        if (v > last) last = v;
    }
    if (!PrepareDraw() || !BindVertices(0, last + 1))
        return;
    if (sIndexRing.base)
    {
        /* The VAO keeps its index buffer: bound once, at AndroidC3D_Init. */
        dst = RingAlloc(&sIndexRing, bytes, &at);
        if (!dst)
            return;
        memcpy(dst, indices, bytes);
    }
    else
    {
        dst = StreamAlloc(GL_ELEMENT_ARRAY_BUFFER, sIndexStream, &sIndexAt, INDEX_BYTES, bytes, &at);
        if (!dst)
            return;
        memcpy(dst, indices, bytes);
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    }
    glDrawElements(Primitive(primitive), count, type == C3D_UNSIGNED_SHORT ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                   (const void *)at);
    ++sDraws;
}

/* ── Lifetime ──────────────────────────────────────────────────────────── */
bool AndroidC3D_Init(void)
{
    memset(&sState, 0, sizeof(sState));
    for (int s = 0; s < 6; ++s)
        C3D_TexEnvInit(&sState.env[s]);
    sState.depthScale = -1.0f;
    sState.depthOffset = 0.0f;
    sState.writeMask = GPU_WRITE_ALL;
    sState.depthFunc = GPU_GREATER;
    GlForget();
    sAttribOn = 0;
    {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        sBufferStorage = ext && strstr(ext, "GL_EXT_buffer_storage")
                       ? (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT") : NULL;
    }
    glGenVertexArrays(1, &sVao);
    glBindVertexArray(sVao);
    glGenBuffers(1, &sStream);
    glGenBuffers(1, &sIndexStream);
    if (!RingInit(&sVertexRing, GL_ARRAY_BUFFER, sStream, STREAM_BYTES))
    {
        glBindBuffer(GL_ARRAY_BUFFER, sStream);
        glBufferData(GL_ARRAY_BUFFER, STREAM_BYTES, NULL, GL_STREAM_DRAW);
    }
    /* The index buffer is the VAO's: bound with it bound. */
    if (!RingInit(&sIndexRing, GL_ELEMENT_ARRAY_BUFFER, sIndexStream, INDEX_BYTES))
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIndexStream);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BYTES, NULL, GL_STREAM_DRAW);
    }
    glBindVertexArray(0);
    sStreamAt = sIndexAt = 0;
    CtrLog_Write(CTR_LOG_VIDEO, "C3D: vertex streams %s", sVertexRing.base && sIndexRing.base
                 ? "mapped once (EXT_buffer_storage)" : "mapped per draw");
    return glGetError() == GL_NO_ERROR;
}

void AndroidC3D_Shutdown(void)
{
    for (unsigned i = 0; i < RING_SEGMENTS; ++i)
    {
        if (sVertexRing.fence[i]) glDeleteSync(sVertexRing.fence[i]);
        if (sIndexRing.fence[i]) glDeleteSync(sIndexRing.fence[i]);
    }
    memset(&sVertexRing, 0, sizeof(sVertexRing));
    memset(&sIndexRing, 0, sizeof(sIndexRing));
    GlForget();
    for (unsigned i = 0; i < sProgramCount; ++i)
        glDeleteProgram(sPrograms[i].program);
    sProgramCount = sProgramNext = 0;
    for (unsigned i = 0; i < PROGRAM_COUNT; ++i)
        if (sVertexShaders[i])
            glDeleteShader(sVertexShaders[i]);
    memset(sVertexShaders, 0, sizeof(sVertexShaders));
    glDeleteBuffers(1, &sStream);
    glDeleteBuffers(1, &sIndexStream);
    glDeleteVertexArrays(1, &sVao);
}

void AndroidC3D_ResetState(void)
{
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    sDrawOn = NULL;
    GlForget();
}

void AndroidC3D_FrameStats(unsigned *draws, unsigned *vertexBytes, unsigned *texelUploads)
{
    *draws = sDraws;
    *vertexBytes = sStreamBytes;
    *texelUploads = sTexelsUploaded;
    sDraws = sStreamBytes = sTexelsUploaded = 0;
}
