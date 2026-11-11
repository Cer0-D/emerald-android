/*
 * Citro3D on OpenGL ES 3 (android_c3d.c): the part of citro3d's API the 3DS
 * GPU code uses (ctr_voxel.c), with the PICA200's behaviour kept where the
 * callers depend on it:
 *
 * - textures keep the PICA's tiled memory layout in a CPU buffer (8x8 Morton
 *   blocks, the first row of memory the top of the picture, v = 1). What the
 *   GPU would have read is detiled into a GL texture when a texture is next
 *   drawn with, for the rows written since: a write is announced as the 3DS
 *   announces it to the GPU (GSPGPU_FlushDataCache, C3D_TexFlush,
 *   C3D_SyncTextureCopy, GX_TextureCopy);
 * - vertex buffers are plain memory read when a draw is made, through the
 *   attribute loaders and buffer permutations as the PICA reads them;
 * - the texture environment (six combiner stages) and the alpha test become a
 *   generated fragment shader; the vertex shaders are GLSL translations of the
 *   port's .v.pica programs, chosen by name (DVLB_ParseFile); uniforms are the
 *   PICA's float registers;
 * - clip space is the PICA's (z from 0 to -w) and depth its mapping
 *   (C3D_DepthMap): the shaders convert, so depth tests read the same.
 *
 * Types and maths follow citro3d 1.7 (C3D_FVec stores w, z, y, x).
 */
#ifndef ANDROID_SHIM_CITRO3D_H
#define ANDROID_SHIM_CITRO3D_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "3ds.h"

/* ── GPU enums (gpu/enums.h) ───────────────────────────────────────────── */
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_CLAMP_TO_EDGE = 0, GPU_CLAMP_TO_BORDER = 1, GPU_REPEAT = 2, GPU_MIRRORED_REPEAT = 3 } GPU_TEXTURE_WRAP_PARAM;
typedef enum { GPU_TEX_2D = 0, GPU_TEX_CUBE_MAP = 1 } GPU_TEXTURE_MODE_PARAM;
typedef enum { GPU_TEXFACE_2D = 0 } GPU_TEXFACE;
typedef enum { GPU_TEXUNIT0 = 1, GPU_TEXUNIT1 = 2, GPU_TEXUNIT2 = 4 } GPU_TEXUNIT;
#define GPU_TEXTURE_MAG_FILTER(v) (((v) & 1) << 1)
#define GPU_TEXTURE_MIN_FILTER(v) (((v) & 1) << 2)
#define GPU_TEXTURE_MIP_FILTER(v) (((v) & 1) << 24)
#define GPU_TEXTURE_WRAP_S(v) (((v) & 3) << 12)
#define GPU_TEXTURE_WRAP_T(v) (((v) & 3) << 8)
#define GPU_TEXTURE_MODE(v) (((v) & 7) << 28)

typedef enum
{
    GPU_RGBA8 = 0x0, GPU_RGB8 = 0x1, GPU_RGBA5551 = 0x2, GPU_RGB565 = 0x3, GPU_RGBA4 = 0x4,
    GPU_LA8 = 0x5, GPU_HILO8 = 0x6, GPU_L8 = 0x7, GPU_A8 = 0x8, GPU_LA4 = 0x9, GPU_L4 = 0xA,
    GPU_A4 = 0xB, GPU_ETC1 = 0xC, GPU_ETC1A4 = 0xD,
} GPU_TEXCOLOR;

typedef enum { GPU_RB_RGBA8 = 0, GPU_RB_RGB8 = 1, GPU_RB_RGBA5551 = 2, GPU_RB_RGB565 = 3, GPU_RB_RGBA4 = 4 } GPU_COLORBUF;
typedef enum { GPU_RB_DEPTH16 = 0, GPU_RB_DEPTH24 = 2, GPU_RB_DEPTH24_STENCIL8 = 3 } GPU_DEPTHBUF;

typedef enum
{
    GPU_NEVER = 0, GPU_ALWAYS = 1, GPU_EQUAL = 2, GPU_NOTEQUAL = 3,
    GPU_LESS = 4, GPU_LEQUAL = 5, GPU_GREATER = 6, GPU_GEQUAL = 7,
} GPU_TESTFUNC;

typedef enum { GPU_EARLYDEPTH_GEQUAL = 0 } GPU_EARLYDEPTHFUNC;
typedef enum { GPU_SCISSOR_DISABLE = 0, GPU_SCISSOR_INVERT = 1, GPU_SCISSOR_NORMAL = 3 } GPU_SCISSORMODE;
typedef enum
{
    GPU_STENCIL_KEEP = 0, GPU_STENCIL_ZERO = 1, GPU_STENCIL_REPLACE = 2, GPU_STENCIL_INCR = 3,
    GPU_STENCIL_DECR = 4, GPU_STENCIL_INVERT = 5, GPU_STENCIL_INCR_WRAP = 6, GPU_STENCIL_DECR_WRAP = 7,
} GPU_STENCILOP;

typedef enum
{
    GPU_WRITE_RED = 0x01, GPU_WRITE_GREEN = 0x02, GPU_WRITE_BLUE = 0x04, GPU_WRITE_ALPHA = 0x08,
    GPU_WRITE_DEPTH = 0x10, GPU_WRITE_COLOR = 0x0F, GPU_WRITE_ALL = 0x1F,
} GPU_WRITEMASK;

typedef enum
{
    GPU_BLEND_ADD = 0, GPU_BLEND_SUBTRACT = 1, GPU_BLEND_REVERSE_SUBTRACT = 2,
    GPU_BLEND_MIN = 3, GPU_BLEND_MAX = 4,
} GPU_BLENDEQUATION;

typedef enum
{
    GPU_ZERO = 0, GPU_ONE = 1, GPU_SRC_COLOR = 2, GPU_ONE_MINUS_SRC_COLOR = 3,
    GPU_DST_COLOR = 4, GPU_ONE_MINUS_DST_COLOR = 5, GPU_SRC_ALPHA = 6, GPU_ONE_MINUS_SRC_ALPHA = 7,
    GPU_DST_ALPHA = 8, GPU_ONE_MINUS_DST_ALPHA = 9, GPU_CONSTANT_COLOR = 10,
    GPU_ONE_MINUS_CONSTANT_COLOR = 11, GPU_CONSTANT_ALPHA = 12, GPU_ONE_MINUS_CONSTANT_ALPHA = 13,
    GPU_SRC_ALPHA_SATURATE = 14,
} GPU_BLENDFACTOR;

typedef enum
{
    GPU_LOGICOP_CLEAR = 0, GPU_LOGICOP_AND = 1, GPU_LOGICOP_AND_REVERSE = 2, GPU_LOGICOP_COPY = 3,
    GPU_LOGICOP_SET = 4, GPU_LOGICOP_COPY_INVERTED = 5, GPU_LOGICOP_NOOP = 6, GPU_LOGICOP_INVERT = 7,
} GPU_LOGICOP;

typedef enum { GPU_BYTE = 0, GPU_UNSIGNED_BYTE = 1, GPU_SHORT = 2, GPU_FLOAT = 3 } GPU_FORMATS;
typedef enum { GPU_CULL_NONE = 0, GPU_CULL_FRONT_CCW = 1, GPU_CULL_BACK_CCW = 2 } GPU_CULLMODE;

typedef enum
{
    GPU_PRIMARY_COLOR = 0x00, GPU_FRAGMENT_PRIMARY_COLOR = 0x01, GPU_FRAGMENT_SECONDARY_COLOR = 0x02,
    GPU_TEXTURE0 = 0x03, GPU_TEXTURE1 = 0x04, GPU_TEXTURE2 = 0x05, GPU_TEXTURE3 = 0x06,
    GPU_PREVIOUS_BUFFER = 0x0D, GPU_CONSTANT = 0x0E, GPU_PREVIOUS = 0x0F,
} GPU_TEVSRC;

typedef enum
{
    GPU_TEVOP_RGB_SRC_COLOR = 0x00, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR = 0x01,
    GPU_TEVOP_RGB_SRC_ALPHA = 0x02, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA = 0x03,
    GPU_TEVOP_RGB_SRC_R = 0x04, GPU_TEVOP_RGB_ONE_MINUS_SRC_R = 0x05,
    GPU_TEVOP_RGB_0x06 = 0x06, GPU_TEVOP_RGB_0x07 = 0x07,
    GPU_TEVOP_RGB_SRC_G = 0x08, GPU_TEVOP_RGB_ONE_MINUS_SRC_G = 0x09,
    GPU_TEVOP_RGB_0x0A = 0x0A, GPU_TEVOP_RGB_0x0B = 0x0B,
    GPU_TEVOP_RGB_SRC_B = 0x0C, GPU_TEVOP_RGB_ONE_MINUS_SRC_B = 0x0D,
} GPU_TEVOP_RGB;

typedef enum
{
    GPU_TEVOP_A_SRC_ALPHA = 0x00, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA = 0x01,
    GPU_TEVOP_A_SRC_R = 0x02, GPU_TEVOP_A_ONE_MINUS_SRC_R = 0x03,
    GPU_TEVOP_A_SRC_G = 0x04, GPU_TEVOP_A_ONE_MINUS_SRC_G = 0x05,
    GPU_TEVOP_A_SRC_B = 0x06, GPU_TEVOP_A_ONE_MINUS_SRC_B = 0x07,
} GPU_TEVOP_A;

typedef enum
{
    GPU_REPLACE = 0x00, GPU_MODULATE = 0x01, GPU_ADD = 0x02, GPU_ADD_SIGNED = 0x03,
    GPU_INTERPOLATE = 0x04, GPU_SUBTRACT = 0x05, GPU_DOT3_RGB = 0x06, GPU_DOT3_RGBA = 0x07,
    GPU_MULTIPLY_ADD = 0x08, GPU_ADD_MULTIPLY = 0x09,
} GPU_COMBINEFUNC;

typedef enum { GPU_TEVSCALE_1 = 0x0, GPU_TEVSCALE_2 = 0x1, GPU_TEVSCALE_4 = 0x2 } GPU_TEVSCALE;

typedef enum
{
    GPU_TRIANGLES = 0x0000, GPU_TRIANGLE_STRIP = 0x0100, GPU_TRIANGLE_FAN = 0x0200,
    GPU_GEOMETRY_PRIM = 0x0300,
} GPU_Primitive_t;

typedef enum { GPU_VERTEX_SHADER = 0x0, GPU_GEOMETRY_SHADER = 0x1 } GPU_SHADER_TYPE;

#define GPU_TEVSOURCES(a, b, c) (((a)) | ((b) << 4) | ((c) << 8))
#define GPU_TEVOPERANDS(a, b, c) (((a)) | ((b) << 4) | ((c) << 8))

/* ── GX transfers (gx.h) ───────────────────────────────────────────────── */
#define GX_BUFFER_DIM(w, h) (((h) << 16) | ((w) & 0xFFFF))
#define GX_TRANSFER_FLIP_VERT(x) ((x) << 0)
#define GX_TRANSFER_OUT_TILED(x) ((x) << 1)
#define GX_TRANSFER_RAW_COPY(x) ((x) << 3)
#define GX_TRANSFER_IN_FORMAT(x) ((x) << 8)
#define GX_TRANSFER_OUT_FORMAT(x) ((x) << 12)
#define GX_TRANSFER_SCALING(x) ((x) << 24)
enum { GX_TRANSFER_FMT_RGBA8 = 0, GX_TRANSFER_FMT_RGB8 = 1, GX_TRANSFER_FMT_RGB565 = 2,
       GX_TRANSFER_FMT_RGB5A1 = 3, GX_TRANSFER_FMT_RGBA4 = 4 };
enum { GX_TRANSFER_SCALE_NO = 0, GX_TRANSFER_SCALE_X = 1, GX_TRANSFER_SCALE_XY = 2 };

/* Texture-copy mode: `size` bytes from `src` to `dst`, each side in lines of
 * (dim & 0xFFFF) * 16 bytes with (dim >> 16) * 16 bytes skipped after each. */
Result GX_TextureCopy(u32 *src, u32 srcDim, u32 *dst, u32 dstDim, u32 size, u8 flags);

/* ── Memory pools (linear heap, VRAM) ──────────────────────────────────── */
void *linearAlloc(size_t size);
void *linearMemAlign(size_t size, size_t alignment);
void linearFree(void *mem);
u32 linearSpaceFree(void);
void *vramAlloc(size_t size);
void vramFree(void *mem);
u32 vramSpaceFree(void);
/* The CPU's writes reach the GPU: the textures over [addr, addr + size) are
 * detiled again where written, before their next draw. */
Result GSPGPU_FlushDataCache(const void *addr, u32 size);
Result GSPGPU_InvalidateDataCache(const void *addr, u32 size);

/* ── Maths (c3d/maths.h) ───────────────────────────────────────────────── */
typedef union
{
    struct { float w, z, y, x; };
    struct { float r, k, j, i; };
    float c[4];
} C3D_FVec;
typedef C3D_FVec C3D_FQuat;
typedef union
{
    C3D_FVec r[4];
    float m[4 * 4];
} C3D_Mtx;

#define C3D_AngleFromDegrees(_angle) ((_angle) * (float)M_PI / 180.0f)
#define C3D_Angle(_angle) ((_angle) * (float)(2 * M_PI))

static inline C3D_FVec FVec4_New(float x, float y, float z, float w)
{
    return (C3D_FVec){{w, z, y, x}};
}
static inline C3D_FVec FVec3_New(float x, float y, float z) { return FVec4_New(x, y, z, 0.0f); }
static inline float FVec3_Dot(C3D_FVec l, C3D_FVec r) { return l.x * r.x + l.y * r.y + l.z * r.z; }
static inline float FVec4_Dot(C3D_FVec l, C3D_FVec r) { return l.x * r.x + l.y * r.y + l.z * r.z + l.w * r.w; }
static inline C3D_FVec FVec3_Subtract(C3D_FVec l, C3D_FVec r) { return FVec3_New(l.x - r.x, l.y - r.y, l.z - r.z); }
static inline C3D_FVec FVec3_Add(C3D_FVec l, C3D_FVec r) { return FVec3_New(l.x + r.x, l.y + r.y, l.z + r.z); }
static inline C3D_FVec FVec3_Scale(C3D_FVec v, float s) { return FVec3_New(v.x * s, v.y * s, v.z * s); }
static inline float FVec3_Magnitude(C3D_FVec v) { return sqrtf(FVec3_Dot(v, v)); }
static inline C3D_FVec FVec3_Normalize(C3D_FVec v)
{
    float m = FVec3_Magnitude(v);
    return FVec3_New(v.x / m, v.y / m, v.z / m);
}
static inline C3D_FVec FVec3_Cross(C3D_FVec l, C3D_FVec r)
{
    return FVec3_New(l.y * r.z - l.z * r.y, l.z * r.x - l.x * r.z, l.x * r.y - l.y * r.x);
}

static inline void Mtx_Zeros(C3D_Mtx *out) { memset(out, 0, sizeof(*out)); }
static inline void Mtx_Copy(C3D_Mtx *out, const C3D_Mtx *in) { *out = *in; }
void Mtx_Identity(C3D_Mtx *out);
void Mtx_Multiply(C3D_Mtx *out, const C3D_Mtx *a, const C3D_Mtx *b);
C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx *mtx, C3D_FVec v);
C3D_FVec Mtx_MultiplyFVec3(const C3D_Mtx *mtx, C3D_FVec v);
void Mtx_Translate(C3D_Mtx *mtx, float x, float y, float z, bool bRightSide);
void Mtx_Scale(C3D_Mtx *mtx, float x, float y, float z);
void Mtx_RotateX(C3D_Mtx *mtx, float angle, bool bRightSide);
void Mtx_RotateY(C3D_Mtx *mtx, float angle, bool bRightSide);
void Mtx_RotateZ(C3D_Mtx *mtx, float angle, bool bRightSide);
void Mtx_Ortho(C3D_Mtx *mtx, float left, float right, float bottom, float top, float near, float far,
               bool isLeftHanded);
void Mtx_Persp(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, bool isLeftHanded);
void Mtx_PerspStereo(C3D_Mtx *mtx, float fovy, float aspect, float near, float far, float iod,
                     float screen, bool isLeftHanded);
void Mtx_LookAt(C3D_Mtx *out, C3D_FVec cameraPosition, C3D_FVec cameraTarget, C3D_FVec cameraUpVector,
                bool isLeftHanded);

/* ── Shaders (shbin.h, shaderProgram.h) ────────────────────────────────── */
/* A "shbin" here names GLSL programs built into android_c3d.c, one per line
 * after the first ("GLES"): the DVLEs, in that order. */
typedef struct
{
    int program; /* index into the shim's GLSL programs */
} DVLE_s;
typedef struct
{
    u32 numDVLE;
    DVLE_s *DVLE;
} DVLB_s;
typedef struct
{
    DVLE_s *dvle;
} shaderInstance_s;
typedef struct
{
    shaderInstance_s *vertexShader;
    shaderInstance_s *geometryShader;
} shaderProgram_s;

DVLB_s *DVLB_ParseFile(u32 *shbinData, u32 shbinSize);
void DVLB_Free(DVLB_s *dvlb);
Result shaderProgramInit(shaderProgram_s *sp);
Result shaderProgramFree(shaderProgram_s *sp);
Result shaderProgramSetVsh(shaderProgram_s *sp, DVLE_s *dvle);
s8 shaderInstanceGetUniformLocation(shaderInstance_s *si, const char *name);

/* ── Textures (c3d/texture.h) ──────────────────────────────────────────── */
typedef struct C3D_Tex
{
    void *data;
    GPU_TEXCOLOR fmt;
    size_t size;
    u16 width, height;
    u32 param;
    u32 border;
    u32 lodParam;
} C3D_Tex;

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
bool C3D_TexInitVRAM(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexDelete(C3D_Tex *tex);
void C3D_TexFlush(C3D_Tex *tex);
void C3D_TexBind(int unitId, C3D_Tex *tex);
void C3D_TexUpload(C3D_Tex *tex, const void *data);
static inline void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter)
{
    tex->param &= ~(GPU_TEXTURE_MAG_FILTER(1) | GPU_TEXTURE_MIN_FILTER(1));
    tex->param |= GPU_TEXTURE_MAG_FILTER(magFilter) | GPU_TEXTURE_MIN_FILTER(minFilter);
}
static inline void C3D_TexSetWrap(C3D_Tex *tex, GPU_TEXTURE_WRAP_PARAM wrapS, GPU_TEXTURE_WRAP_PARAM wrapT)
{
    tex->param &= ~(GPU_TEXTURE_WRAP_S(3) | GPU_TEXTURE_WRAP_T(3));
    tex->param |= GPU_TEXTURE_WRAP_S(wrapS) | GPU_TEXTURE_WRAP_T(wrapT);
}
static inline int C3D_TexCalcMaxLevel(u32 width, u32 height) { (void)width; (void)height; return 0; }
/* Bits per texel. */
unsigned C3D_TexFmtBits(GPU_TEXCOLOR fmt);

/* A GPU copy (texture-copy mode) from `src` to `dst`: memcpy here. */
void C3D_SyncTextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags);

/* ── Render targets (c3d/renderqueue.h, framebuffer.h) ─────────────────── */
typedef struct C3D_RenderTarget_tag C3D_RenderTarget;
enum { C3D_CLEAR_COLOR = 1, C3D_CLEAR_DEPTH = 2, C3D_CLEAR_ALL = 3 };
typedef int C3D_ClearBits;
enum { C3D_FRAME_SYNCDRAW = 1, C3D_FRAME_NONBLOCK = 2 };

C3D_RenderTarget *C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, int depthFmt);
C3D_RenderTarget *C3D_RenderTargetCreateFromTex(C3D_Tex *tex, GPU_TEXFACE face, int level, int depthFmt);
void C3D_RenderTargetDelete(C3D_RenderTarget *target);
void C3D_RenderTargetClear(C3D_RenderTarget *target, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth);
bool C3D_FrameDrawOn(C3D_RenderTarget *target);
void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h);
void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom);

/* ── Attribute and buffer configuration (c3d/attribs.h, buffers.h) ────── */
typedef struct
{
    int attrCount;
    u8 reg[12];
    u8 format[12];
    u8 count[12];
    bool fixed[12];
} C3D_AttrInfo;

typedef struct
{
    const void *data;
    u32 stride;
    u32 attribCount;
    u64 permutation;
} C3D_BufCfg;

typedef struct
{
    int bufCount;
    C3D_BufCfg buffers[12];
} C3D_BufInfo;

void AttrInfo_Init(C3D_AttrInfo *info);
int AttrInfo_AddLoader(C3D_AttrInfo *info, int regId, GPU_FORMATS format, int count);
int AttrInfo_AddFixed(C3D_AttrInfo *info, int regId);
C3D_AttrInfo *C3D_GetAttrInfo(void);
void C3D_SetAttrInfo(C3D_AttrInfo *info);
void BufInfo_Init(C3D_BufInfo *info);
int BufInfo_Add(C3D_BufInfo *info, const void *data, ptrdiff_t stride, int attribCount, u64 permutation);
C3D_BufInfo *C3D_GetBufInfo(void);
void C3D_SetBufInfo(C3D_BufInfo *info);
C3D_FVec *C3D_FixedAttribGetWritePtr(int id);
static inline void C3D_FixedAttribSet(int id, float x, float y, float z, float w)
{
    C3D_FVec *ptr = C3D_FixedAttribGetWritePtr(id);
    ptr->x = x; ptr->y = y; ptr->z = z; ptr->w = w;
}

/* ── Uniforms (c3d/uniforms.h) ─────────────────────────────────────────── */
C3D_FVec *C3D_FVUnifWritePtr(GPU_SHADER_TYPE type, int id, int size);
static inline void C3D_FVUnifSet(GPU_SHADER_TYPE type, int id, float x, float y, float z, float w)
{
    C3D_FVec *ptr = C3D_FVUnifWritePtr(type, id, 1);
    ptr->x = x; ptr->y = y; ptr->z = z; ptr->w = w;
}
static inline void C3D_FVUnifMtxNx4(GPU_SHADER_TYPE type, int id, const C3D_Mtx *mtx, int num)
{
    C3D_FVec *ptr = C3D_FVUnifWritePtr(type, id, num);
    for (int i = 0; i < num; ++i)
        ptr[i] = mtx->r[i];
}
static inline void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE type, int id, const C3D_Mtx *mtx)
{
    C3D_FVUnifMtxNx4(type, id, mtx, 4);
}

/* ── Texture environment (c3d/texenv.h) ────────────────────────────────── */
typedef struct
{
    u16 srcRgb, srcAlpha;
    u16 opRgb, opAlpha;
    u16 funcRgb, funcAlpha;
    u32 color;
    u16 scaleRgb, scaleAlpha;
} C3D_TexEnv;

typedef enum { C3D_RGB = 1 << 0, C3D_Alpha = 1 << 1, C3D_Both = C3D_RGB | C3D_Alpha } C3D_TexEnvMode;

C3D_TexEnv *C3D_GetTexEnv(int id);
void C3D_SetTexEnv(int id, C3D_TexEnv *env);
void C3D_DirtyTexEnv(C3D_TexEnv *env);
static inline void C3D_TexEnvInit(C3D_TexEnv *env)
{
    env->srcRgb = GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0);
    env->srcAlpha = env->srcRgb;
    env->opRgb = env->opAlpha = 0;
    env->funcRgb = env->funcAlpha = GPU_REPLACE;
    env->color = 0xFFFFFFFF;
    env->scaleRgb = env->scaleAlpha = GPU_TEVSCALE_1;
}
static inline void C3D_TexEnvSrc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_TEVSRC s1, GPU_TEVSRC s2, GPU_TEVSRC s3)
{
    int param = GPU_TEVSOURCES((int)s1, (int)s2, (int)s3);
    if (mode & C3D_RGB) env->srcRgb = (u16)param;
    if (mode & C3D_Alpha) env->srcAlpha = (u16)param;
}
static inline void C3D_TexEnvOpRgb(C3D_TexEnv *env, GPU_TEVOP_RGB o1, GPU_TEVOP_RGB o2, GPU_TEVOP_RGB o3)
{
    env->opRgb = (u16)GPU_TEVOPERANDS((int)o1, (int)o2, (int)o3);
}
static inline void C3D_TexEnvOpAlpha(C3D_TexEnv *env, GPU_TEVOP_A o1, GPU_TEVOP_A o2, GPU_TEVOP_A o3)
{
    env->opAlpha = (u16)GPU_TEVOPERANDS((int)o1, (int)o2, (int)o3);
}
static inline void C3D_TexEnvFunc(C3D_TexEnv *env, C3D_TexEnvMode mode, GPU_COMBINEFUNC param)
{
    if (mode & C3D_RGB) env->funcRgb = (u16)param;
    if (mode & C3D_Alpha) env->funcAlpha = (u16)param;
}
static inline void C3D_TexEnvColor(C3D_TexEnv *env, u32 color) { env->color = color; }
static inline void C3D_TexEnvScale(C3D_TexEnv *env, int mode, GPU_TEVSCALE param)
{
    if (mode & C3D_RGB) env->scaleRgb = (u16)param;
    if (mode & C3D_Alpha) env->scaleAlpha = (u16)param;
}
void C3D_TexEnvBufUpdate(int mode, int mask);
void C3D_TexEnvBufColor(u32 color);

/* ── Effects (c3d/effect.h) ────────────────────────────────────────────── */
void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset);
void C3D_CullFace(GPU_CULLMODE mode);
void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask);
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass);
void C3D_BlendingColor(u32 color);
void C3D_EarlyDepthTest(bool enable, GPU_EARLYDEPTHFUNC function, u32 ref);
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask);
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref);
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr,
                    GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha);
void C3D_ColorLogicOp(GPU_LOGICOP op);

/* ── Drawing (c3d/base.h) ──────────────────────────────────────────────── */
enum { C3D_UNSIGNED_BYTE = 0, C3D_UNSIGNED_SHORT = 1 };
void C3D_BindProgram(shaderProgram_s *program);
void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size);
void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void *indices);

/* The colour words citro2d builds (0xAABBGGRR), which ctr_voxel.c uses too. */
#ifndef C2D_Color32
static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a)
{
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}
static inline u32 C2D_Color32f(float r, float g, float b, float a)
{
    return C2D_Color32((u8)(r * 255.0f), (u8)(g * 255.0f), (u8)(b * 255.0f), (u8)(a * 255.0f));
}
#endif

/* ── Android side (android_video.c) ────────────────────────────────────── */
/* After the EGL context is current; false without a usable GLES 3. */
bool AndroidC3D_Init(void);
void AndroidC3D_Shutdown(void);
/* The GL texture a C3D_Tex is drawn from (uploaded first if written). */
unsigned AndroidC3D_GLTexture(C3D_Tex *tex);
/* The 3DS's flush of the linear heap at C3D_FrameEnd: textures the CPU
 * wrote there since their upload go up again. Before the frame's draws. */
void AndroidC3D_FlushLinear(void);
/* Forget the GL state the shim believes current: the compositor's own GL
 * calls come between the shim's draws. */
void AndroidC3D_ResetState(void);
/* A render target over `tex` drawn at `scale` times the texture's size (the
 * display's resolution rather than the 3DS's); coordinates stay the
 * texture's. Replaces the GL texture of an earlier target over it. */
C3D_RenderTarget *AndroidC3D_RenderTargetScaled(C3D_Tex *tex, int depthFmt, float scale);
/* The framebuffer object of a target (0: none). */
unsigned AndroidC3D_TargetFramebuffer(C3D_RenderTarget *target);
/* Draws this frame, and vertex bytes streamed. */
void AndroidC3D_FrameStats(unsigned *draws, unsigned *vertexBytes, unsigned *texelUploads);

#endif
