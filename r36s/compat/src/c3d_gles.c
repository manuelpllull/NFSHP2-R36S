// citro3d on the R36S, drawn with GLES 2.
//
// The game hands citro3d what it hands the PICA200: shader binaries,
// uniforms in registers, attribute and buffer layouts, six combiner
// stages, textures in the PICA's tiled formats, render targets. This file
// keeps all of that with citro3d's semantics and turns each draw into GL:
//
//   * Shaders: each .v.pica has a GLSL twin (c3d_shaders.h), picked by the
//     shader binary's name; the game's uniform registers map onto the
//     twin's uniforms by name and are uploaded when they change.
//   * The combiner: one fragment shader per configuration of the six
//     stages, the combiner buffer and the alpha test, generated from them
//     as the PICA computes them (Citra's order) and cached by that key.
//     The stages' colours, the buffer's and the alpha reference are
//     uniforms, so a fade does not make a new shader.
//   * Vertices: read where the game's buffers are, through BufInfo and
//     AttrInfo, with the PICA's packing (each attribute aligned to its
//     element size, padding nibbles).
//   * Textures: untiled from the PICA's 8x8 Morton tiles on first bind
//     after a change, the memory's first row at v = 1 as the game writes
//     it; mipmap chains are completed down to 1x1 for GLES.
//   * Targets: a framebuffer object each. A screen's target is landscape
//     (640x480 for the top), its projections untilted (c3d_maths.c); one
//     made from a texture renders into that texture's GL image, a cube's
//     face included.
//   * Depth: the PICA's z/w map (C3D_DepthMap) goes to the twins as a
//     uniform, so GREATER and GEQUAL tests hold as the game wrote them.
//   * Frames: C3D_FrameEnd presents the top target over the window,
//     letterboxed to 4:3, with the bottom target as a panel over its
//     lower half when r36s_bottom_popup says so (main.c), then swaps.
//
// NFSHP2_SHOTS saves frames as PPM files in the working directory, for
// headless runs: "every:N" or a list of frame numbers.

#include <SDL.h>
#include <dlfcn.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <citro3d.h>

#include "c3d_shaders.h"
#include "r36s_gl.h"
#include "r36s_platform.h"
#include "glthread.h"

// ------------------------------------------------------------- state

C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_FVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16      C3D_BoolUnifs[2];
bool     C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
bool     C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
bool     C3D_BoolUnifsDirty[2];

// citro3d's context starts with its GX queue, which main.c's
// waitRenderingDone reads; it is never running here.
u8 __C3D_Context[256];

typedef struct {
    bool              up;
    shaderProgram_s*  program;
    C3D_AttrInfo      attr;
    C3D_BufInfo       buf;
    C3D_TexEnv        env[6];
    u32               texEnvBuf;        // update masks: rgb bits 8-11, alpha 12-15
    u32               texEnvBufColor;
    C3D_Tex*          tex[3];
    GPU_CULLMODE      cull;
    bool              depthTest;
    GPU_TESTFUNC      depthFunc;
    GPU_WRITEMASK     writeMask;
    bool              alphaTest;
    GPU_TESTFUNC      alphaFunc;
    int               alphaRef;
    GPU_BLENDEQUATION colorEq, alphaEq;
    GPU_BLENDFACTOR   srcClr, dstClr, srcAlpha, dstAlpha;
    u32               blendColor;
    bool              zBuffer;
    float             zScale, zOffset;
    u32               viewport[4];
    C3D_FrameBuf      fb;
    C3D_RenderTarget* target;
    C3D_RenderTarget* targets;
    bool              inFrame;
    u32               frames;
    u64               frameStartTick;
    float             drawMs, procMs;
} Context;

static Context s;

// The target each screen shows: as citro3d's, the last one linked to it.
static C3D_RenderTarget* s_linked[2];

// NFSHP2_GL_FROM=N, for headless runs on a slow renderer: nothing is drawn,
// cleared or presented before frame N (textures and targets are made on
// first use, so they start then). The game runs as it would.
static u32 s_glFrom;
static bool gl_skip(void) { return s.frames < s_glFrom; }

// What GL has now, so a draw sets only what changed.
typedef struct {
    bool     valid;
    GLuint   program;
    GLuint   fbo;
    GLint    vp[4];
    int      depthTest;      // -1 unknown
    GLenum   depthFunc;
    int      depthMask;
    int      colorMask;
    int      cull;           // 0 off, else GL_FRONT/GL_BACK
    int      blend;
    GLenum   blendEq[2], blendFn[4];
    u32      blendColor;
    unsigned attribs;
    int      activeUnit;
    GLuint   boundTex[3];
    GLenum   boundTarget[3];
} GLCache;

static GLCache g;

// The draws' shader key, kept while nothing it is made of changes
// (key_get): the combiner's stages and buffer, and the alpha test, mark it
// stale where they are set.
static bool s_keyStale = true;

// GL_VERSION, read once at C3D_Init.
static char s_glVersion[128];
static char s_glRenderer[128];

// NFSHP2_DRAWSTATS' pass number: a clear or a target change starts a pass.
static u32 s_dsPass;

// NFSHP2_GLDEBUG=N: every N frames, what the draws did.
static struct {
    int every;
    u32 asked, drawn, noTarget, noProgram, noTwin, noLink, quads2d, flush2d;
    u32 uploads;
} s_dbg;
void r36sgl_debug_2d(int quads)
{
    s_dbg.flush2d++;
    s_dbg.quads2d += (u32)quads;
    r36s_glcounts.flush2d++;
    r36s_glcounts.quads2d += (unsigned)quads;
}
bool r36sgl_debug_frame(void) { return s_dbg.every > 0 && s.frames == (u32)s_dbg.every + 1; }
static bool    s_gl;            // a context exists
static bool    s_extMinMax, s_extDepth24, s_extPacked;
static void  (*s_flush2d)(void);

bool r36sgl_active(void) { return s_gl; }
void r36sgl_set_flush2d(void (*flush)(void)) { s_flush2d = flush; }
void r36sgl_flush2d(void) { if (s_flush2d) s_flush2d(); }
void r36sgl_depth_map(float out[2]) { out[0] = s.zScale; out[1] = s.zOffset; }

static void tex_env_defaults(void)
{
    for (int i = 0; i < 6; i++)
        C3D_TexEnvInit(&s.env[i]);
    s_keyStale = true;
}

static bool has_ext(const char* name)
{
    const char* e = (const char*)glGetString(GL_EXTENSIONS);
    size_t n = strlen(name);
    while (e != NULL && (e = strstr(e, name)) != NULL) {
        if (e[n] == ' ' || e[n] == '\0') return true;
        e += n;
    }
    return false;
}

bool C3D_Init(size_t cmdBufSize)
{
    (void)cmdBufSize;
    if (s.up) return true;
    memset(&s, 0, sizeof s);
    tex_env_defaults();
    s.texEnvBufColor = 0xFFFFFFFF;
    s.depthTest = true;
    s.depthFunc = GPU_GREATER;
    s.writeMask = GPU_WRITE_ALL;
    s.cull = GPU_CULL_BACK_CCW;
    s.colorEq = s.alphaEq = GPU_BLEND_ADD;
    s.srcClr = s.srcAlpha = GPU_SRC_ALPHA;
    s.dstClr = s.dstAlpha = GPU_ONE_MINUS_SRC_ALPHA;
    s.zBuffer = true;
    s.zScale = -1.0f;
    s.zOffset = 0.0f;
    r36s_video_init();
    s_gl = r36s_video_has_gl();
    if (s_gl) {
        s_extMinMax = has_ext("GL_EXT_blend_minmax");
        s_extDepth24 = has_ext("GL_OES_depth24");
        s_extPacked = has_ext("GL_OES_packed_depth_stencil");
        snprintf(s_glVersion, sizeof s_glVersion, "%s", (const char*)glGetString(GL_VERSION));
        snprintf(s_glRenderer, sizeof s_glRenderer, "%s", (const char*)glGetString(GL_RENDERER));
        printf("R36S: GL %s, %s\n", s_glVersion, s_glRenderer);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glDisable(GL_DITHER);
    }
    memset(&g, 0, sizeof g);
    {
        const char* e = getenv("NFSHP2_GLDEBUG");
        s_dbg.every = e && atoi(e) > 0 ? atoi(e) : -1;
        e = getenv("NFSHP2_GL_FROM");
        s_glFrom = e ? (u32)strtoul(e, NULL, 10) : 0;
    }
    s.up = true;
    // From here the GL calls are played on the GL thread (glthread.c).
    if (s_gl) rgl_start();
    return true;
}

void C3D_Fini(void)
{
    s.up = false;
}

float C3D_GetCmdBufUsage(void) { return 0.0f; }

void C3D_BindProgram(shaderProgram_s* program) { s.program = program; }

void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h)
{
    s.viewport[0] = x; s.viewport[1] = y; s.viewport[2] = w; s.viewport[3] = h;
}

void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom)
{
    (void)mode; (void)left; (void)top; (void)right; (void)bottom;
}

void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset)
{
    s.zBuffer = bIsZBuffer;
    s.zScale = zScale;
    s.zOffset = zOffset;
}

void C3D_CullFace(GPU_CULLMODE mode) { s.cull = mode; }

void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask)
{
    (void)enable; (void)function; (void)ref; (void)inputMask; (void)writeMask;
}

void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass)
{
    (void)sfail; (void)dfail; (void)pass;
}

void C3D_BlendingColor(u32 color) { s.blendColor = color; }
void C3D_EarlyDepthTest(bool enable, GPU_EARLYDEPTHFUNC function, u32 ref)
{
    (void)enable; (void)function; (void)ref;
}

void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask)
{
    s.depthTest = enable;
    s.depthFunc = function;
    s.writeMask = writemask;
}

void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref)
{
    // The reference is a uniform, not the key's.
    if (s.alphaTest != enable || s.alphaFunc != function) s_keyStale = true;
    s.alphaTest = enable;
    s.alphaFunc = function;
    s.alphaRef = ref;
}

void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq,
                    GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr,
                    GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha)
{
    s.colorEq = colorEq; s.alphaEq = alphaEq;
    s.srcClr = srcClr; s.dstClr = dstClr;
    s.srcAlpha = srcAlpha; s.dstAlpha = dstAlpha;
}

void C3D_ColorLogicOp(GPU_LOGICOP op) { (void)op; }
void C3D_FragOpMode(GPU_FRAGOPMODE mode) { (void)mode; }
void C3D_FragOpShadow(float scale, float bias) { (void)scale; (void)bias; }

void C3Di_UpdateContext(void) {}
void C3Di_DirtyUniforms(GPU_SHADER_TYPE type)
{
    for (int i = 0; i < C3D_FVUNIF_COUNT; i++) C3D_FVUnifDirty[type][i] = true;
    C3D_BoolUnifsDirty[type] = true;
}
void C3Di_LoadShaderUniforms(shaderInstance_s* si) { (void)si; }
bool C3Di_SplitFrame(u32** pBuf, u32* pSize)
{
    if (pBuf) *pBuf = NULL;
    if (pSize) *pSize = 0;
    return false;
}
void C3D_UpdateUniforms(GPU_SHADER_TYPE type) { (void)type; }

// The command buffer goes nowhere, but georender.c moves vertex buffer 0
// with one register (its bind_buffer): the offset from base_paddr.
void GPUCMD_Add(u32 header, const u32* param, u32 paramlength)
{
    u32 reg = header & 0x3FF;
    bool incremental = (header >> 31) != 0;
    for (u32 i = 0; i < paramlength; i++) {
        u32 r = incremental ? reg + i : reg;
        if (r == GPUREG_ATTRIBBUFFER0_OFFSET) {
            s.buf.buffers[0].offset = param[i];
            s.buf.r36s_vaddr[0] = (const void*)(uintptr_t)(s.buf.base_paddr + param[i]);
        }
    }
}
void GPUCMD_SetBuffer(u32* adr, u32 size, u32 offset) { (void)adr; (void)size; (void)offset; }
void GPUCMD_GetBuffer(u32** adr, u32* size, u32* offset)
{
    if (adr) *adr = NULL;
    if (size) *size = 0;
    if (offset) *offset = 0;
}

// -------------------------------------------------- attributes, buffers

void AttrInfo_Init(C3D_AttrInfo* info)
{
    memset(info, 0, sizeof *info);
    info->flags[1] = 0xFFF << 16;
}

int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count)
{
    int id;
    if (info->attrCount == 12 || regId < 0 || regId > 15) return -1;
    id = info->attrCount++;
    if (id < 8)
        info->flags[0] |= GPU_ATTRIBFMT(id, count, format);
    else
        info->flags[1] |= GPU_ATTRIBFMT(id - 8, count, format);
    info->flags[1] = (info->flags[1] & 0xFFFF) | ((info->flags[1] >> 16) & ~(1u << id)) << 16;
    info->permutation |= (u64)regId << (id * 4);
    return id;
}

int AttrInfo_AddFixed(C3D_AttrInfo* info, int regId)
{
    int id;
    if (info->attrCount == 12 || regId < 0 || regId > 15) return -1;
    id = info->attrCount++;
    info->flags[1] |= (1u << (id + 16));
    info->permutation |= (u64)regId << (id * 4);
    return id;
}

C3D_AttrInfo* C3D_GetAttrInfo(void) { return &s.attr; }
void C3D_SetAttrInfo(C3D_AttrInfo* info) { if (info != &s.attr) s.attr = *info; }

void BufInfo_Init(C3D_BufInfo* info)
{
    memset(info, 0, sizeof *info);
    info->base_paddr = 0;
}

// citro3d stores each buffer's offset from base_paddr; the GL layer needs
// the address itself, kept beside it. base_paddr is 0 here, so the offset
// is the address too, as georender.c's register write assumes.
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride,
                int attribCount, u64 permutation)
{
    int id;
    if (info->bufCount == 12) return -1;
    id = info->bufCount++;
    info->buffers[id].offset = (u32)(uintptr_t)data;
    info->buffers[id].flags[0] = (u32)(permutation & 0xFFFFFFFF);
    info->buffers[id].flags[1] = (u32)((permutation >> 32) & 0xFFFF)
                               | ((u32)stride << 16) | ((u32)attribCount << 28);
    info->r36s_vaddr[id] = data;
    return id;
}

C3D_BufInfo* C3D_GetBufInfo(void) { return &s.buf; }
void C3D_SetBufInfo(C3D_BufInfo* info) { if (info != &s.buf) s.buf = *info; }

// --------------------------------------------------------- tex env

// These mark the draws' shader key stale (s_keyStale). As citro3d on the
// 3DS: a stage changes through the pointer C3D_GetTexEnv gives, before the
// next draw, or is marked with C3D_DirtyTexEnv.
C3D_TexEnv* C3D_GetTexEnv(int id) { s_keyStale = true; return &s.env[(id < 0 || id > 5) ? 0 : id]; }
void C3D_SetTexEnv(int id, C3D_TexEnv* env) { s_keyStale = true; if (id >= 0 && id <= 5) s.env[id] = *env; }
void C3D_DirtyTexEnv(C3D_TexEnv* env) { (void)env; s_keyStale = true; }

// As citro3d's: `mask` is the stages (0-3) whose output goes to the
// combiner buffer, for the colour, the alpha or both.
void C3D_TexEnvBufUpdate(int mode, int mask)
{
    u32 val = s.texEnvBuf;
    mask &= 0xF;
    if (mode & C3D_RGB) val = (val & ~(0xFu << 8)) | ((u32)mask << 8);
    if (mode & C3D_Alpha) val = (val & ~(0xFu << 12)) | ((u32)mask << 12);
    s.texEnvBuf = val;
    s_keyStale = true;
}
void C3D_TexEnvBufColor(u32 color) { s.texEnvBufColor = color; }

// ----------------------------------------------------------- GL utility

static GLuint compile(GLenum type, const char* src, const char* what)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        printf("R36S: %s %s shader: %s\n%s\n", what,
               type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint r36sgl_link(const char* vs, const char* fs, const char* what)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs, what);
    GLuint f = v ? compile(GL_FRAGMENT_SHADER, fs, what) : 0;
    GLuint p;
    GLint ok = 0;
    if (!v || !f) {
        if (v) glDeleteShader(v);
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "v0");
    glBindAttribLocation(p, 1, "v1");
    glBindAttribLocation(p, 2, "v2");
    glBindAttribLocation(p, 3, "v3");
    // The instanced twin's per-instance attributes (kVsGeoSolidInst).
    glBindAttribLocation(p, 4, "iMvp0");
    glBindAttribLocation(p, 5, "iMvp1");
    glBindAttribLocation(p, 6, "iMvp2");
    glBindAttribLocation(p, 7, "iMvp3");
    glBindAttribLocation(p, 8, "iFog");
    glBindAttribLocation(p, 9, "iLight");
    if (rgl_program_binaries() && strstr(s_glVersion, "OpenGL ES 3") != NULL
        && rgl_direct_active())
        rgl_ProgramParameteri(p, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        printf("R36S: %s program: %s\n", what, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void r36sgl_use_program(GLuint prog)
{
    if (!g.valid || g.program != prog) {
        glUseProgram(prog);
        g.program = prog;
        r36s_glcounts.programs++;
    }
}

// The attribute pointers GL has, with the array buffer each was set
// with, so the batches of one mesh set them once (a model's batches share
// its buffer and layout).
static struct {
    bool        valid;
    GLint       buf;
    GLint       size;
    GLenum      type;
    GLboolean   norm;
    GLsizei     stride;
    const void* ptr;
} s_ptr[10];

void r36sgl_attrib_forget(void)
{
    memset(s_ptr, 0, sizeof s_ptr);
}

void r36sgl_attrib_pointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                           const void* ptr)
{
    GLint buf = r36sgl_bound_array();
    if (i < 10 && buf >= 0 && s_ptr[i].valid && s_ptr[i].buf == buf && s_ptr[i].size == size
        && s_ptr[i].type == type && s_ptr[i].norm == norm && s_ptr[i].stride == stride
        && s_ptr[i].ptr == ptr)
        return;
    glVertexAttribPointer(i, size, type, norm, stride, ptr);
    if (i < 10) {
        s_ptr[i].valid = buf >= 0;
        s_ptr[i].buf = buf;
        s_ptr[i].size = size;
        s_ptr[i].type = type;
        s_ptr[i].norm = norm;
        s_ptr[i].stride = stride;
        s_ptr[i].ptr = ptr;
    }
}

void r36sgl_attrib_mask(unsigned mask)
{
    unsigned diff = g.valid ? (g.attribs ^ mask) : 0x3FFu;
    for (unsigned i = 0; i < 10; i++) {
        if (!(diff & (1u << i))) continue;
        if (mask & (1u << i)) glEnableVertexAttribArray(i);
        else glDisableVertexAttribArray(i);
    }
    g.attribs = mask;
}

static void active_unit(int unit)
{
    if (g.activeUnit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g.activeUnit = unit;
    }
}

static void bind_gl_texture(int unit, GLenum target, GLuint name)
{
    if (g.boundTex[unit] == name && g.boundTarget[unit] == target) return;
    active_unit(unit);
    if (g.boundTarget[unit] != 0 && g.boundTarget[unit] != target)
        glBindTexture(g.boundTarget[unit], 0);
    glBindTexture(target, name);
    r36s_glcounts.textures++;
    g.boundTex[unit] = name;
    g.boundTarget[unit] = target;
}

// Forgets what GL has, after something set it behind the cache's back.
static void cache_reset(void)
{
    memset(&g, 0, sizeof g);
    g.depthTest = g.depthMask = g.colorMask = g.cull = g.blend = -1;
    g.attribs = 0;
    for (int i = 0; i < 10; i++) glDisableVertexAttribArray(i);
    for (int i = 0; i < 3; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    g.activeUnit = 0;
    glUseProgram(0);
    g.program = 0;
    g.fbo = (GLuint)-1;
    r36sgl_buffers_unknown();
    r36sgl_attrib_forget();
    g.valid = true;
}

// --------------------------------------------------------- textures

u32 C3D_TexCalcMaxLevel(u32 width, u32 height)
{
    u32 size = width < height ? width : height, lvl = 0;
    while (size > 8) { size >>= 1; lvl++; }
    return lvl;
}

u32 C3D_TexCalcLevelSize(u32 size, int level) { return size >> (2 * level); }

u32 C3D_TexCalcTotalSize(u32 size, int maxLevel)
{
    // size + size/4 + size/16 + ...: as citro3d writes it.
    return (size - C3D_TexCalcLevelSize(size, maxLevel + 1)) * 4 / 3;
}

static int tex_mode(const C3D_Tex* tex) { return (int)((tex->param >> 28) & 7); }

// A cube's six faces' memory, by GL name: georender.c renders each face
// through a 2D C3D_Tex copied from the cube's, with `data` set to the
// face, so a target made over one is found here by that pointer.
#define MAX_CUBES 8
static struct { GLuint gl; void* face[6]; bool rt; } s_cubes[MAX_CUBES];

// Whether a target draws into the cube `gl`: then GL holds its faces.
static bool cube_is_rt(GLuint gl)
{
    for (int i = 0; gl != 0 && i < MAX_CUBES; i++)
        if (s_cubes[i].gl == gl) return s_cubes[i].rt;
    return false;
}

static void cube_register(GLuint gl, const C3D_TexCube* cube)
{
    for (int i = 0; i < MAX_CUBES; i++)
        if (s_cubes[i].gl == 0) {
            s_cubes[i].gl = gl;
            for (int f = 0; f < 6; f++) s_cubes[i].face[f] = cube->data[f];
            return;
        }
}

static void cube_forget(GLuint gl)
{
    for (int i = 0; i < MAX_CUBES; i++)
        if (s_cubes[i].gl == gl) memset(&s_cubes[i], 0, sizeof s_cubes[i]);
}

// GL names given to textures, each with the generation that owns it. The
// game copies C3D_Tex structs (a UI image's, a cube face's), and the copy
// keeps the name: once one copy deletes it, GL may give the name to
// something else, a screen's colour buffer say, which a stale copy must
// then neither bind nor delete. A copy whose generation no longer owns its
// name gets a texture of its own on its next bind.
// What each GL texture's storage was last specified as, by name (the game
// copies C3D_Tex structs, so a struct can't hold it): the same again
// replaces the pixels with no new storage for the driver to make and free.
typedef struct { u16 w, h; u8 levels, ok; GLenum format, type; } TexSpec;
static TexSpec* s_spec;
static u32*   s_owner;
static GLuint s_ownerCap;
static u32    s_ownerGen;
unsigned r36s_gl_textures_made;

static void tex_gl_new(C3D_Tex* tex)
{
    GLuint name = 0;
    glGenTextures(1, &name);
    if (name >= s_ownerCap) {
        GLuint cap = s_ownerCap ? s_ownerCap : 256;
        while (cap <= name) cap *= 2;
        u32* grown = (u32*)realloc(s_owner, cap * sizeof *grown);
        if (grown == NULL) abort();
        memset(grown + s_ownerCap, 0, (cap - s_ownerCap) * sizeof *grown);
        s_owner = grown;
        {
            TexSpec* spec = (TexSpec*)realloc(s_spec, cap * sizeof *spec);
            if (spec == NULL) abort();
            memset(spec + s_ownerCap, 0, (cap - s_ownerCap) * sizeof *spec);
            s_spec = spec;
        }
        s_ownerCap = cap;
    }
    s_owner[name] = ++s_ownerGen;
    r36s_gl_textures_made++;
    s_spec[name].ok = 0;
    tex->r36s_gl = name;
    tex->r36s_gen = s_ownerGen;
    tex->r36s_dirty = true;
    tex->r36s_param = 0;
}

static bool tex_gl_live(const C3D_Tex* tex)
{
    GLuint n = tex->r36s_gl;
    return n != 0 && n < s_ownerCap && s_owner[n] == tex->r36s_gen;
}

// ------------------------------------------- GL textures by pixel memory
//
// The game copies C3D_Tex structs: a flipbook shows a frame with
// `t->tex = f->tex` (trackview.c), from a struct that has never been bound
// and so has no GL name. With a name per struct, every flip made a new GL
// texture, uploaded the pixels again, and lost the old name with the old
// copy: about 138 KB a flip (measured under emulation), until the system
// killed the game. So a texture's GL name goes with its pixel memory: a
// fresh copy of pixels that already have a texture takes it, and the upload
// it needs is only the first. Freeing the memory, or deleting any struct
// that has it, frees the texture. NFSHP2_TEXSHARE=0 turns this off.
typedef struct PixelTex {
    const void*      data;
    GLuint           name;
    u32              gen;
    u32              param;
    u16              w, h;
    u8               fmt, levels;
    bool             current;   // GL has these pixels
    struct PixelTex* next;
} PixelTex;

#define PIX_BUCKETS 1024
static PixelTex*       s_pix[PIX_BUCKETS];
static pthread_mutex_t s_pixLock = PTHREAD_MUTEX_INITIALIZER;
static int             s_shareMode = -1;
static GLuint*         s_pixDead;           // names to delete at the frame's end
static u32*            s_pixDeadGen;
static int             s_pixDeadN, s_pixDeadCap;

static bool share_on(void)
{
    if (s_shareMode < 0) {
        const char* e = getenv("NFSHP2_TEXSHARE");
        s_shareMode = e == NULL || e[0] != '0';
    }
    return s_shareMode != 0;
}

static PixelTex** pix_slot(const void* data)
{
    uintptr_t h = (uintptr_t)data;
    PixelTex** p = &s_pix[((h >> 7) ^ (h >> 17)) & (PIX_BUCKETS - 1)];
    while (*p != NULL && (*p)->data != data) p = &(*p)->next;
    return p;
}

static bool pix_shareable(const C3D_Tex* tex)
{
    return share_on() && tex->data != NULL && !tex->r36s_rt && tex_mode(tex) != GPU_TEX_CUBE_MAP;
}

// A copy with no live name takes the texture of its pixels, if they have
// one: false when they don't.
static bool tex_try_share(C3D_Tex* tex)
{
    bool ok = false;
    if (!pix_shareable(tex)) return false;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex* e = *pix_slot(tex->data);
        if (e != NULL && e->w == tex->width && e->h == tex->height && e->fmt == (u8)tex->fmt
            && e->levels == tex->maxLevel && e->name < s_ownerCap && s_owner[e->name] == e->gen) {
            tex->r36s_gl = e->name;
            tex->r36s_gen = e->gen;
            tex->r36s_dirty = !e->current;
            tex->r36s_param = e->param;
            ok = true;
        }
    }
    pthread_mutex_unlock(&s_pixLock);
    return ok;
}

// Off the books: its name goes at the frame's end.
static void pix_retire(PixelTex** slot)
{
    PixelTex* e = *slot;
    *slot = e->next;
    if (s_pixDeadN == s_pixDeadCap) {
        int cap = s_pixDeadCap ? s_pixDeadCap * 2 : 64;
        GLuint* d = (GLuint*)realloc(s_pixDead, (size_t)cap * sizeof *d);
        u32* g2 = (u32*)realloc(s_pixDeadGen, (size_t)cap * sizeof *g2);
        if (d != NULL) s_pixDead = d;
        if (g2 != NULL) s_pixDeadGen = g2;
        if (d != NULL && g2 != NULL) s_pixDeadCap = cap;
    }
    if (s_pixDeadN < s_pixDeadCap) {
        s_pixDead[s_pixDeadN] = e->name;
        s_pixDeadGen[s_pixDeadN] = e->gen;
        s_pixDeadN++;
    }
    free(e);
}

// A new name for a struct whose pixels have none, on the books.
static void tex_attach(C3D_Tex* tex)
{
    if (tex_try_share(tex)) return;
    tex_gl_new(tex);
    if (!pix_shareable(tex)) return;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex** slot = pix_slot(tex->data);
        PixelTex* e;
        if (*slot != NULL) pix_retire(slot);    // the same memory as something else now
        e = (PixelTex*)calloc(1, sizeof *e);
        if (e != NULL) {
            e->data = tex->data;
            e->name = tex->r36s_gl;
            e->gen = tex->r36s_gen;
            e->w = tex->width;
            e->h = tex->height;
            e->fmt = (u8)tex->fmt;
            e->levels = tex->maxLevel;
            e->next = NULL;
            *pix_slot(tex->data) = e;
        }
    }
    pthread_mutex_unlock(&s_pixLock);
}

static void pix_uploaded(const C3D_Tex* tex)
{
    if (s_shareMode <= 0 || tex->data == NULL) return;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex* e = *pix_slot(tex->data);
        if (e != NULL && e->name == tex->r36s_gl) e->current = true;
    }
    pthread_mutex_unlock(&s_pixLock);
}

static void pix_param(const C3D_Tex* tex)
{
    if (s_shareMode <= 0 || tex->data == NULL) return;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex* e = *pix_slot(tex->data);
        if (e != NULL && e->name == tex->r36s_gl) e->param = tex->r36s_param;
    }
    pthread_mutex_unlock(&s_pixLock);
}

// The game changed the pixels.
static void pix_stale(const void* data)
{
    if (s_shareMode <= 0 || data == NULL) return;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex* e = *pix_slot(data);
        if (e != NULL) e->current = false;
    }
    pthread_mutex_unlock(&s_pixLock);
}

// The memory is gone (linearFree, vramFree: any thread). Its pointer may
// come back as other pixels, so it leaves the books at once.
void r36s_tex_memory_freed(const void* mem)
{
    if (s_shareMode <= 0 || mem == NULL) return;
    pthread_mutex_lock(&s_pixLock);
    {
        PixelTex** slot = pix_slot(mem);
        if (*slot != NULL) pix_retire(slot);
    }
    pthread_mutex_unlock(&s_pixLock);
}

static void gl_name_delete(GLuint name)
{
    for (int i = 0; i < 3; i++)
        if (g.boundTex[i] == name) {
            g.boundTex[i] = 0;
            g.boundTarget[i] = 0;
        }
    cube_forget(name);
    glDeleteTextures(1, &name);
    if (name < s_ownerCap) s_owner[name] = 0;
}

// At the frame's end, on the game's thread, after the held draws: the
// retired textures go. One a live struct renamed since isn't deleted.
static void pix_bury(void)
{
    GLuint name[64];
    u32 gen[64];
    int n;
    do {
        pthread_mutex_lock(&s_pixLock);
        n = s_pixDeadN < 64 ? s_pixDeadN : 64;
        for (int i = 0; i < n; i++) {
            name[i] = s_pixDead[--s_pixDeadN];
            gen[i] = s_pixDeadGen[s_pixDeadN];
        }
        pthread_mutex_unlock(&s_pixLock);
        for (int i = 0; i < n; i++)
            if (name[i] < s_ownerCap && s_owner[name[i]] == gen[i]) gl_name_delete(name[i]);
    } while (n > 0);
}

bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p)
{
    u32 size;
    if ((p.width & (p.width - 1)) || (p.height & (p.height - 1))
        || p.width < 8 || p.height < 8 || p.width > 1024 || p.height > 1024)
        return false;
    memset(tex, 0, sizeof *tex);
    tex->width = p.width;
    tex->height = p.height;
    tex->fmt = p.format;
    tex->maxLevel = p.maxLevel;
    size = (u32)p.width * p.height * (u32)C3D_TexFmtBpp(p.format) / 8;
    tex->size = size;
    if (p.maxLevel) size = C3D_TexCalcTotalSize(size, p.maxLevel);
    if (p.type == GPU_TEX_CUBE_MAP) {
        if (cube == NULL) return false;
        for (int i = 0; i < 6; i++) {
            cube->data[i] = p.onVram ? vramAlloc(size) : linearAlloc(size);
            if (cube->data[i] == NULL) {
                for (int j = 0; j < i; j++)
                    p.onVram ? vramFree(cube->data[j]) : linearFree(cube->data[j]);
                return false;
            }
        }
        tex->cube = cube;
    } else {
        tex->data = p.onVram ? vramAlloc(size) : linearAlloc(size);
        if (tex->data == NULL) return false;
    }
    tex->param = GPU_TEXTURE_MODE(p.type)
               | GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST);
    if (p.format == GPU_ETC1) tex->param |= GPU_TEXTURE_ETC1_PARAM;
    tex->r36s_dirty = true;
    // A cube gets its GL name now, so the 2D copies of it that georender
    // makes for its faces' targets carry the name with them.
    if (p.type == GPU_TEX_CUBE_MAP && s_gl) {
        tex_gl_new(tex);
        cube_register(tex->r36s_gl, cube);
    }
    return true;
}

void* C3D_TexGetImagePtr(C3D_Tex* tex, void* data, int level, u32* size)
{
    u32 off = 0;
    if (level > 0) off = C3D_TexCalcTotalSize(tex->size, level - 1);
    if (size) *size = level >= 0 ? C3D_TexCalcLevelSize(tex->size, level)
                                 : C3D_TexCalcTotalSize(tex->size, tex->maxLevel);
    return (u8*)data + (level >= 0 ? off : 0);
}

void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level)
{
    u32 size;
    void* out = C3D_TexGetImagePtr(tex,
        tex_mode(tex) == GPU_TEX_CUBE_MAP ? tex->cube->data[face] : tex->data,
        level, &size);
    memcpy(out, data, size);
    tex->r36s_dirty = true;
    if (tex_mode(tex) != GPU_TEX_CUBE_MAP) pix_stale(tex->data);
}

void C3D_TexGenerateMipmap(C3D_Tex* tex, GPU_TEXFACE face)
{
    (void)face;
    tex->r36s_dirty = true;
    if (tex_mode(tex) != GPU_TEX_CUBE_MAP) pix_stale(tex->data);
}
void C3D_TexBind(int unitId, C3D_Tex* tex) { if (unitId >= 0 && unitId < 3) s.tex[unitId] = tex; }
void C3D_TexFlush(C3D_Tex* tex)
{
    tex->r36s_dirty = true;
    if (tex_mode(tex) != GPU_TEX_CUBE_MAP) pix_stale(tex->data);
}

void C3D_TexDelete(C3D_Tex* tex)
{
    if (s_gl) {
        // Its pixels' texture, whichever struct it was made for, and any
        // other name this struct holds.
        GLuint shared = 0;
        u32 sharedGen = 0;
        if (s_shareMode > 0 && tex->data != NULL && tex_mode(tex) != GPU_TEX_CUBE_MAP) {
            pthread_mutex_lock(&s_pixLock);
            {
                PixelTex** slot = pix_slot(tex->data);
                if (*slot != NULL) {
                    PixelTex* e = *slot;
                    shared = e->name;
                    sharedGen = e->gen;
                    *slot = e->next;
                    free(e);
                }
            }
            pthread_mutex_unlock(&s_pixLock);
        }
        if (tex_gl_live(tex) || shared != 0) r36sgl_flush_held();
        if (tex_gl_live(tex)) {
            if (tex->r36s_gl == shared) shared = 0;
            gl_name_delete(tex->r36s_gl);
        }
        if (shared != 0 && shared < s_ownerCap && s_owner[shared] == sharedGen)
            gl_name_delete(shared);
    }
    tex->r36s_gl = 0;
    for (int i = 0; i < 3; i++)
        if (s.tex[i] == tex) s.tex[i] = NULL;
    if (tex_mode(tex) == GPU_TEX_CUBE_MAP) {
        for (int i = 0; i < 6; i++) linearFree(tex->cube->data[i]);
    } else {
        linearFree(tex->data);
    }
    tex->data = NULL;
}

void C3D_TexShadowParams(bool perspective, float bias) { (void)perspective; (void)bias; }

// The PICA's tiling: texel (x, y) of a level w wide is at tile
// (y/8)*(w/8) + x/8, then this order within the tile's 8x8.
static u8 s_morton[64];

static void morton_init(void)
{
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++)
            s_morton[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
                                     | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

typedef struct {
    GLenum fmt, type;
    int    srcBpp;   // bytes a texel in the PICA's memory (0: 4-bit)
    int    dstBpp;   // bytes a texel for GL
} TexFormat;

static bool tex_format(GPU_TEXCOLOR f, TexFormat* out)
{
    switch (f) {
    case GPU_RGBA8:    *out = (TexFormat){ GL_RGBA, GL_UNSIGNED_BYTE, 4, 4 }; return true;
    case GPU_RGB8:     *out = (TexFormat){ GL_RGB, GL_UNSIGNED_BYTE, 3, 3 }; return true;
    case GPU_RGBA5551: *out = (TexFormat){ GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, 2 }; return true;
    case GPU_RGB565:   *out = (TexFormat){ GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 2 }; return true;
    case GPU_RGBA4:    *out = (TexFormat){ GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, 2 }; return true;
    case GPU_LA8:      *out = (TexFormat){ GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, 2 }; return true;
    case GPU_HILO8:    *out = (TexFormat){ GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, 2 }; return true;
    case GPU_L8:       *out = (TexFormat){ GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, 1 }; return true;
    case GPU_A8:       *out = (TexFormat){ GL_ALPHA, GL_UNSIGNED_BYTE, 1, 1 }; return true;
    case GPU_LA4:      *out = (TexFormat){ GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 1, 2 }; return true;
    case GPU_L4:       *out = (TexFormat){ GL_LUMINANCE, GL_UNSIGNED_BYTE, 0, 1 }; return true;
    case GPU_A4:       *out = (TexFormat){ GL_ALPHA, GL_UNSIGNED_BYTE, 0, 1 }; return true;
    default:           return false;   // ETC1: the game makes none
    }
}

// One texel from the PICA's bytes into GL's, by format.
static inline void texel_convert(GPU_TEXCOLOR f, const u8* src, u32 index, u8* dst)
{
    switch (f) {
    case GPU_RGBA8: {   // a, b, g, r
        const u8* p = src + index * 4;
        dst[0] = p[3]; dst[1] = p[2]; dst[2] = p[1]; dst[3] = p[0];
        break;
    }
    case GPU_RGB8: {    // b, g, r
        const u8* p = src + index * 3;
        dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0];
        break;
    }
    case GPU_RGBA5551:
    case GPU_RGB565:
    case GPU_RGBA4:
        memcpy(dst, src + index * 2, 2);   // the same bits as GL's packed types
        break;
    case GPU_LA8:
    case GPU_HILO8: {   // a, l
        const u8* p = src + index * 2;
        dst[0] = p[1]; dst[1] = p[0];
        break;
    }
    case GPU_LA4: {     // l in the high nibble, a in the low
        u8 v = src[index];
        dst[0] = (u8)((v >> 4) * 17); dst[1] = (u8)((v & 15) * 17);
        break;
    }
    case GPU_L4:
    case GPU_A4: {
        u8 v = src[index >> 1];
        dst[0] = (u8)(((index & 1) ? (v >> 4) : (v & 15)) * 17);
        break;
    }
    default:            // L8, A8
        dst[0] = src[index];
        break;
    }
}

static u8*    s_scratch;
static size_t s_scratchCap;

static u8* scratch(size_t n)
{
    if (n > s_scratchCap) {
        free(s_scratch);
        s_scratch = (u8*)malloc(n);
        s_scratchCap = s_scratch ? n : 0;
    }
    return s_scratch;
}

// A level from tiled PICA memory into GL's rows: the memory's first row
// is the image's top, v = 1, so it becomes GL's last row.
static void untile(GPU_TEXCOLOR f, const TexFormat* tf, const u8* src, u32 w, u32 h, u8* dst)
{
    if (s_morton[63] == 0) morton_init();
    for (u32 y = 0; y < h; y++) {
        u8* row = dst + (size_t)(h - 1 - y) * w * tf->dstBpp;
        u32 tileRow = (y >> 3) * (w >> 3);
        for (u32 x = 0; x < w; x++) {
            u32 i = ((tileRow + (x >> 3)) << 6) + s_morton[((y & 7) << 3) | (x & 7)];
            texel_convert(f, src, i, row + (size_t)x * tf->dstBpp);
        }
    }
}

// The next level down of a GL image, for the levels below the PICA's
// smallest (8x8) that a complete GLES chain needs: bytes averaged, or the
// top-left texel of each 2x2 for the packed 16-bit formats.
static void halve(const TexFormat* tf, const u8* src, u32 w, u32 h, u8* dst)
{
    u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    int bpp = tf->dstBpp;
    bool packed = tf->type != GL_UNSIGNED_BYTE;
    for (u32 y = 0; y < nh; y++)
        for (u32 x = 0; x < nw; x++) {
            u32 x0 = x * 2 < w ? x * 2 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1;
            u32 x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            u8* o = dst + ((size_t)y * nw + x) * bpp;
            if (packed) {
                memcpy(o, src + ((size_t)y0 * w + x0) * bpp, bpp);
                continue;
            }
            for (int c = 0; c < bpp; c++) {
                unsigned sum = src[((size_t)y0 * w + x0) * bpp + c] + src[((size_t)y0 * w + x1) * bpp + c]
                             + src[((size_t)y1 * w + x0) * bpp + c] + src[((size_t)y1 * w + x1) * bpp + c];
                o[c] = (u8)((sum + 2) / 4);
            }
        }
}

// Uploads one face (or the 2D image) with its mip chain, completed to 1x1.
static void upload_image(GLenum target, const C3D_Tex* tex, const u8* data, const TexFormat* tf)
{
    u32 w = tex->width, h = tex->height;
    int levels = tex->maxLevel;
    size_t first = (size_t)w * h * tf->dstBpp;
    u8* buf = scratch(first * 2 + 64);
    u8* cur;
    static int subMode = -1;
    bool same = false;
    if (subMode < 0) {
        const char* e = getenv("NFSHP2_TEXSUB");
        subMode = e == NULL || e[0] != '0';
    }
    if (buf == NULL) return;
    if (subMode && target == GL_TEXTURE_2D && tex->r36s_gl < s_ownerCap) {
        TexSpec* sp = &s_spec[tex->r36s_gl];
        same = sp->ok && sp->w == w && sp->h == h && sp->levels == levels
            && sp->format == (GLenum)tf->fmt && sp->type == tf->type;
        sp->ok = 1;
        sp->w = (u16)w;
        sp->h = (u16)h;
        sp->levels = (u8)levels;
        sp->format = tf->fmt;
        sp->type = tf->type;
    }
    for (int lvl = 0; lvl <= levels; lvl++) {
        u32 lw = w >> lvl, lh = h >> lvl;
        u32 size;
        const u8* src = (const u8*)C3D_TexGetImagePtr((C3D_Tex*)tex, (void*)data, lvl, &size);
        if (lw < 8 || lh < 8) break;
        untile(tex->fmt, tf, src, lw, lh, buf);
        if (same) rgl_TexWhole2D(target, lvl, lw, lh, tf->fmt, tf->type, buf);
        else glTexImage2D(target, lvl, tf->fmt, lw, lh, 0, tf->fmt, tf->type, buf);
    }
    if (levels > 0) {
        // Down to 1x1 from the PICA's last level, which is still in `buf`.
        u32 lw = w >> levels, lh = h >> levels;
        cur = buf;
        for (int lvl = levels + 1; lw > 1 || lh > 1; lvl++) {
            u8* next = (cur == buf) ? buf + first : buf;
            halve(tf, cur, lw, lh, next);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            if (same) rgl_TexWhole2D(target, lvl, lw, lh, tf->fmt, tf->type, next);
            else glTexImage2D(target, lvl, tf->fmt, lw, lh, 0, tf->fmt, tf->type, next);
            cur = next;
        }
    }
}

static void apply_tex_params(GLenum target, C3D_Tex* tex)
{
    static const GLenum wrap[4] = { GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT };
    u32 p = tex->param;
    bool mips = tex->maxLevel > 0 && !tex->r36s_rt;
    bool mag = (p >> 1) & 1, min = (p >> 2) & 1, mip = (p >> 24) & 1;
    GLenum minf = !mips ? (min ? GL_LINEAR : GL_NEAREST)
                : min ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                      : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap[(p >> 12) & 3]);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap[(p >> 8) & 3]);
    tex->r36s_param = p | 0x80000000u;   // never 0 once applied
    pix_param(tex);
}

GLenum r36sgl_bind_texture(int unit, C3D_Tex* tex)
{
    GLenum target;
    TexFormat tf;
    if (tex == NULL || !s_gl) return 0;
    target = tex_mode(tex) == GPU_TEX_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    if (!tex_gl_live(tex)) tex_attach(tex);
    bind_gl_texture(unit, target, tex->r36s_gl);
    if (tex->r36s_dirty && !tex->r36s_rt && !cube_is_rt(tex->r36s_gl)
        && tex_format(tex->fmt, &tf)) {
        if (target == GL_TEXTURE_CUBE_MAP) {
            for (int f = 0; f < 6; f++)
                upload_image(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, tex, (const u8*)tex->cube->data[f], &tf);
        } else {
            upload_image(GL_TEXTURE_2D, tex, (const u8*)tex->data, &tf);
            pix_uploaded(tex);
            if (s_dbg.every > 0 && s_dbg.uploads < 40) {
                size_t nz = 0;
                for (size_t i = 0; i < tex->size; i++) nz += ((const u8*)tex->data)[i] != 0;
                printf("gl: upload %p fmt %d %dx%d levels %d, %u of %u bytes non-zero\n",
                       tex->data, (int)tex->fmt, tex->width, tex->height, tex->maxLevel,
                       (unsigned)nz, (unsigned)tex->size);
                s_dbg.uploads++;
            }
        }
        tex->r36s_param = 0;
        r36s_glcounts.uploads++;
        r36s_glcounts.uploadKB += (unsigned)((target == GL_TEXTURE_CUBE_MAP ? 6u : 1u)
                                             * tex->width * tex->height * tf.dstBpp / 1024u);
    }
    tex->r36s_dirty = false;
    if (tex->r36s_param != (tex->param | 0x80000000u))
        apply_tex_params(target, tex);
    return target;
}

// ------------------------------------------------------ framebuffers

u32 C3D_CalcColorBufSize(u32 width, u32 height, GPU_COLORBUF fmt)
{
    static const u8 bpp[] = { 4, 3, 2, 2, 2 };
    return width * height * bpp[fmt];
}

u32 C3D_CalcDepthBufSize(u32 width, u32 height, GPU_DEPTHBUF fmt)
{
    return width * height * (fmt == GPU_RB_DEPTH16 ? 2 : fmt == GPU_RB_DEPTH24 ? 3 : 4);
}

C3D_FrameBuf* C3D_GetFrameBuf(void) { return &s.fb; }
void C3D_SetFrameBuf(C3D_FrameBuf* fb) { if (fb != &s.fb) s.fb = *fb; }

static bool depth_wanted(GPU_DEPTHBUF f) { return (int)f >= 0 && (int)f <= 3; }

// A target's framebuffer object, made on first use: by then the game has
// said whether it is a screen's (C3D_RenderTargetSetOutput).
static bool target_ready(C3D_RenderTarget* t)
{
    GLenum status;
    GLuint fbo = 0;
    if (!s_gl || t == NULL) return false;
    if (t->frameBuf.r36s_fbo != 0) return true;
    if (t->linked) {
        // The 3DS's screens are portrait framebuffers; here they are the
        // landscape image itself.
        t->r36s_w = t->frameBuf.height;
        t->r36s_h = t->frameBuf.width;
        // The game's picture, at the screen's own resolution. The game's
        // coordinates reach it through its matrices, whatever its size.
        if (t->screen == GFX_TOP) r36s_screen_pixels(&t->r36s_w, &t->r36s_h);
    } else {
        t->r36s_w = t->frameBuf.width;
        t->r36s_h = t->frameBuf.height;
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    g.fbo = fbo;
    if (t->r36s_tex != NULL) {
        C3D_Tex* tex = t->r36s_tex;
        GLuint name;
        GLenum face = GL_TEXTURE_2D;
        int cube = -1;
        if (!tex_gl_live(tex)) tex_gl_new(tex);
        name = tex->r36s_gl;
        for (int i = 0; name != 0 && i < MAX_CUBES; i++)
            if (s_cubes[i].gl == name)
                for (int f = 0; f < 6; f++)
                    if (s_cubes[i].face[f] == tex->data) {
                        cube = f;
                        s_cubes[i].rt = true;
                    }
        tex->r36s_rt = true;
        tex->r36s_dirty = false;
        tex->r36s_param = 0;
        if (cube >= 0) {
            face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + cube;
            bind_gl_texture(0, GL_TEXTURE_CUBE_MAP, name);
        } else {
            bind_gl_texture(0, GL_TEXTURE_2D, name);
        }
        glTexImage2D(face, 0, GL_RGBA, t->r36s_w, t->r36s_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (cube < 0) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, face, name, 0);
        t->r36s_face = cube;
    } else {
        GLuint name = 0;
        glGenTextures(1, &name);
        bind_gl_texture(0, GL_TEXTURE_2D, name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->r36s_w, t->r36s_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
        t->r36s_color = name;
        t->r36s_face = -1;
    }
    if (depth_wanted(t->frameBuf.depthFmt)) {
        GLuint rb = 0;
        GLenum fmt = s_extPacked ? GL_DEPTH24_STENCIL8_OES
                   : s_extDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, fmt, t->r36s_w, t->r36s_h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        if (s_extPacked)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
        t->r36s_depth = rb;
    }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("R36S: target %dx%d incomplete (0x%x)\n", t->r36s_w, t->r36s_h, status);
    t->frameBuf.r36s_fbo = fbo;
    if (s.target == t) s.fb.r36s_fbo = fbo;
    return true;
}

// NFSHP2_INVALIDATE: leaving a target discards its depth and stencil, which
// nothing reads after (every target is cleared before its next pass), so a
// tile GPU needn't write them to memory. 0: off. 2: a test for a run on a
// software renderer, where discarding does nothing: scribbles over them
// instead, so that anything relying on them shows.
static int s_invMode = -1;

static void discard_depth(void)
{
    if (s_invMode < 0) {
        const char* e = getenv("NFSHP2_INVALIDATE");
        s_invMode = e != NULL && e[0] != '\0' ? atoi(e) : 1;
        if (s_invMode && (strstr(s_glVersion, "OpenGL ES 3") == NULL || !rgl_invalidation()))
            s_invMode = 0;
        printf("R36S: discarding depth %s (NFSHP2_INVALIDATE=%d)\n", s_invMode ? "on" : "off",
               s_invMode);
    }
    if (s_invMode == 1) {
        rgl_InvalidateDepth(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT);
    } else if (s_invMode == 2) {
        if (g.depthMask != 1) {
            glDepthMask(GL_TRUE);
            g.depthMask = 1;
        }
        glClearDepthf(0.37f);
        glClearStencil(0x5A);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

static void bind_fbo(GLuint fbo)
{
    if (g.fbo != fbo) {
        if (g.valid && g.fbo != 0 && g.fbo != (GLuint)-1) discard_depth();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        g.fbo = fbo;
        r36s_glcounts.targets++;
    }
}

// The top screen's drawn share of its target this frame, 0.25 to 1
// (r36s_render_scale): the same target, drawn in its lower left corner and
// shown stretched, so it can change on any frame.
static float s_frameScale = 1.0f;

static void target_size(const C3D_RenderTarget* t, GLint* w, GLint* h)
{
    *w = t->r36s_w;
    *h = t->r36s_h;
    if (t->screen == GFX_TOP && s_frameScale < 1.0f) {
        *w = (GLint)((float)*w * s_frameScale) & ~1;
        *h = (GLint)((float)*h * s_frameScale) & ~1;
    }
}

static void set_viewport(GLint x, GLint y, GLint w, GLint h)
{
    if (g.vp[0] != x || g.vp[1] != y || g.vp[2] != w || g.vp[3] != h) {
        glViewport(x, y, w, h);
        g.vp[0] = x; g.vp[1] = y; g.vp[2] = w; g.vp[3] = h;
    }
}

static C3D_RenderTarget* target_of(C3D_FrameBuf* fb)
{
    if (fb == &s.fb) return s.target;
    return (C3D_RenderTarget*)fb->r36s_target;
}

void C3D_FrameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth)
{
    C3D_RenderTarget* t = target_of(fb);
    GLbitfield bits = 0;
    if (!s_gl || t == NULL || gl_skip() || !target_ready(t)) return;
    r36sgl_flush2d();
    r36sgl_flush_held();
    if (!g.valid) cache_reset();
    bind_fbo(t->frameBuf.r36s_fbo);
    if (clearBits & C3D_CLEAR_COLOR) {
        // The PICA's clear colour is 0xRRGGBBAA.
        glClearColor(((clearColor >> 24) & 0xFF) / 255.0f, ((clearColor >> 16) & 0xFF) / 255.0f,
                     ((clearColor >> 8) & 0xFF) / 255.0f, (clearColor & 0xFF) / 255.0f);
        if (g.colorMask != 0xF) {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            g.colorMask = 0xF;
        }
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((clearBits & C3D_CLEAR_DEPTH) && t->r36s_depth != 0) {
        glClearDepthf((float)(clearDepth & 0xFFFFFF) / 16777215.0f);
        if (g.depthMask != 1) {
            glDepthMask(GL_TRUE);
            g.depthMask = 1;
        }
        bits |= GL_DEPTH_BUFFER_BIT;
        if (s_extPacked) {
            glClearStencil((int)(clearDepth >> 24));
            bits |= GL_STENCIL_BUFFER_BIT;
        }
    }
    if (bits) glClear(bits);
    s_dsPass++;
}

void C3D_FrameBufTransfer(C3D_FrameBuf* fb, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags)
{
    (void)fb; (void)screen; (void)side; (void)transferFlags;
}

static C3D_RenderTarget* target_new(int width, int height, GPU_COLORBUF colorFmt,
                                    GPU_DEPTHBUF depthFmt)
{
    C3D_RenderTarget* t = (C3D_RenderTarget*)calloc(1, sizeof *t);
    if (t == NULL) return NULL;
    t->frameBuf.width = (u16)width;
    t->frameBuf.height = (u16)height;
    t->frameBuf.colorFmt = colorFmt;
    t->frameBuf.depthFmt = depthFmt;
    t->frameBuf.colorMask = 0xF;
    t->frameBuf.depthMask = 0xF;
    t->frameBuf.r36s_target = t;
    t->r36s_face = -1;
    t->next = s.targets;
    if (s.targets) s.targets->prev = t;
    s.targets = t;
    return t;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt,
                                         GPU_DEPTHBUF depthFmt)
{
    return target_new(width, height, colorFmt, depthFmt);
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level,
                                                GPU_DEPTHBUF depthFmt)
{
    C3D_RenderTarget* t;
    (void)face;
    t = target_new(tex->width >> level, tex->height >> level, (GPU_COLORBUF)tex->fmt, depthFmt);
    if (t != NULL) {
        t->r36s_tex = tex;
        t->frameBuf.colorBuf = tex->data;
        tex->r36s_rt = true;
    }
    return t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* t)
{
    if (t == NULL) return;
    r36sgl_flush_held();
    if (t->prev) t->prev->next = t->next; else s.targets = t->next;
    if (t->next) t->next->prev = t->prev;
    if (s.target == t) s.target = NULL;
    for (int i = 0; i < 2; i++)
        if (s_linked[i] == t) s_linked[i] = NULL;
    if (s_gl) {
        if (t->frameBuf.r36s_fbo) {
            GLuint n = t->frameBuf.r36s_fbo;
            if (g.fbo == n) bind_fbo(0);
            glDeleteFramebuffers(1, &n);
        }
        if (t->r36s_color) {
            GLuint n = t->r36s_color;
            glDeleteTextures(1, &n);
        }
        if (t->r36s_depth) {
            GLuint n = t->r36s_depth;
            glDeleteRenderbuffers(1, &n);
        }
    }
    free(t);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen,
                               gfx3dSide_t side, u32 transferFlags)
{
    if ((unsigned)screen < 2) s_linked[screen] = target;
    if (target == NULL) return;
    target->screen = screen;
    target->side = side;
    target->transferFlags = transferFlags;
    target->linked = true;
}

void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
    (void)inadr; (void)indim; (void)outadr; (void)outdim; (void)flags;
}

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
    GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
}

void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a,
                        u32 buf1v, u32* buf1e, u16 control1)
{
    (void)buf0a; (void)buf0v; (void)buf0e; (void)control0;
    (void)buf1a; (void)buf1v; (void)buf1e; (void)control1;
}

// ------------------------------------------------- fixed-function state

static GLenum gl_test(GPU_TESTFUNC f)
{
    static const GLenum m[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL,
                                 GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
    return m[f & 7];
}

static GLenum gl_factor(GPU_BLENDFACTOR f)
{
    static const GLenum m[15] = {
        GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR,
        GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
        GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE,
    };
    return (unsigned)f < 15 ? m[f] : GL_ONE;
}

static GLenum gl_equation(GPU_BLENDEQUATION e)
{
    switch (e) {
    case GPU_BLEND_SUBTRACT:         return GL_FUNC_SUBTRACT;
    case GPU_BLEND_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case GPU_BLEND_MIN:              return s_extMinMax ? GL_MIN_EXT : GL_FUNC_ADD;
    case GPU_BLEND_MAX:              return s_extMinMax ? GL_MAX_EXT : GL_FUNC_ADD;
    default:                         return GL_FUNC_ADD;
    }
}

// The PICA's culling: GPU_CULL_BACK_CCW drops the triangles wound
// clockwise on screen, as GL's default (front = CCW, cull back).
// The fixed-function state a draw applies, as citro3d holds it: what
// apply_fixed sets, kept with a draw that waits to be grouped.
typedef struct {
    bool              depthTest;
    GPU_TESTFUNC      depthFunc;
    GPU_WRITEMASK     writeMask;
    GPU_CULLMODE      cull;
    GPU_BLENDEQUATION colorEq, alphaEq;
    GPU_BLENDFACTOR   srcClr, dstClr, srcAlpha, dstAlpha;
    u32               blendColor;
} FixedSnap;

static void fixed_snap(FixedSnap* f)
{
    memset(f, 0, sizeof *f);
    f->depthTest = s.depthTest;
    f->depthFunc = s.depthFunc;
    f->writeMask = s.writeMask;
    f->cull = s.cull;
    f->colorEq = s.colorEq;
    f->alphaEq = s.alphaEq;
    f->srcClr = s.srcClr;
    f->dstClr = s.dstClr;
    f->srcAlpha = s.srcAlpha;
    f->dstAlpha = s.dstAlpha;
    f->blendColor = s.blendColor;
}

// Whether `f` blends: the PICA always does, and ONE/ZERO with ADD is the
// same as off.
static bool fixed_blends(const FixedSnap* f)
{
    return !(f->colorEq == GPU_BLEND_ADD && f->alphaEq == GPU_BLEND_ADD
             && f->srcClr == GPU_ONE && f->dstClr == GPU_ZERO
             && f->srcAlpha == GPU_ONE && f->dstAlpha == GPU_ZERO);
}

static void apply_fixed_from(const FixedSnap* f)
{
    int dt = f->depthTest ? 1 : 0;
    GLenum df = gl_test(f->depthFunc);
    int dm = (f->depthTest && (f->writeMask & GPU_WRITE_DEPTH)) ? 1 : 0;
    int cm = f->writeMask & 0xF;
    int cull = f->cull == GPU_CULL_BACK_CCW ? GL_BACK : f->cull == GPU_CULL_FRONT_CCW ? GL_FRONT : 0;
    GLenum eq[2] = { gl_equation(f->colorEq), gl_equation(f->alphaEq) };
    GLenum fn[4] = { gl_factor(f->srcClr), gl_factor(f->dstClr), gl_factor(f->srcAlpha), gl_factor(f->dstAlpha) };
    int blend = 1;

    if (g.depthTest != dt) {
        if (dt) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        g.depthTest = dt;
    }
    if (dt && g.depthFunc != df) {
        glDepthFunc(df);
        g.depthFunc = df;
    }
    if (g.depthMask != dm) {
        glDepthMask(dm ? GL_TRUE : GL_FALSE);
        g.depthMask = dm;
    }
    if (g.colorMask != cm) {
        glColorMask((cm & 1) != 0, (cm & 2) != 0, (cm & 4) != 0, (cm & 8) != 0);
        g.colorMask = cm;
    }
    if (g.cull != cull) {
        if (cull == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            if (g.cull <= 0) glEnable(GL_CULL_FACE);
            glCullFace(cull);
        }
        g.cull = cull;
    }
    // The PICA always blends; ONE/ZERO with ADD is the same as off.
    if (!fixed_blends(f)) blend = 0;
    if (g.blend != blend) {
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        g.blend = blend;
    }
    if (blend) {
        if (g.blendEq[0] != eq[0] || g.blendEq[1] != eq[1]) {
            glBlendEquationSeparate(eq[0], eq[1]);
            g.blendEq[0] = eq[0]; g.blendEq[1] = eq[1];
        }
        if (memcmp(g.blendFn, fn, sizeof fn) != 0) {
            glBlendFuncSeparate(fn[0], fn[1], fn[2], fn[3]);
            memcpy(g.blendFn, fn, sizeof fn);
        }
        if (g.blendColor != f->blendColor) {
            u32 c = f->blendColor;
            glBlendColor((c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                         ((c >> 16) & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
            g.blendColor = c;
        }
    }
}

static void apply_fixed(void)
{
    FixedSnap f;
    fixed_snap(&f);
    apply_fixed_from(&f);
}

bool r36sgl_begin_draw(void)
{
    C3D_RenderTarget* t = s.target;
    if (!s_gl || t == NULL || !s.inFrame || gl_skip()) return false;
    if (!target_ready(t)) return false;
    if (r36sgl_debug_frame()) {
        GLint actual = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &actual);
        printf("gl:   draw on target %p %dx%d fbo %u linked %d screen %d; cached fbo %d, GL's %d\n",
               (void*)t, t->r36s_w, t->r36s_h, (unsigned)t->frameBuf.r36s_fbo, t->linked,
               (int)t->screen, (int)g.fbo, actual);
    }
    if (!g.valid) cache_reset();
    bind_fbo(t->frameBuf.r36s_fbo);
    if (t->linked) {
        // A screen's viewport is in the 3DS's portrait terms; the game
        // only ever gives the whole target there.
        GLint tw, th;
        target_size(t, &tw, &th);
        set_viewport(0, 0, tw, th);
    } else {
        set_viewport((GLint)s.viewport[0], (GLint)s.viewport[1],
                     (GLint)s.viewport[2], (GLint)s.viewport[3]);
    }
    apply_fixed();
    return true;
}

// ---------------------------------------------------------- programs

// The fragment shader's configuration: what changes its code. The stages'
// colours, the buffer's initial colour and the alpha reference are
// uniforms.
typedef struct {
    u16 srcRgb[6], srcAlpha[6];
    u16 opRgb[6], opAlpha[6];
    u8  funcRgb[6], funcAlpha[6];
    u8  scaleRgb[6], scaleAlpha[6];
    u8  bufRgb, bufAlpha;   // update masks, stages 0-3
    u8  alphaFunc;          // GPU_ALWAYS when the test is off
    u8  texType[3];         // 0 none, 1 2D, 2 cube
    u8  vs;                 // the vertex twin
    u8  pad[2];
} ProgKey;

// VS_SOLID_INST is geosolid's instanced twin, which no shader binary names:
// the grouped draws use it (try_defer).
enum { VS_SOLID, VS_LIT, VS_FOLD, VS_SOLID_INST, VS_COUNT };
static const char* const kVsSource[VS_COUNT] = { kVsGeoSolid, kVsGeoLit, kVsGeoFold,
                                                 kVsGeoSolidInst };
static const char* const kVsName[VS_COUNT] = { "geosolid", "geolit", "geofold", "geosolid-inst" };

typedef struct {
    GLint loc;
    char  kind;
    u8    reg, count;
} UniformMap;

#define MAX_UNIFORMS 24

typedef struct Prog {
    ProgKey    key;
    bool       used;
    GLuint     gl;
    GLint      uDepth, uConst, uBuf, uAlphaRef, uTex[3];
    int        nUni;
    UniformMap uni[MAX_UNIFORMS];
    float      shadow[C3D_FVUNIF_COUNT][4];
    u16        boolShadow;
    bool       fresh;
    float      depthShadow[2];
    u32        constShadow[6], bufShadow;
    int        refShadow;
    // geosolid's: the registers of the model's uniforms (mvp's four rows,
    // fogModel, lightModel), -1 when absent, and the instanced twin with
    // this one's combiner, made the first time a draw is held.
    int        instMvp, instFog, instLight;
    bool       instTried;
    struct Prog* inst;
} Prog;

#define PROG_SLOTS 512
static Prog* s_progs;
static Prog* s_lastProg;
static ProgKey s_lastKey;

static int vs_of(const DVLE_s* dvle)
{
    for (int i = 0; i < VS_SOLID_INST; i++)
        if (strstr(dvle->name, kVsName[i]) != NULL) return i;
    return -1;
}

static bool src_uses(const ProgKey* k, int src)
{
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 3; j++)
            if (((k->srcRgb[i] >> (4 * j)) & 0xF) == src || ((k->srcAlpha[i] >> (4 * j)) & 0xF) == src)
                return true;
    return false;
}

// One stage's operand, as GLSL, from source `src` with operand `op`.
static void gen_operand_rgb(char* out, size_t n, const char* src, unsigned op)
{
    static const char* const m[16] = {
        "%s.rgb", "(1.0 - %s.rgb)", "%s.aaa", "(1.0 - %s.aaa)",
        "%s.rrr", "(1.0 - %s.rrr)", "%s.rgb", "%s.rgb",
        "%s.ggg", "(1.0 - %s.ggg)", "%s.rgb", "%s.rgb",
        "%s.bbb", "(1.0 - %s.bbb)", "%s.rgb", "%s.rgb",
    };
    snprintf(out, n, m[op & 15], src);
}

static void gen_operand_alpha(char* out, size_t n, const char* src, unsigned op)
{
    static const char* const m[8] = {
        "%s.a", "(1.0 - %s.a)", "%s.r", "(1.0 - %s.r)",
        "%s.g", "(1.0 - %s.g)", "%s.b", "(1.0 - %s.b)",
    };
    snprintf(out, n, m[op & 7], src);
}

static const char* gen_source(unsigned src, int stage, char* buf, size_t n)
{
    switch (src) {
    case GPU_PRIMARY_COLOR:  return "pc";
    case GPU_TEXTURE0:       return "t0";
    case GPU_TEXTURE1:       return "t1";
    case GPU_TEXTURE2:       return "t2";
    case GPU_PREVIOUS_BUFFER:return "buf";
    case GPU_CONSTANT:       snprintf(buf, n, "uConst[%d]", stage); return buf;
    case GPU_PREVIOUS:       return "prev";
    default:                 return "zero";   // lighting, procedural
    }
}

static void gen_combine(char* out, size_t n, unsigned func, const char* a, const char* b,
                        const char* c, bool rgb)
{
    const char* half = rgb ? "vec3(0.5)" : "0.5";
    switch (func) {
    case GPU_MODULATE:     snprintf(out, n, "(%s * %s)", a, b); break;
    case GPU_ADD:          snprintf(out, n, "(%s + %s)", a, b); break;
    case GPU_ADD_SIGNED:   snprintf(out, n, "(%s + %s - %s)", a, b, half); break;
    case GPU_INTERPOLATE:  snprintf(out, n, "mix(%s, %s, %s)", b, a, c); break;
    case GPU_SUBTRACT:     snprintf(out, n, "(%s - %s)", a, b); break;
    case GPU_DOT3_RGB:
    case GPU_DOT3_RGBA:
        if (rgb) snprintf(out, n, "vec3(4.0 * dot(%s - 0.5, %s - 0.5))", a, b);
        else     snprintf(out, n, "%s", a);
        break;
    case GPU_MULTIPLY_ADD: snprintf(out, n, "(%s * %s + %s)", a, b, c); break;
    case GPU_ADD_MULTIPLY: snprintf(out, n, "(min(%s + %s, %s) * %s)", a, b, rgb ? "vec3(1.0)" : "1.0", c); break;
    default:               snprintf(out, n, "%s", a); break;   // REPLACE
    }
}

// The combiner as the PICA runs it (Citra's order): each stage reads the
// previous stage's output and the combiner buffer, which lags a stage
// behind the outputs it is updated from.
static char* gen_fragment(const ProgKey* k)
{
    size_t cap = 16384, len = 0;
    char* fs = (char*)malloc(cap);
    char a[3][96], b[3][96], rgb[512], alpha[512], cb[3][32];
#define EMIT(...) (len += (size_t)snprintf(fs + len, cap - len, __VA_ARGS__))
    if (fs == NULL) return NULL;
    EMIT("precision mediump float;\n"
         R36S_FS_HIGHP
         "varying vec4 vColor;\n"
         "varying HIGHP vec4 vTex0;\n"
         "varying HIGHP vec4 vTex1;\n"
         "uniform vec4 uConst[6];\n"
         "uniform vec4 uBuf;\n"
         "uniform float uAlphaRef;\n");
    for (int u = 0; u < 3; u++)
        if (k->texType[u]) EMIT("uniform %s uTex%d;\n", k->texType[u] == 2 ? "samplerCube" : "sampler2D", u);
    EMIT("void main() {\n"
         "    vec4 zero = vec4(0.0);\n"
         "    vec4 pc = clamp(vColor, 0.0, 1.0);\n");
    for (int u = 0; u < 3; u++) {
        // Unit 0 reads texcoord0; units 1 and 2 texcoord1 (the shaders
        // write no texcoord2).
        const char* tc = u == 0 ? "vTex0" : "vTex1";
        if (k->texType[u] == 2) EMIT("    vec4 t%d = textureCube(uTex%d, %s.xyz);\n", u, u, tc);
        else if (k->texType[u] == 1) EMIT("    vec4 t%d = texture2D(uTex%d, %s.xy);\n", u, u, tc);
        else EMIT("    vec4 t%d = vec4(0.0);\n", u);
    }
    EMIT("    vec4 prev = vec4(0.0);\n"
         "    vec4 buf = vec4(0.0);\n"
         "    vec4 nextBuf = uBuf;\n"
         "    vec4 o;\n");
    for (int i = 0; i < 6; i++) {
        static const char* const scale[4] = { "1.0", "2.0", "4.0", "1.0" };
        for (int j = 0; j < 3; j++) {
            const char* sr = gen_source((k->srcRgb[i] >> (4 * j)) & 0xF, i, cb[j], sizeof cb[j]);
            gen_operand_rgb(a[j], sizeof a[j], sr, (k->opRgb[i] >> (4 * j)) & 0xF);
        }
        for (int j = 0; j < 3; j++) {
            char cbuf[32];
            const char* sa = gen_source((k->srcAlpha[i] >> (4 * j)) & 0xF, i, cbuf, sizeof cbuf);
            gen_operand_alpha(b[j], sizeof b[j], sa, (k->opAlpha[i] >> (4 * j)) & 0xF);
        }
        gen_combine(rgb, sizeof rgb, k->funcRgb[i], a[0], a[1], a[2], true);
        gen_combine(alpha, sizeof alpha, k->funcAlpha[i], b[0], b[1], b[2], false);
        EMIT("    o.rgb = clamp(%s * %s, 0.0, 1.0);\n", rgb, scale[k->scaleRgb[i] & 3]);
        if (k->funcRgb[i] == GPU_DOT3_RGBA)
            EMIT("    o.a = o.r;\n");
        else
            EMIT("    o.a = clamp(%s * %s, 0.0, 1.0);\n", alpha, scale[k->scaleAlpha[i] & 3]);
        EMIT("    prev = o;\n"
             "    buf = nextBuf;\n");
        if (i < 4 && (k->bufRgb & (1u << i))) EMIT("    nextBuf.rgb = o.rgb;\n");
        if (i < 4 && (k->bufAlpha & (1u << i))) EMIT("    nextBuf.a = o.a;\n");
    }
    switch (k->alphaFunc) {
    case GPU_NEVER:    EMIT("    discard;\n"); break;
    case GPU_ALWAYS:   break;
    default: {
        static const char* const op[8] = { "", "", "==", "!=", "<", "<=", ">", ">=" };
        EMIT("    if (!(floor(prev.a * 255.0 + 0.5) %s uAlphaRef)) discard;\n", op[k->alphaFunc & 7]);
        break;
    }
    }
    EMIT("    gl_FragColor = prev;\n}\n");
#undef EMIT
    return fs;
}

static u32 key_hash(const ProgKey* k)
{
    const u8* p = (const u8*)k;
    u32 h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// ------------------------------------------------ the program cache
//
// A program compiled on the device takes the Mali driver tens of ms, and
// the game meets its combiner settings as it draws: a stall in the race.
// So each is saved, as the driver's binary, to nfshp2_programs.bin in the
// port's folder, and the first draw that needs a vertex shader loads every
// saved program for that shader. A record names its key and a hash of the
// shader's text, and the file names the driver, so a change to either
// compiles again. NFSHP2_PROGCACHE=0 turns it off.
#define PROGCACHE_FILE "nfshp2_programs.bin"
#define PROGCACHE_MAGIC 0x31435046u

typedef struct {
    ProgKey key;
    u32     src;
    u32     format;
    u32     len;
    void*   data;
} ProgRec;

static ProgRec* s_recs;
static int      s_recN, s_recCap;
static int      s_cacheMode = -1;
static bool     s_warmed[VS_COUNT];

static u32 src_hash(const char* vs, const char* fs)
{
    u32 h = 2166136261u;
    for (const char* c = vs; *c != '\0'; c++) h = (h ^ (u8)*c) * 16777619u;
    for (const char* c = fs; *c != '\0'; c++) h = (h ^ (u8)*c) * 16777619u;
    return h;
}

static void cache_open(void)
{
    const char* e;
    FILE* f;
    u32 head[3];
    char id[256];
    long good;
    if (s_cacheMode >= 0) return;
    e = getenv("NFSHP2_PROGCACHE");
    s_cacheMode = e == NULL || e[0] != '0';
    if (s_cacheMode && (strstr(s_glVersion, "OpenGL ES 3") == NULL || !rgl_program_binaries()))
        s_cacheMode = 0;
    printf("R36S: program cache %s\n", s_cacheMode ? "on" : "off");
    if (!s_cacheMode || (f = fopen(PROGCACHE_FILE, "rb")) == NULL) return;
    good = 0;
    if (fread(head, sizeof head, 1, f) == 1 && head[0] == PROGCACHE_MAGIC
        && head[1] == sizeof(ProgKey) && head[2] == strlen(s_glVersion) && head[2] < sizeof id
        && fread(id, head[2], 1, f) == 1 && memcmp(id, s_glVersion, head[2]) == 0) {
        good = ftell(f);
        for (;;) {
            ProgRec r;
            if (fread(&r.key, sizeof r.key, 1, f) != 1 || fread(&r.src, 4, 1, f) != 1
                || fread(&r.format, 4, 1, f) != 1 || fread(&r.len, 4, 1, f) != 1
                || r.len == 0 || r.len > 4u << 20)
                break;
            r.data = malloc(r.len);
            if (r.data == NULL || fread(r.data, r.len, 1, f) != 1) {
                free(r.data);
                break;
            }
            if (s_recN == s_recCap) {
                int cap = s_recCap ? s_recCap * 2 : 128;
                ProgRec* n = (ProgRec*)realloc(s_recs, (size_t)cap * sizeof *n);
                if (n == NULL) {
                    free(r.data);
                    break;
                }
                s_recs = n;
                s_recCap = cap;
            }
            s_recs[s_recN++] = r;
            good = ftell(f);
        }
    }
    fclose(f);
    // Another driver's file, or a record cut short: appending must start
    // from what was read whole.
    if (truncate(PROGCACHE_FILE, good) != 0) printf("R36S: program cache: can't trim the file\n");
    printf("R36S: program cache: %d programs in the file\n", s_recN);
}

static GLuint cache_load_program(const ProgKey* k, u32 src)
{
    GLint ok = 0;
    GLuint p;
    cache_open();
    for (int i = 0; i < s_recN; i++) {
        const ProgRec* r = &s_recs[i];
        if (r->src != src || memcmp(&r->key, k, sizeof *k) != 0) continue;
        p = glCreateProgram();
        rgl_ProgramBinary(p, r->format, r->data, (GLsizei)r->len);
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) return p;
        glDeleteProgram(p);
        return 0;
    }
    return 0;
}

static void cache_save_program(const ProgKey* k, u32 src, GLuint p)
{
    GLint len = 0;
    GLsizei got = 0;
    GLenum format = 0;
    void* data;
    FILE* f;
    cache_open();
    if (!s_cacheMode) return;
    glGetProgramiv(p, 0x8741 /* PROGRAM_BINARY_LENGTH */, &len);
    if (len <= 0 || (data = malloc((size_t)len)) == NULL) return;
    rgl_GetProgramBinary(p, len, &got, &format, data);
    f = fopen(PROGCACHE_FILE, "ab");
    if (f != NULL && got > 0) {
        u32 u = (u32)got, fm = format;
        if (ftell(f) == 0 && (fseek(f, 0, SEEK_END), ftell(f)) == 0) {
            u32 head[3] = { PROGCACHE_MAGIC, sizeof(ProgKey), (u32)strlen(s_glVersion) };
            fwrite(head, sizeof head, 1, f);
            fwrite(s_glVersion, head[2], 1, f);
        }
        fwrite(k, sizeof *k, 1, f);
        fwrite(&src, 4, 1, f);
        fwrite(&fm, 4, 1, f);
        fwrite(&u, 4, 1, f);
        fwrite(data, u, 1, f);
    }
    if (f != NULL) fclose(f);
    free(data);
}

static Prog* prog_build(const ProgKey* k, const DVLE_s* dvle);

// The first draw from a vertex shader: every saved program of that shader
// made ready now, in one lend of the context, not one by one in the race.
static void cache_warm(int vs, const DVLE_s* dvle)
{
    int base = vs == VS_SOLID_INST ? VS_SOLID : vs;
    u64 t0;
    int n = 0;
    if (s_warmed[base]) return;
    s_warmed[base] = true;
    cache_open();
    if (!s_cacheMode || s_recN == 0) return;
    t0 = svcGetSystemTick();
    rgl_direct_begin();
    for (int i = 0; i < s_recN; i++) {
        int rv = s_recs[i].key.vs == VS_SOLID_INST ? VS_SOLID : s_recs[i].key.vs;
        if (rv != base) continue;
        ProgKey key = s_recs[i].key;
        if (prog_build(&key, dvle) != NULL) n++;
    }
    rgl_direct_end();
    printf("R36S: program cache: %d %s programs ready in %.0f ms\n", n, kVsName[base],
           (double)(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC);
}

static Prog* prog_build(const ProgKey* k, const DVLE_s* dvle)
{
    u32 h = key_hash(k) & (PROG_SLOTS - 1);
    Prog* p = NULL;
    char* fs;
    char what[48];
    u64 t0;
    cache_warm(k->vs, dvle);
    if (s_progs == NULL) {
        s_progs = (Prog*)calloc(PROG_SLOTS, sizeof(Prog));
        if (s_progs == NULL) return NULL;
    }
    for (u32 i = 0; i < PROG_SLOTS; i++) {
        Prog* q = &s_progs[(h + i) & (PROG_SLOTS - 1)];
        if (!q->used) { p = q; break; }
        if (memcmp(&q->key, k, sizeof *k) == 0) return q->gl ? q : NULL;
    }
    if (p == NULL) return NULL;
    t0 = svcGetSystemTick();
    memset(p, 0, sizeof *p);
    p->used = true;
    p->key = *k;
    // The link and the uniforms' locations need answers: one lend of the
    // context for all of them (glthread.c).
    rgl_direct_begin();
    fs = gen_fragment(k);
    snprintf(what, sizeof what, "%s", kVsName[k->vs]);
    if (fs != NULL) {
        u32 src = src_hash(kVsSource[k->vs], fs);
        p->gl = cache_load_program(k, src);
        if (p->gl == 0) {
            p->gl = r36sgl_link(kVsSource[k->vs], fs, what);
            if (p->gl != 0) {
                cache_save_program(k, src, p->gl);
                r36s_glcounts.progCompiled++;
            }
        }
    }
    free(fs);
    if (p->gl == 0) {
        rgl_direct_end();
        return NULL;
    }
    p->uDepth = glGetUniformLocation(p->gl, "uDepth");
    p->uConst = glGetUniformLocation(p->gl, "uConst");
    p->uBuf = glGetUniformLocation(p->gl, "uBuf");
    p->uAlphaRef = glGetUniformLocation(p->gl, "uAlphaRef");
    for (int u = 0; u < 3; u++) {
        char n[8];
        snprintf(n, sizeof n, "uTex%d", u);
        p->uTex[u] = glGetUniformLocation(p->gl, n);
    }
    for (int i = 0; i < dvle->numUniforms && p->nUni < MAX_UNIFORMS; i++) {
        const R36SPicaUniform* u = &dvle->uniforms[i];
        GLint loc = glGetUniformLocation(p->gl, u->name);
        if (loc < 0) continue;      // declared but unused: optimised out
        p->uni[p->nUni].loc = loc;
        p->uni[p->nUni].kind = u->kind;
        p->uni[p->nUni].reg = u->reg;
        p->uni[p->nUni].count = u->count ? u->count : 1;
        p->nUni++;
    }
    rgl_direct_end();
    r36s_glcounts.progBuilds++;
    r36s_glcounts.progUs += (unsigned)((double)(svcGetSystemTick() - t0) * 1000.0
                                       / CPU_TICKS_PER_MSEC);
    p->instMvp = p->instFog = p->instLight = -1;
    if (k->vs == VS_SOLID) {
        for (int i = 0; i < dvle->numUniforms; i++) {
            const R36SPicaUniform* u = &dvle->uniforms[i];
            if (u->kind != 'f') continue;
            if (strcmp(u->name, "mvp") == 0 && u->count >= 4) p->instMvp = u->reg;
            else if (strcmp(u->name, "fogModel") == 0) p->instFog = u->reg;
            else if (strcmp(u->name, "lightModel") == 0) p->instLight = u->reg;
        }
    }
    p->fresh = true;
    r36sgl_use_program(p->gl);
    for (int u = 0; u < 3; u++)
        if (p->uTex[u] >= 0) glUniform1i(p->uTex[u], u);
    return p;
}

static void upload_uniforms(Prog* p)
{
    const C3D_FVec* f = C3D_FVUnif[GPU_VERTEX_SHADER];
    u16 bools = C3D_BoolUnifs[GPU_VERTEX_SHADER];
    float depth[2] = { s.zScale, s.zOffset };
    for (int i = 0; i < p->nUni; i++) {
        const UniformMap* m = &p->uni[i];
        if (m->kind == 'f') {
            float v[24][4];
            int n = m->count > 24 ? 24 : m->count;
            for (int k = 0; k < n; k++) {
                const C3D_FVec* c = &f[(m->reg + k) % C3D_FVUNIF_COUNT];
                v[k][0] = c->x; v[k][1] = c->y; v[k][2] = c->z; v[k][3] = c->w;
            }
            if (p->fresh || memcmp(p->shadow[m->reg], v, (size_t)n * 16) != 0) {
                glUniform4fv(m->loc, n, &v[0][0]);
                r36s_glcounts.uniforms++;
                if (m->reg + n <= C3D_FVUNIF_COUNT) memcpy(p->shadow[m->reg], v, (size_t)n * 16);
            }
        } else if (m->kind == 'b') {
            bool on = (bools >> m->reg) & 1;
            if (p->fresh || (((p->boolShadow ^ bools) >> m->reg) & 1))
            {
                glUniform1i(m->loc, on ? 1 : 0);
                r36s_glcounts.uniforms++;
            }
        }
    }
    p->boolShadow = bools;
    if (p->uDepth >= 0 && (p->fresh || memcmp(p->depthShadow, depth, sizeof depth) != 0)) {
        glUniform2f(p->uDepth, depth[0], depth[1]);
        r36s_glcounts.uniforms++;
        memcpy(p->depthShadow, depth, sizeof depth);
    }
    if (p->uConst >= 0) {
        bool diff = p->fresh;
        for (int i = 0; i < 6 && !diff; i++) diff = p->constShadow[i] != s.env[i].color;
        if (diff) {
            float c[6][4];
            for (int i = 0; i < 6; i++) {
                u32 v = s.env[i].color;
                c[i][0] = (v & 0xFF) / 255.0f; c[i][1] = ((v >> 8) & 0xFF) / 255.0f;
                c[i][2] = ((v >> 16) & 0xFF) / 255.0f; c[i][3] = ((v >> 24) & 0xFF) / 255.0f;
                p->constShadow[i] = v;
            }
            glUniform4fv(p->uConst, 6, &c[0][0]);
            r36s_glcounts.uniforms++;
        }
    }
    if (p->uBuf >= 0 && (p->fresh || p->bufShadow != s.texEnvBufColor)) {
        u32 v = s.texEnvBufColor;
        glUniform4f(p->uBuf, (v & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
                    ((v >> 16) & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f);
        p->bufShadow = v;
        r36s_glcounts.uniforms++;
    }
    if (p->uAlphaRef >= 0 && (p->fresh || p->refShadow != s.alphaRef)) {
        glUniform1f(p->uAlphaRef, (float)s.alphaRef);
        p->refShadow = s.alphaRef;
        r36s_glcounts.uniforms++;
    }
    p->fresh = false;
}

// The key for this draw, with stages that pass the previous stage's
// output through made alike, so they do not multiply the shaders.
static void key_build(ProgKey* k, int vs)
{
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 6; i++) {
        const C3D_TexEnv* e = &s.env[i];
        k->srcRgb[i] = e->srcRgb;
        k->srcAlpha[i] = e->srcAlpha;
        k->opRgb[i] = (u16)e->opRgb;
        k->opAlpha[i] = (u16)e->opAlpha;
        k->funcRgb[i] = (u8)e->funcRgb;
        k->funcAlpha[i] = (u8)e->funcAlpha;
        k->scaleRgb[i] = (u8)e->scaleRgb;
        k->scaleAlpha[i] = (u8)e->scaleAlpha;
        // A REPLACE of the first source only reads that source.
        if (e->funcRgb == GPU_REPLACE) { k->srcRgb[i] &= 0xF; k->opRgb[i] &= 0xF; }
        if (e->funcAlpha == GPU_REPLACE) { k->srcAlpha[i] &= 0xF; k->opAlpha[i] &= 0xF; }
    }
    k->bufRgb = (u8)((s.texEnvBuf >> 8) & 0xF);
    k->bufAlpha = (u8)((s.texEnvBuf >> 12) & 0xF);
    k->alphaFunc = (u8)(s.alphaTest ? s.alphaFunc : GPU_ALWAYS);
    {
        // The sources the stages read, as src_uses finds them, in one pass.
        u32 used = 0;
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 3; j++)
                used |= 1u << ((k->srcRgb[i] >> (4 * j)) & 0xF)
                      | 1u << ((k->srcAlpha[i] >> (4 * j)) & 0xF);
        for (int u = 0; u < 3; u++) {
            C3D_Tex* t = s.tex[u];
            if (t != NULL && ((used >> (GPU_TEXTURE0 + u)) & 1u))
                k->texType[u] = tex_mode(t) == GPU_TEX_CUBE_MAP ? 2 : 1;
        }
    }
    k->vs = (u8)vs;
}

// key_build's answer, kept until something it reads changes: the stages,
// the buffer's and the alpha test's (s_keyStale), the bound textures and
// their kind (2D or cube, which a texture's own setup may change), and the
// vertex shader. Most draws of a frame come in runs of one material, and
// were building it twice each (try_hold, then prepare_draw).
// NFSHP2_KEYCACHE=0 builds it every time; =2 builds it every time too and
// logs where the kept one differed (and uses the built one).
static struct {
    bool           valid;
    int            vs;
    const C3D_Tex* tex[3];
    u32            texParam[3];
    ProgKey        key;
} s_kc;
static int      s_kcMode = -1;
static unsigned s_kcHits, s_kcBuilds, s_kcWrong;

static void key_get(ProgKey* k, int vs)
{
    bool same;

    if (s_kcMode < 0) {
        const char* e = getenv("NFSHP2_KEYCACHE");
        s_kcMode = e != NULL && e[0] != '\0' ? atoi(e) : 1;
        if (s_kcMode < 0 || s_kcMode > 2) s_kcMode = 1;
        if (s_kcMode != 1) printf("R36S: shader key cache %s\n", s_kcMode ? "checked" : "off");
    }
    if (s_kcMode == 0) {
        key_build(k, vs);
        return;
    }
    same = s_kc.valid && !s_keyStale && s_kc.vs == vs;
    for (int u = 0; u < 3 && same; u++)
        same = s_kc.tex[u] == s.tex[u]
            && (s.tex[u] == NULL || s_kc.texParam[u] == (s.tex[u]->param & 0x70000000u));
    if (s_kcMode == 2 && (s_kcHits + s_kcBuilds) % 20000 == 19999)
        printf("R36S: shader key cache: %u kept, %u built, %u stale\n", s_kcHits, s_kcBuilds,
               s_kcWrong);
    if (same) {
        s_kcHits++;
        *k = s_kc.key;
        if (s_kcMode == 2) {
            ProgKey b;
            key_build(&b, vs);
            if (memcmp(&b, k, sizeof b) != 0) {
                if (s_kcWrong++ < 20) printf("R36S: shader key cache: a kept key was stale\n");
                *k = b;
                s_kc.key = b;
            }
        }
        return;
    }
    s_kcBuilds++;
    key_build(k, vs);
    if (s_kcMode == 2) {
        // key_build's one-pass source mask against src_uses, source by source.
        for (int u = 0; u < 3; u++) {
            bool want = s.tex[u] != NULL && src_uses(k, GPU_TEXTURE0 + u);
            if (want != (k->texType[u] != 0) && s_kcWrong++ < 20)
                printf("R36S: shader key: texture %d's use differs from src_uses\n", u);
        }
    }
    s_kc.valid = true;
    s_kc.vs = vs;
    for (int u = 0; u < 3; u++) {
        s_kc.tex[u] = s.tex[u];
        s_kc.texParam[u] = s.tex[u] != NULL ? (s.tex[u]->param & 0x70000000u) : 0;
    }
    s_kc.key = *k;
    s_keyStale = false;
}

// vs_of's answer for the last shader asked about (its name searched for
// the twins' names each draw, twice). DVLB_Free forgets it.
static const DVLE_s* s_vsLast;
static int           s_vsLastVs = -1;

static int vs_of_cached(const DVLE_s* dvle)
{
    if (dvle != s_vsLast) {
        s_vsLast = dvle;
        s_vsLastVs = vs_of(dvle);
    }
    return s_vsLastVs;
}

// ------------------------------------------------------------ draws

static GLenum gl_prim(GPU_Primitive_t p)
{
    switch (p) {
    case GPU_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case GPU_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    default:                 return GL_TRIANGLES;
    }
}

// The attributes from the game's buffers, as the PICA's loader reads
// them: each buffer's attributes in its permutation's order, each aligned
// to its element's size, nibbles 0xC-0xF padding of 4 to 16 bytes.
static void setup_vertices(void)
{
    static const GLenum type[4] = { GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT };
    static const int elem[4] = { 1, 1, 2, 4 };
    unsigned mask = 0;
    int live = 0;
    for (int b = 0; b < s.buf.bufCount; b++)
        if (s.buf.r36s_vaddr[b] != NULL) live++;
    if (live == 0) r36sgl_vertex_source(NULL, false);
    for (int b = 0; b < s.buf.bufCount; b++) {
        const C3D_BufCfg* cfg = &s.buf.buffers[b];
        const u8* base = (const u8*)s.buf.r36s_vaddr[b];
        u64 perm = (u64)cfg->flags[0] | ((u64)(cfg->flags[1] & 0xFFFF) << 32);
        int stride = (int)((cfg->flags[1] >> 16) & 0xFFF);
        int count = (int)(cfg->flags[1] >> 28);
        u32 off = 0;
        if (base == NULL) continue;
        // A baked mesh's GPU buffer, or the game's memory (c3d_buffers.c).
        base = (const u8*)r36sgl_vertex_source(base, live == 1);
        for (int k = 0; k < count; k++) {
            unsigned id = (unsigned)((perm >> (4 * k)) & 0xF);
            if (id >= 12) {
                off += 4 * (id - 11);
                continue;
            }
            u32 fmtBits = id < 8 ? (s.attr.flags[0] >> (id * 4)) : (s.attr.flags[1] >> ((id - 8) * 4));
            unsigned fmt = fmtBits & 3, n = ((fmtBits >> 2) & 3) + 1;
            unsigned reg = (unsigned)((s.attr.permutation >> (4 * id)) & 0xF);
            off = (off + (u32)elem[fmt] - 1) & ~((u32)elem[fmt] - 1);
            if (reg < 8) {
                r36sgl_attrib_pointer(reg, (GLint)n, type[fmt], GL_FALSE, stride, base + off);
                mask |= 1u << reg;
            }
            off += (u32)elem[fmt] * n;
        }
    }
    r36sgl_attrib_mask(mask);
}

static Prog* prepare_draw(void)
{
    ProgKey key;
    int vs;
    Prog* p;
    s_dbg.asked++;
    r36sgl_flush2d();
    if (!r36sgl_begin_draw()) { s_dbg.noTarget++; return NULL; }
    if (s.program == NULL || s.program->vertexShader == NULL || s.program->vertexShader->dvle == NULL) {
        s_dbg.noProgram++;
        return NULL;
    }
    vs = vs_of_cached(s.program->vertexShader->dvle);
    if (vs < 0) { s_dbg.noTwin++; return NULL; }
    key_get(&key, vs);
    if (s_lastProg != NULL && memcmp(&key, &s_lastKey, sizeof key) == 0) {
        p = s_lastProg;
    } else {
        p = prog_build(&key, s.program->vertexShader->dvle);
        if (p == NULL) { s_dbg.noLink++; return NULL; }
        s_lastProg = p;
        s_lastKey = key;
    }
    r36sgl_use_program(p->gl);
    upload_uniforms(p);
    for (int u = 0; u < 3; u++)
        if (key.texType[u]) r36sgl_bind_texture(u, s.tex[u]);
    setup_vertices();
    s_dbg.drawn++;
    r36s_glcounts.draws++;
    return p;
}

// ------------------------------------------ grouped (instanced) draws
//
// docs/R36S_OPTIMISATION_BACKLOG.md, item 12. The Mali driver costs
// about 39 us a draw whatever the draw, and a race frame draws the same
// tree, rock or sign mesh many times with the same state: only the
// model's uniforms (geosolid's mvp, fogModel and lightModel) differ. So a
// geosolid draw that is opaque (depth test and write on, no blend, every
// colour channel written), from a mesh's GPU buffer, waits ("held"); the
// held draws with equal state and mesh become one instanced draw of
// geosolid's twin (kVsGeoSolidInst), the model's uniforms per instance.
//
// Order: opaque draws that test and write depth make the same picture in
// any order (but for exact ties in depth), so held draws may pass each
// other and lit or client-array opaque draws, which go at once. Anything
// else -- a blended draw, one that doesn't write depth or all colours,
// citro2d's batch, a clear, a target change, a texture or mesh about to
// be uploaded, a texture or target deleted, the frame's end -- draws the
// held ones first (r36sgl_flush_held). A group goes where its first draw
// was; its other draws come forward to it.
//
// NFSHP2_INSTANCE=0 turns it off: every draw goes at once, as before.

#define HELD_MAX 4096
#define HELD_UNI 4      // vec4s of shared uniforms a held draw keeps

typedef struct {
    u8     reg, size;
    u16    stride;
    GLenum type;
    u32    off;
} AttrSpec;

typedef struct {
    // What two draws must share to be one: all but the model's uniforms.
    // Zeroed before it is filled, so memcmp compares it.
    struct {
        Prog*     prog;          // the instanced twin
        FixedSnap fixed;
        GLuint    fbo;
        GLint     vp[4];
        GLuint    tex[3];
        GLenum    texTarget[3];
        GLuint    vbo, ibo;
        uintptr_t ioff;          // the indices' offset, or the first vertex
        u32       count;
        GLenum    prim, itype;   // itype 0: an array draw
        u32       nAttr;
        AttrSpec  attr[4];
        u32       bools;
        float     depth[2];
        u32       env[6], buf;
        int       alphaRef;
        u32       nUni;
        float     uni[HELD_UNI][4];
    } k;
    u32   hash;
    float inst[6][4];            // mvp's rows, fogModel, lightModel
} Held;


static int    s_instMode = -1;   // NFSHP2_INSTANCE
static Held*  s_held;
static int    s_heldN;
static int*   s_heldOrder;       // HELD_MAX: sorted; HELD_MAX more: links
static float* s_instData;        // HELD_MAX * 24 floats
static GLuint s_instVbo;

static bool inst_on(void)
{
    if (s_instMode < 0) {
        const char* e = getenv("NFSHP2_INSTANCE");
        s_instMode = e != NULL && e[0] != '\0' ? atoi(e) : 1;
        if (s_instMode && strstr(s_glVersion, "OpenGL ES 3") == NULL) {
            printf("R36S: grouped draws off: \"%s\" is not GLES 3\n", s_glVersion);
            s_instMode = 0;
        }
        // Midgard (Mali-T6xx to T8xx; the RG552's T860) draws grouped
        // scenery wrong: ground and road vanish and rocks stretch into
        // spikes, with the instance data aligned to 64 bytes too. Its CPU
        // is fast enough that grouping doesn't help there (RG552: 33-44 fps
        // without, 27-34 with). NFSHP2_INSTANCE=1 still turns it on.
        if (s_instMode && (e == NULL || e[0] == '\0') && strstr(s_glRenderer, "Mali-T") != NULL) {
            printf("R36S: grouped draws off: %s draws them wrong\n", s_glRenderer);
            s_instMode = 0;
        }
        if (s_instMode) {
            s_held = (Held*)malloc(HELD_MAX * sizeof *s_held);
            s_heldOrder = (int*)malloc(2 * HELD_MAX * sizeof *s_heldOrder);
            s_instData = (float*)malloc(HELD_MAX * 24 * sizeof *s_instData);
            if (!rgl_instancing() || s_held == NULL || s_heldOrder == NULL || s_instData == NULL) {
                printf("R36S: grouped draws off: no GLES 3 instancing\n");
                s_instMode = 0;
            } else {
                glGenBuffers(1, &s_instVbo);
                for (GLuint a = 4; a < 10; a++) rgl_VertexAttribDivisor(a, 1);
            }
        }
        printf("R36S: grouped draws %s (NFSHP2_INSTANCE=%d)\n", s_instMode ? "on" : "off",
               s_instMode);
    }
    return s_instMode != 0;
}

// Whether binding `t` would upload it or change its sampling: what a held
// draw that reads it must not see happen after it.
static bool tex_would_change(C3D_Tex* t)
{
    if (!tex_gl_live(t) && !tex_try_share(t)) return true;
    if (t->r36s_dirty && !t->r36s_rt && !cube_is_rt(t->r36s_gl)) return true;
    return t->r36s_param != (t->param | 0x80000000u);
}

static bool fixed_order_free(const FixedSnap* f)
{
    return f->depthTest && (f->writeMask & GPU_WRITE_DEPTH) && (f->writeMask & 0xF) == 0xF
        && !fixed_blends(f);
}

// Buffer 0's attributes as setup_vertices lays them out, with offsets from
// the buffer's start. False when there are more than four, or one is past
// location 3 (4..9 are the instances').
static bool attr_specs(AttrSpec* out, u32* n)
{
    static const GLenum type[4] = { GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT };
    static const int elem[4] = { 1, 1, 2, 4 };
    const C3D_BufCfg* cfg = &s.buf.buffers[0];
    u64 perm = (u64)cfg->flags[0] | ((u64)(cfg->flags[1] & 0xFFFF) << 32);
    int stride = (int)((cfg->flags[1] >> 16) & 0xFFF);
    int count = (int)(cfg->flags[1] >> 28);
    u32 off = 0;
    *n = 0;
    for (int k = 0; k < count; k++) {
        unsigned id = (unsigned)((perm >> (4 * k)) & 0xF);
        if (id >= 12) {
            off += 4 * (id - 11);
            continue;
        }
        u32 fmtBits = id < 8 ? (s.attr.flags[0] >> (id * 4)) : (s.attr.flags[1] >> ((id - 8) * 4));
        unsigned fmt = fmtBits & 3, sz = ((fmtBits >> 2) & 3) + 1;
        unsigned reg = (unsigned)((s.attr.permutation >> (4 * id)) & 0xF);
        off = (off + (u32)elem[fmt] - 1) & ~((u32)elem[fmt] - 1);
        if (reg < 8) {
            if (reg > 3 || *n == 4) return false;
            out[*n].reg = (u8)reg;
            out[*n].size = (u8)sz;
            out[*n].stride = (u16)stride;
            out[*n].type = type[fmt];
            out[*n].off = off;
            (*n)++;
        }
        off += (u32)elem[fmt] * sz;
    }
    return true;
}

// A word at a time: this runs for every held draw.
static u32 held_hash(const void* p, size_t n)
{
    const u32* w = (const u32*)p;
    u32 h = 2166136261u;
    for (size_t i = 0; i < n / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

// NFSHP2_GLDEBUG's count of the draws not held, by reason.
static unsigned s_holdWhy[11];

// Holds this draw for grouping, if it can be: true when held.
static bool try_hold(GPU_Primitive_t primitive, u32 count, GLenum itype, const void* indices,
                     u32 first)
{
    C3D_RenderTarget* t = s.target;
    const DVLE_s* dvle;
    const C3D_FVec* f = C3D_FVUnif[GPU_VERTEX_SHADER];
    FixedSnap fx;
    ProgKey key;
    Prog *p, *ip;
    Held* h;
    GLuint vbo, ibo;
    uintptr_t ioff;
    int vs;
    u32 v = 0;

    if (!s_gl || t == NULL || !s.inFrame || gl_skip() || !inst_on()) return false;
    if (s.program == NULL || s.program->vertexShader == NULL
        || (dvle = s.program->vertexShader->dvle) == NULL)
        return false;
    vs = vs_of_cached(dvle);
    if (vs != VS_SOLID || s.buf.bufCount != 1 || s.buf.r36s_vaddr[0] == NULL) { s_holdWhy[1]++; return false; }
    fixed_snap(&fx);
    if (!fixed_order_free(&fx)) {
        s_holdWhy[2]++;
        if (!fx.depthTest) s_holdWhy[0] += 1u;
        else if (!(fx.writeMask & GPU_WRITE_DEPTH)) s_holdWhy[0] += 1u << 10;
        else if (fixed_blends(&fx)) s_holdWhy[0] += 1u << 20;
        return false;
    }
    if (r36sgl_mesh_dirty(s.buf.r36s_vaddr[0])) { s_holdWhy[3]++; return false; }
    key_get(&key, vs);
    for (int u = 0; u < 3; u++)
        if (key.texType[u] && (s.tex[u] == NULL || tex_would_change(s.tex[u]))) { s_holdWhy[4]++; return false; }
    if (!target_ready(t)) { s_holdWhy[5]++; return false; }
    if (s_lastProg != NULL && memcmp(&key, &s_lastKey, sizeof key) == 0) {
        p = s_lastProg;
    } else {
        p = prog_build(&key, dvle);
        if (p == NULL) return false;
        s_lastProg = p;
        s_lastKey = key;
    }
    if (p->instMvp < 0 || p->instFog < 0 || p->instLight < 0) { s_holdWhy[6]++; return false; }
    if (!p->instTried) {
        ProgKey ik = key;
        ik.vs = VS_SOLID_INST;
        p->instTried = true;
        p->inst = prog_build(&ik, dvle);
        if (p->inst == NULL) printf("R36S: no instanced twin for a geosolid combiner\n");
    }
    if ((ip = p->inst) == NULL) { s_holdWhy[7]++; return false; }
    if (!r36sgl_mesh_ref(s.buf.r36s_vaddr[0], itype ? indices : NULL, &vbo, &ibo, &ioff))
        { s_holdWhy[8]++; return false; }

    // citro2d's batch, and the held draws before it, come before this.
    r36sgl_flush2d();
    if (s_heldN == HELD_MAX) r36sgl_flush_held();
    h = &s_held[s_heldN];
    memset(&h->k, 0, sizeof h->k);
    if (!attr_specs(h->k.attr, &h->k.nAttr)) { s_holdWhy[9]++; return false; }
    for (int i = 0; i < ip->nUni; i++) {
        const UniformMap* m = &ip->uni[i];
        if (m->kind != 'f') continue;
        int n = m->count > 24 ? 24 : m->count;
        if (v + (u32)n > HELD_UNI) { s_holdWhy[10]++; return false; }
        for (int k = 0; k < n; k++, v++) {
            const C3D_FVec* c = &f[(m->reg + k) % C3D_FVUNIF_COUNT];
            h->k.uni[v][0] = c->x; h->k.uni[v][1] = c->y; h->k.uni[v][2] = c->z; h->k.uni[v][3] = c->w;
        }
    }
    h->k.nUni = v;
    h->k.prog = ip;
    h->k.fixed = fx;
    h->k.fbo = t->frameBuf.r36s_fbo;
    if (t->linked) {
        target_size(t, &h->k.vp[2], &h->k.vp[3]);
    } else {
        for (int i = 0; i < 4; i++) h->k.vp[i] = (GLint)s.viewport[i];
    }
    for (int u = 0; u < 3; u++) {
        if (!key.texType[u]) continue;
        h->k.tex[u] = s.tex[u]->r36s_gl;
        h->k.texTarget[u] = tex_mode(s.tex[u]) == GPU_TEX_CUBE_MAP ? GL_TEXTURE_CUBE_MAP
                                                                   : GL_TEXTURE_2D;
    }
    h->k.vbo = vbo;
    h->k.ibo = ibo;
    h->k.ioff = itype ? ioff : first;
    h->k.count = count;
    h->k.prim = gl_prim(primitive);
    h->k.itype = itype;
    h->k.bools = C3D_BoolUnifs[GPU_VERTEX_SHADER];
    h->k.depth[0] = s.zScale;
    h->k.depth[1] = s.zOffset;
    for (int i = 0; i < 6; i++) h->k.env[i] = s.env[i].color;
    h->k.buf = s.texEnvBufColor;
    h->k.alphaRef = s.alphaRef;
    for (int r = 0; r < 4; r++) {
        const C3D_FVec* c = &f[(p->instMvp + r) % C3D_FVUNIF_COUNT];
        h->inst[r][0] = c->x; h->inst[r][1] = c->y; h->inst[r][2] = c->z; h->inst[r][3] = c->w;
    }
    {
        const C3D_FVec* c = &f[p->instFog % C3D_FVUNIF_COUNT];
        h->inst[4][0] = c->x; h->inst[4][1] = c->y; h->inst[4][2] = c->z; h->inst[4][3] = c->w;
        c = &f[p->instLight % C3D_FVUNIF_COUNT];
        h->inst[5][0] = c->x; h->inst[5][1] = c->y; h->inst[5][2] = c->z; h->inst[5][3] = c->w;
    }
    h->hash = held_hash(&h->k, sizeof h->k);
    s_heldN++;
    s_dbg.drawn++;
    r36s_glcounts.held++;
    return true;
}

// Before a draw that goes at once: the held draws first, if this one must
// follow them -- it isn't order-free, or it will upload a texture or a
// mesh the held ones may read.
static void held_before(void)
{
    FixedSnap fx;
    bool flush;
    if (s_heldN == 0) return;
    fixed_snap(&fx);
    flush = !fixed_order_free(&fx);
    for (int u = 0; u < 3 && !flush; u++)
        if (s.tex[u] != NULL && tex_would_change(s.tex[u])) flush = true;
    for (int b = 0; b < s.buf.bufCount && !flush; b++)
        if (r36sgl_mesh_dirty(s.buf.r36s_vaddr[b])) flush = true;
    if (flush) r36sgl_flush_held();
}

// The shared uniforms of a group, as upload_uniforms sends a draw's.
static void upload_held_uniforms(Prog* p, const Held* h)
{
    u32 v = 0;
    for (int i = 0; i < p->nUni; i++) {
        const UniformMap* m = &p->uni[i];
        if (m->kind == 'f') {
            int n = m->count > 24 ? 24 : m->count;
            const float* d = h->k.uni[v];
            v += (u32)n;
            if (p->fresh || memcmp(p->shadow[m->reg], d, (size_t)n * 16) != 0) {
                glUniform4fv(m->loc, n, d);
                r36s_glcounts.uniforms++;
                if (m->reg + n <= C3D_FVUNIF_COUNT) memcpy(p->shadow[m->reg], d, (size_t)n * 16);
            }
        } else if (m->kind == 'b') {
            bool on = (h->k.bools >> m->reg) & 1;
            if (p->fresh || (((p->boolShadow ^ h->k.bools) >> m->reg) & 1)) {
                glUniform1i(m->loc, on ? 1 : 0);
                r36s_glcounts.uniforms++;
            }
        }
    }
    p->boolShadow = (u16)h->k.bools;
    if (p->uDepth >= 0 && (p->fresh || memcmp(p->depthShadow, h->k.depth, sizeof h->k.depth) != 0)) {
        glUniform2f(p->uDepth, h->k.depth[0], h->k.depth[1]);
        r36s_glcounts.uniforms++;
        memcpy(p->depthShadow, h->k.depth, sizeof h->k.depth);
    }
    if (p->uConst >= 0) {
        bool diff = p->fresh;
        for (int i = 0; i < 6 && !diff; i++) diff = p->constShadow[i] != h->k.env[i];
        if (diff) {
            float c[6][4];
            for (int i = 0; i < 6; i++) {
                u32 x = h->k.env[i];
                c[i][0] = (x & 0xFF) / 255.0f; c[i][1] = ((x >> 8) & 0xFF) / 255.0f;
                c[i][2] = ((x >> 16) & 0xFF) / 255.0f; c[i][3] = ((x >> 24) & 0xFF) / 255.0f;
                p->constShadow[i] = x;
            }
            glUniform4fv(p->uConst, 6, &c[0][0]);
            r36s_glcounts.uniforms++;
        }
    }
    if (p->uBuf >= 0 && (p->fresh || p->bufShadow != h->k.buf)) {
        u32 x = h->k.buf;
        glUniform4f(p->uBuf, (x & 0xFF) / 255.0f, ((x >> 8) & 0xFF) / 255.0f,
                    ((x >> 16) & 0xFF) / 255.0f, ((x >> 24) & 0xFF) / 255.0f);
        p->bufShadow = x;
        r36s_glcounts.uniforms++;
    }
    if (p->uAlphaRef >= 0 && (p->fresh || p->refShadow != h->k.alphaRef)) {
        glUniform1f(p->uAlphaRef, (float)h->k.alphaRef);
        p->refShadow = h->k.alphaRef;
        r36s_glcounts.uniforms++;
    }
    p->fresh = false;
}

// Order-free groups, sorted to change program and texture least.
static const int* s_sortLead;

static int group_cmp(const void* a, const void* b)
{
    const Held* x = &s_held[s_sortLead[*(const int*)a]];
    const Held* y = &s_held[s_sortLead[*(const int*)b]];
    if (x->k.prog != y->k.prog) return x->k.prog < y->k.prog ? -1 : 1;
    for (int u = 0; u < 3; u++)
        if (x->k.tex[u] != y->k.tex[u]) return x->k.tex[u] < y->k.tex[u] ? -1 : 1;
    if (x->k.vbo != y->k.vbo) return x->k.vbo < y->k.vbo ? -1 : 1;
    return s_sortLead[*(const int*)a] - s_sortLead[*(const int*)b];
}

static int held_cmp(const void* a, const void* b)
{
    int x = *(const int*)a, y = *(const int*)b;
    if (s_held[x].hash != s_held[y].hash) return s_held[x].hash < s_held[y].hash ? -1 : 1;
    return x - y;
}

void r36sgl_flush_held(void)
{
    int n = s_heldN, groups = 0, used = 0;
    int* order = s_heldOrder;
    int* link = s_heldOrder + HELD_MAX;   // a group's next member, or -1
    // Group i: its first member (the leader), last member and size, kept
    // in the leader's slot of these.
    static int lead[HELD_MAX], tail[HELD_MAX], size[HELD_MAX], at[HELD_MAX];

    if (n == 0) return;
    s_heldN = 0;
    for (int i = 0; i < n; i++) order[i] = i;
    qsort(order, (size_t)n, sizeof *order, held_cmp);
    // Groups within each run of one hash, by contents; leaders by index.
    for (int r = 0; r < n; ) {
        int e = r, first = groups;
        while (e < n && s_held[order[e]].hash == s_held[order[r]].hash) e++;
        for (int q = r; q < e; q++) {
            int i = order[q], gi = first;
            while (gi < groups && memcmp(&s_held[lead[gi]].k, &s_held[i].k, sizeof s_held[i].k) != 0)
                gi++;
            link[i] = -1;
            if (gi == groups) {
                lead[groups] = tail[groups] = i;
                size[groups] = 1;
                groups++;
            } else {
                link[tail[gi]] = i;
                tail[gi] = i;
                size[gi]++;
            }
        }
        r = e;
    }
    // Draw the groups where their first draws were: by leader.
    for (int gi = 0; gi < groups; gi++) order[gi] = gi;
    {
        // Insertion order of leaders: a counting pass over held indices.
        static int byLead[HELD_MAX];
        for (int i = 0; i < n; i++) byLead[i] = -1;
        for (int gi = 0; gi < groups; gi++) byLead[lead[gi]] = gi;
        groups = 0;
        for (int i = 0; i < n; i++)
            if (byLead[i] >= 0) order[groups++] = byLead[i];
    }
    {
        static int s_sortMode = -1;
        if (s_sortMode < 0) {
            const char* e = getenv("NFSHP2_INSTANCE_SORT");
            s_sortMode = e == NULL || e[0] != '0';
        }
        if (s_sortMode) {
            s_sortLead = lead;
            qsort(order, (size_t)groups, sizeof *order, group_cmp);
        }
    }
    // Every instance's data, group by group, in one upload.
    for (int q = 0; q < groups; q++) {
        int gi = order[q];
        at[gi] = used;
        for (int i = lead[gi]; i >= 0; i = link[i])
            memcpy(&s_instData[(size_t)used++ * 24], s_held[i].inst, sizeof s_held[i].inst);
    }
    r36sgl_bind_array(s_instVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)used * 96, s_instData, GL_STREAM_DRAW);
    for (int q = 0; q < groups; q++) {
        int gi = order[q];
        const Held* h = &s_held[lead[gi]];
        Prog* p = h->k.prog;
        unsigned mask = 0x3F0u;
        bind_fbo(h->k.fbo);
        set_viewport(h->k.vp[0], h->k.vp[1], h->k.vp[2], h->k.vp[3]);
        apply_fixed_from(&h->k.fixed);
        r36sgl_use_program(p->gl);
        upload_held_uniforms(p, h);
        for (int u = 0; u < 3; u++)
            if (h->k.tex[u] != 0) bind_gl_texture(u, h->k.texTarget[u], h->k.tex[u]);
        r36sgl_bind_array(h->k.vbo);
        for (u32 a = 0; a < h->k.nAttr; a++) {
            const AttrSpec* sp = &h->k.attr[a];
            r36sgl_attrib_pointer(sp->reg, sp->size, sp->type, GL_FALSE, sp->stride,
                                  (const void*)(uintptr_t)sp->off);
            mask |= 1u << sp->reg;
        }
        r36sgl_bind_array(s_instVbo);
        for (int k = 0; k < 6; k++)
            r36sgl_attrib_pointer((GLuint)(4 + k), 4, GL_FLOAT, GL_FALSE, 96,
                                  (const void*)(uintptr_t)((size_t)at[gi] * 96 + (size_t)k * 16));
        r36sgl_attrib_mask(mask);
        if (h->k.itype != 0) {
            r36sgl_bind_element(h->k.ibo);
            rgl_DrawElementsInstanced(h->k.prim, (GLsizei)h->k.count, h->k.itype,
                               (const void*)h->k.ioff, size[gi]);
        } else {
            rgl_DrawArraysInstanced(h->k.prim, (GLint)h->k.ioff, (GLsizei)h->k.count, size[gi]);
        }
        r36s_glcounts.draws++;
        r36s_glcounts.groups++;
    }
}

// NFSHP2_DRAWSTATS=N: every N frames, how that frame's draws could merge.
// Each draw's state (program, textures, target, fixed state) and geometry
// (buffer, first index, count) are hashed; a clear or a target change
// splits the frame into passes. Within a pass, the draws between two
// blended ones could be grouped in any order: "by mesh" counts the
// draws left if each group of the same state and geometry became one
// instanced draw (solid, unlit draws only, as the lit twin's per-model
// lights would need instancing too), "same state" counts draws whose
// state matches the draw before.
#define DS_MAX 16384
typedef struct { u32 state, geom; u8 blend, lit; } DrawRec;
static DrawRec* s_ds;
static int s_dsN, s_dsEvery = -1;

static u32 ds_mix(u32 h, u32 v) { return (h ^ v) * 16777619u; }

static void ds_note(const Prog* p, const void* geomBase, u32 first, u32 count)
{
    u32 st = 2166136261u, ge = 2166136261u;
    if (s_dsEvery < 0) {
        const char* e = getenv("NFSHP2_DRAWSTATS");
        s_dsEvery = e ? atoi(e) : 0;
        if (s_dsEvery > 0) s_ds = (DrawRec*)malloc(DS_MAX * sizeof *s_ds);
        if (s_ds == NULL) s_dsEvery = 0;
    }
    if (s_dsEvery <= 0 || s.frames % (u32)s_dsEvery != 0 || s_dsN >= DS_MAX) return;
    st = ds_mix(st, p->gl);
    for (int u = 0; u < 3; u++) st = ds_mix(st, g.boundTex[u]);
    st = ds_mix(st, g.fbo);
    st = ds_mix(st, (u32)g.depthTest | (u32)g.depthMask << 2 | (u32)g.colorMask << 4
                    | (u32)(g.cull & 0xFFFF) << 8);
    st = ds_mix(st, g.depthFunc);
    st = ds_mix(st, (u32)g.blend);
    st = ds_mix(st, g.blendFn[0] ^ g.blendFn[1] << 4 ^ g.blendFn[2] << 8 ^ g.blendFn[3] << 12);
    st = ds_mix(st, s_dsPass);
    ge = ds_mix(ge, (u32)(uintptr_t)geomBase);
    ge = ds_mix(ge, first);
    ge = ds_mix(ge, count);
    s_ds[s_dsN].state = st;
    s_ds[s_dsN].geom = ge;
    s_ds[s_dsN].blend = (u8)(g.blend == 1);
    s_ds[s_dsN].lit = (u8)(p->key.vs != VS_SOLID);
    s_dsN++;
}

static int ds_cmp(const void* a, const void* b)
{
    const DrawRec* x = (const DrawRec*)a;
    const DrawRec* y = (const DrawRec*)b;
    if (x->state != y->state) return x->state < y->state ? -1 : 1;
    if (x->geom != y->geom) return x->geom < y->geom ? -1 : 1;
    return 0;
}

static void ds_report(void)
{
    int blended = 0, lit = 0, same = 0, byMesh = 0, runs = 0, adjacent = 0, adjSolid = 0;
    if (s_dsN == 0) return;
    for (int i = 0; i < s_dsN; i++) {
        blended += s_ds[i].blend;
        lit += s_ds[i].lit;
        if (i > 0 && s_ds[i].state == s_ds[i - 1].state) {
            same++;
            if (s_ds[i].geom == s_ds[i - 1].geom) {
                adjacent++;
                if (!s_ds[i].lit) adjSolid++;
            }
        }
    }
    printf("drawstats: frame %u: the same state and mesh as the draw before: %d (%d unlit)\n",
           (unsigned)s.frames, adjacent, adjSolid);
    // Runs of unblended draws in one pass; each blended draw stays itself.
    for (int i = 0; i < s_dsN; ) {
        int j = i, solid = 0;
        if (s_ds[i].blend) { byMesh++; i++; continue; }
        while (j < s_dsN && !s_ds[j].blend) j++;
        runs++;
        // The lit draws stay as they are; the solid ones group by state and mesh.
        for (int k = i; k < j; k++) {
            if (s_ds[k].lit) byMesh++;
            else s_ds[i + solid++] = s_ds[k];
        }
        qsort(&s_ds[i], (size_t)solid, sizeof *s_ds, ds_cmp);
        for (int k = 0; k < solid; k++)
            if (k == 0 || ds_cmp(&s_ds[i + k], &s_ds[i + k - 1]) != 0) byMesh++;
        i = j;
    }
    printf("drawstats: frame %u: %d draws (%d blended, %d lit); same state as the draw before %d;"
           " %d unblended runs; by mesh %d draws\n",
           (unsigned)s.frames, s_dsN, blended, lit, same, runs, byMesh);
    s_dsN = 0;
}

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
    Prog* p;
    if (size <= 0) return;
    if (try_hold(primitive, (u32)size, 0, NULL, (u32)first)) return;
    held_before();
    if ((p = prepare_draw()) == NULL) return;
    ds_note(p, s.buf.r36s_vaddr[0], (u32)first, (u32)size);
    glDrawArrays(gl_prim(primitive), first, size);
}

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices)
{
    Prog* p;
    if (count <= 0) return;
    if (try_hold(primitive, (u32)count,
                 type == C3D_UNSIGNED_SHORT ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, indices, 0))
        return;
    held_before();
    if ((p = prepare_draw()) == NULL) return;
    ds_note(p, s.buf.r36s_vaddr[0], (u32)(uintptr_t)indices, (u32)count);
    glDrawElements(gl_prim(primitive), count,
                   type == C3D_UNSIGNED_SHORT ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                   r36sgl_index_source(indices));
}

void C3D_ImmDrawBegin(GPU_Primitive_t primitive) { (void)primitive; }
void C3D_ImmSendAttrib(float x, float y, float z, float w) { (void)x; (void)y; (void)z; (void)w; }
void C3D_ImmDrawEnd(void) {}

// ----------------------------------------------------------- frames

// Whether this frame shows the bottom screen's panel (3ds.h): the top
// target over the whole screen and, when this is set, the bottom target as
// a panel over its lower half.
static bool s_bottomPopup;

void r36s_bottom_popup(bool on) { s_bottomPopup = on; }

float C3D_FrameRate(float fps) { (void)fps; return 60.0f; }

bool C3D_FrameBegin(u8 flags)
{
    (void)flags;
    if (s.inFrame) return false;
    s.inFrame = true;
    s.frameStartTick = svcGetSystemTick();
    s_frameScale = r36s_render_scale();
    return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target)
{
    if (!s.inFrame) return false;
    r36sgl_flush2d();
    r36sgl_flush_held();
    s_dsPass++;
    s.target = target;
    if (target != NULL) {
        target->used = true;
        s.fb = target->frameBuf;
        // As citro3d's: the viewport is the whole target.
        C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
    }
    return true;
}

void C3D_FrameSplit(u8 flags) { (void)flags; }

static GLuint s_presentProg;
static GLint  s_presentRect, s_presentUv, s_presentShade, s_presentUvScale;

// Whether the race's world was drawn this frame (trackview_draw, through
// screen.h). Its picture fills the screen, wider or taller than 4:3. Any
// other frame -- the menus, the loading screens, the movies -- is the PS2's
// 4:3 frame in the middle of the picture, and the room around it shows that
// frame's own edge rows, stretched and dimmed (NFSHP2_FILL unset or "edge"),
// or its edges mirrored at half brightness ("mirror", as the THUG port's
// menus do): no bars, and nothing stretched. What the game left in that room
// (a panel's end that the PS2 hid past the frame's edge, text parked below
// it) is covered. NFSHP2_FILL=0 shows what's drawn there instead, as before.
static bool s_worldFrame;

void r36s_screen_world(void)
{
    s_worldFrame = true;
}

enum { FILL_NONE, FILL_MIRROR, FILL_EDGE };

static int fill_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("NFSHP2_FILL");
        mode = e == NULL ? FILL_EDGE
             : e[0] == '0' ? FILL_NONE
             : strcmp(e, "mirror") == 0 ? FILL_MIRROR : FILL_EDGE;
        printf("R36S: 4:3 frames %s\n",
               mode == FILL_EDGE   ? "fill the screen, their edge rows stretched"
             : mode == FILL_MIRROR ? "fill the screen, their edges mirrored"
                                   : "keep what's drawn around them");
    }
    return mode;
}

// A part of the picture rectangle r (GL's -1..1), from p0 to p1 of its width
// (`wide`) or height, showing the picture from u0 over du of that axis (du
// negative: mirrored; zero: the one line at u0 stretched), at brightness
// `shade`.
static void present_part(const float r[4], bool wide, float p0, float p1, float u0, float du,
                         float shade)
{
    float q[4], uv[4];
    if (wide) {
        q[0] = r[0] + p0 * r[2]; q[1] = r[1]; q[2] = (p1 - p0) * r[2]; q[3] = r[3];
        uv[0] = u0; uv[1] = 0.0f; uv[2] = du; uv[3] = 1.0f;
    } else {
        q[0] = r[0]; q[1] = r[1] + p0 * r[3]; q[2] = r[2]; q[3] = (p1 - p0) * r[3];
        uv[0] = 0.0f; uv[1] = u0; uv[2] = 1.0f; uv[3] = du;
    }
    glUniform4fv(s_presentRect, 1, q);
    glUniform4fv(s_presentUv, 1, uv);
    glUniform1f(s_presentShade, shade);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static C3D_RenderTarget* screen_target(gfxScreen_t screen)
{
    C3D_RenderTarget* t = (unsigned)screen < 2 ? s_linked[screen] : NULL;
    return t != NULL && t->frameBuf.r36s_fbo != 0 ? t : NULL;
}

// ------------------------------------------------------------ the sun's sky
//
// The PS2 sizes the sun's glare by how much sky shows around it: StuffSun
// counts the pixels of a 32x32 rect of the last frame's Z that the clear
// left, which only the sky does (sundraw.h). The 3DS port reads that depth
// back; GLES can't read depth, so here each of 16 blocks of the rect gets an
// occlusion query: a rectangle at the far plane, which the depth test passes
// only where the depth is still the clear's, drawn once the frame's world
// is. The queries are GLES 3's, or GLES 2's GL_EXT_occlusion_query_boolean;
// a GPU with neither (Mali-400's Lima, say) shows the sun as before. The GL
// thread keeps the answers (rgl_ProbeMask), a frame or so late as the PS2's
// transfer is, and the next StuffSun reads them as Z words: 0 for a block
// with sky, a far one for one without. Until the first answers every block
// has sky, as before. NFSHP2_SUNOCC=0 leaves it so; =debug probes the whole
// screen instead and prints its 4x4 sky map; =ext uses the extension's calls
// on an ES 3 context too, to test them.
static GLuint s_probeProg;
static GLint  s_probeRect;
static int    s_sunOcc = -1;      // 0 off, 1 on, 2 debug

void r36s_sun_probe(uint32_t* dst, int x, int y, int w, int h, int targetW, int targetH)
{
    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static unsigned calls;
    C3D_RenderTarget* t = screen_target(GFX_TOP);
    unsigned mask;
    GLint tw, th;
    int bx, by;

    if (s_sunOcc < 0) {
        const char* e = getenv("NFSHP2_SUNOCC");
        // "ext": the ES 2 extension's calls, as a GPU without ES 3 would use
        // (Mesa gives an ES 3 context even to an ES 2 request, so a test run
        // can't get one, and an ES 3 context doesn't list the extension).
        bool forced = e != NULL && strcmp(e, "ext") == 0;
        bool es3 = strstr(s_glVersion, "OpenGL ES 3") != NULL && !forced;
        // ES 3 has the queries; ES 2 may have them as an extension (not
        // every GPU does: Mali-400's Lima, for one).
        bool can = s_gl && (es3 ? rgl_occlusion(true)
                                : (forced || has_ext("GL_EXT_occlusion_query_boolean"))
                                  && rgl_occlusion(false));
        if (!can)
            printf("R36S: no occlusion queries (%s)\n",
                   es3 ? "ES 3's calls not found"
                       : "GL_EXT_occlusion_query_boolean not listed or its calls not found");
        s_sunOcc = !can ? 0 : e == NULL ? 1 : e[0] == '0' ? 0 : strcmp(e, "debug") == 0 ? 2 : 1;
        printf("R36S: the sun's sky counted by %s\n",
               s_sunOcc == 2 ? "occlusion queries over the whole screen (debug)"
               : s_sunOcc    ? "occlusion queries" : "nothing (it always shows)");
    }
    if (!s_sunOcc || w <= 0 || h <= 0 || targetW <= 0 || targetH <= 0) return;
    calls++;

    // What the last frame's queries found, as Z words.
    mask = rgl_ProbeMask();
    if (s_sunOcc == 1 && dst != NULL) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                dst[j * w + i] = (mask >> ((j * 4 / h) * 4 + (i * 4 / w))) & 1 ? 0u : 0xffffffu;
    } else if (s_sunOcc == 2 && calls % 30 == 0) {
        printf("sun probe: the screen's sky map, top row first\n");
        for (by = 0; by < 4; by++) {
            char row[5];
            for (bx = 0; bx < 4; bx++) row[bx] = (mask >> (by * 4 + bx)) & 1 ? 'S' : '#';
            row[4] = '\0';
            printf("sun probe:   %s\n", row);
        }
    }

    // This frame's: the world is drawn into the screen's target by now.
    if (t == NULL || s.target != t || !s.inFrame || t->r36s_depth == 0 || gl_skip()
        || !target_ready(t))
        return;
    if (s_probeProg == 0) {
        s_probeProg = r36sgl_link(kVsProbe, kFsProbe, "sun probe");
        if (s_probeProg == 0) {
            s_sunOcc = 0;
            return;
        }
        s_probeRect = glGetUniformLocation(s_probeProg, "uRect");
    }
    if (s_sunOcc == 2) {
        x = 0;
        y = 0;
        w = targetW;
        h = targetH;
    }
    r36sgl_flush2d();
    r36sgl_flush_held();
    if (!g.valid) cache_reset();
    bind_fbo(t->frameBuf.r36s_fbo);
    target_size(t, &tw, &th);
    set_viewport(0, 0, tw, th);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    r36sgl_use_program(s_probeProg);
    r36sgl_client_arrays();
    r36sgl_attrib_mask(1);
    r36sgl_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    for (by = 0; by < 4; by++) {
        for (bx = 0; bx < 4; bx++) {
            // The block's pixels of the rect, from the target's top, in GL's
            // -1..1 with y up.
            float px0 = (float)x + (float)w * (float)bx * 0.25f;
            float py0 = (float)y + (float)h * (float)by * 0.25f;
            float r[4];

            r[0] = px0 / (float)targetW * 2.0f - 1.0f;
            r[2] = (float)w * 0.25f / (float)targetW * 2.0f;
            r[3] = (float)h * 0.25f / (float)targetH * 2.0f;
            r[1] = 1.0f - (py0 + (float)h * 0.25f) / (float)targetH * 2.0f;
            glUniform4fv(s_probeRect, 1, r);
            rgl_ProbeBegin((unsigned)(by * 4 + bx));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            rgl_ProbeEnd();
        }
    }
    // Everything this changed is behind the cache's back: the next draw
    // starts from GL's state as the cache's reset has it.
    g.valid = false;
}

// The window in GL's terms for a rectangle in a 640x480 frame fitted to
// it at 4:3.
static void present_rect(int ww, int wh, float x, float y, float w, float h, float out[4])
{
    float sw = (float)ww, sh = (float)wh, scale, ox, oy;
    scale = sw * 3.0f > sh * 4.0f ? sh / 480.0f : sw / 640.0f;
    ox = (sw - 640.0f * scale) * 0.5f;
    oy = (sh - 480.0f * scale) * 0.5f;
    // From top-left pixels to GL's -1..1, y up.
    out[0] = (ox + x * scale) / sw * 2.0f - 1.0f;
    out[1] = 1.0f - (oy + (y + h) * scale) / sh * 2.0f;
    out[2] = w * scale / sw * 2.0f;
    out[3] = h * scale / sh * 2.0f;
}

// Headless test runs only (NFSHP2_GL=pbuffer on Mesa's llvmpipe under
// QEMU): llvmpipe there does not show a texture's sampler what was drawn
// into it, only its clear, which a standalone GLES program reproduces and
// softpipe does not do. A GLES 3 blit of the target reads it right. The
// device's presenter samples the target, as below. NFSHP2_PRESENT=sample
// forces that path here too (with GALLIUM_DRIVER=softpipe).
static bool blit_present(C3D_RenderTarget* t, int ww, int wh, const float r[4])
{
    typedef void (*BlitFn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                           GLbitfield, GLenum);
    static int mode = -1;
    static BlitFn blit;
    const char* e;
    int x0, y0, x1, y1;
    if (mode < 0) {
        e = getenv("NFSHP2_PRESENT");
        mode = r36s_video_headless() && !(e && strcmp(e, "sample") == 0);
        if (mode) {
            void* self = dlopen(NULL, RTLD_LAZY);
            blit = self ? (BlitFn)dlsym(self, "glBlitFramebuffer") : NULL;
            if (blit == NULL) mode = 0;
        }
    }
    if (!mode) return false;
    x0 = (int)((r[0] + 1.0f) * 0.5f * ww + 0.5f);
    y0 = (int)((r[1] + 1.0f) * 0.5f * wh + 0.5f);
    x1 = x0 + (int)(r[2] * 0.5f * ww + 0.5f);
    y1 = y0 + (int)(r[3] * 0.5f * wh + 0.5f);
    glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, t->frameBuf.r36s_fbo);
    glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, 0);
    {
        GLint tw, th;
        target_size(t, &tw, &th);
        blit(0, 0, tw, th, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g.fbo = 0;
    return true;
}

static void present_screens(void)
{
    static const float quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    C3D_RenderTarget* top = screen_target(GFX_TOP);
    const char* test = getenv("NFSHP2_C2DTEST");
    C3D_RenderTarget* bot = (s_bottomPopup || (test != NULL && *test != '\0'))
                          ? screen_target(GFX_BOTTOM) : NULL;
    int ww, wh;
    float r[4];
    if (s_presentProg == 0) {
        s_presentProg = r36sgl_link(kVsPresent, kFsPresent, "present");
        if (s_presentProg == 0) return;
        s_presentRect = glGetUniformLocation(s_presentProg, "uRect");
        s_presentUv = glGetUniformLocation(s_presentProg, "uUv");
        s_presentShade = glGetUniformLocation(s_presentProg, "uShade");
        s_presentUvScale = glGetUniformLocation(s_presentProg, "uUvScale");
        r36sgl_use_program(s_presentProg);
        glUniform1i(glGetUniformLocation(s_presentProg, "uTex"), 0);
    }
    if (!g.valid) cache_reset();
    r36s_video_size(&ww, &wh);
    bind_fbo(0);
    set_viewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    if (g.colorMask != 0xF) { glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); g.colorMask = 0xF; }
    glClear(GL_COLOR_BUFFER_BIT);
    if (g.depthTest != 0) { glDisable(GL_DEPTH_TEST); g.depthTest = 0; }
    if (g.cull != 0) { glDisable(GL_CULL_FACE); g.cull = 0; }
    if (g.blend != 0) { glDisable(GL_BLEND); g.blend = 0; }
    r36sgl_use_program(s_presentProg);
    r36sgl_client_arrays();
    r36sgl_attrib_mask(1);
    r36sgl_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    if (top != NULL) {
        // The whole picture, centred; it fills the window unless
        // NFSHP2_ASPECT=0 or a limit on its shape left bars.
        {
            float fw = r36s_screen_full_w(), fh = r36s_screen_full_h();
            float k = (float)ww / fw;
            if ((float)wh / fh < k) k = (float)wh / fh;
            r[2] = fw * k / (float)ww * 2.0f;
            r[3] = fh * k / (float)wh * 2.0f;
            r[0] = -r[2] * 0.5f;
            r[1] = -r[3] * 0.5f;
        }
        bool world = s_worldFrame;
        s_worldFrame = false;
        if (blit_present(top, ww, wh, r)) return;
        bind_gl_texture(0, GL_TEXTURE_2D, top->r36s_color);
        {
            GLint tw, th;
            target_size(top, &tw, &th);
            glUniform2f(s_presentUvScale, (float)tw / (float)top->r36s_w,
                        (float)th / (float)top->r36s_h);
        }
        {
            float fw = r36s_screen_full_w(), fh = r36s_screen_full_h();
            bool wide = fw > 640.5f, tall = fh > 480.5f;
            int fill = (!world && (wide || tall)) ? fill_mode() : FILL_NONE;
            if (fill != FILL_NONE) {
                // The 4:3 frame's share of the picture, e0 to e1, and the
                // room each side of it: the frame's edge row stretched, or
                // the frame mirrored about its own edge, at half brightness,
                // so it reads as background.
                float e0 = 0.5f - 0.5f * (wide ? 640.0f / fw : 480.0f / fh);
                float e1 = 1.0f - e0;
                if (fill == FILL_EDGE) {
                    // A line two pixels inside the frame, not the edge's own
                    // pixel, which linear sampling blends with what lies
                    // outside it.
                    int pw, ph;
                    float in;
                    r36s_screen_pixels(&pw, &ph);
                    in = 2.0f / (float)(wide ? pw : ph);
                    present_part(r, wide, 0.0f, e0, e0 + in, 0.0f, 0.5f);
                    present_part(r, wide, e0, e1, e0, e1 - e0, 1.0f);
                    present_part(r, wide, e1, 1.0f, e1 - in, 0.0f, 0.5f);
                } else {
                    present_part(r, wide, 0.0f, e0, 2.0f * e0, -e0, 0.5f);
                    present_part(r, wide, e0, e1, e0, e1 - e0, 1.0f);
                    present_part(r, wide, e1, 1.0f, e1, -e0, 0.5f);
                }
            } else {
                present_part(r, wide, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
            }
        }
    }
    if (bot != NULL) {
        // The bottom screen's 320x240, centred over the lower half.
        present_rect(ww, wh, 160, 240, 320, 240, r);
        bind_gl_texture(0, GL_TEXTURE_2D, bot->r36s_color);
        glUniform2f(s_presentUvScale, 1.0f, 1.0f);
        present_part(r, true, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
    }
    // The screens' colour textures are drawn into next frame: none stays
    // bound for sampling meanwhile.
    bind_gl_texture(0, GL_TEXTURE_2D, 0);
}

// ------------------------------------------------------- the frame counter
//
// Select + L3 or R3 (ctru_hid.c), as the other R36S ports' Select + R3:
// frames a second and the mean and longest frame of the last half second
// (r36s_perf.c), top left, drawn over the presented frame in a 5x7 font of
// its own, one rectangle per run of lit pixels.

static const struct { char c; u8 rows[7]; } kFont[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'a', { 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F } },
    { 'm', { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11 } },
    { 's', { 0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E } },
    { 'x', { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 } },
};

static const char kVsMeter[] =
    "attribute vec2 v0;\n"
    "attribute vec4 v1;\n"
    "uniform vec2 uScale;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "    vCol = v1;\n"
    "    gl_Position = vec4(v0.x * uScale.x - 1.0, 1.0 - v0.y * uScale.y, 0.0, 1.0);\n"
    "}\n";
static const char kFsMeter[] =
    "precision mediump float;\n"
    "varying vec4 vCol;\n"
    "void main() { gl_FragColor = vCol; }\n";

#define METER_MAX_QUADS 512
typedef struct { float x, y; u8 rgba[4]; } MeterVertex;
static MeterVertex s_meterVerts[METER_MAX_QUADS * 6];
static int s_meterN;
static GLuint s_meterProg;
static GLint s_meterScale;

static void meter_quad(float x0, float y0, float x1, float y1, u32 rgba)
{
    const float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
    if (s_meterN + 6 > METER_MAX_QUADS * 6) return;
    for (int i = 0; i < 6; i++) {
        MeterVertex* v = &s_meterVerts[s_meterN++];
        v->x = xs[i];
        v->y = ys[i];
        v->rgba[0] = (u8)(rgba >> 24); v->rgba[1] = (u8)(rgba >> 16);
        v->rgba[2] = (u8)(rgba >> 8);  v->rgba[3] = (u8)rgba;
    }
}

// Text at (x, y), the top left, `px` pixels to a font pixel; its width.
static float meter_text(float x, float y, const char* text, float px, u32 rgba, bool draw)
{
    float x0 = x;
    for (const char* c = text; *c != '\0'; c++) {
        const u8* rows = NULL;
        for (size_t i = 0; i < sizeof kFont / sizeof kFont[0]; i++)
            if (kFont[i].c == *c) rows = kFont[i].rows;
        if (rows != NULL && draw) {
            for (int r = 0; r < 7; r++) {
                for (int b = 4; b >= 0; ) {
                    int run;
                    if (!((rows[r] >> b) & 1)) { b--; continue; }
                    for (run = 0; b - run >= 0 && ((rows[r] >> (b - run)) & 1); run++) {}
                    meter_quad(x + (4 - b) * px, y + r * px, x + (4 - b + run) * px, y + (r + 1) * px,
                               rgba);
                    b -= run;
                }
            }
        }
        x += 6 * px;
    }
    return x - x0 - px;
}

// The frame counter, top left, if it's on, and a note (r36s_toast), top
// centre, if one is showing, in one draw.
static void draw_meter(int ww, int wh, bool meter, const char* toast)
{
    char l1[32], l2[32];
    float s = wh / 480.0f, big = 3.0f * s, small = 2.0f * s, w;
    if (s_meterProg == 0) {
        s_meterProg = r36sgl_link(kVsMeter, kFsMeter, "frame counter");
        if (s_meterProg == 0) return;
        s_meterScale = glGetUniformLocation(s_meterProg, "uScale");
    }
    s_meterN = 0;
    if (meter) {
        r36s_fps_meter_text(l1, sizeof l1, l2, sizeof l2);
        w = meter_text(0, 0, l1, big, 0, false);
        if (meter_text(0, 0, l2, small, 0, false) > w) w = meter_text(0, 0, l2, small, 0, false);
        meter_quad(6 * s, 6 * s, 6 * s + w + 8 * s, 6 * s + 7 * big + 7 * small + 10 * s, 0x000000A0u);
        meter_text(10 * s, 10 * s, l1, big, 0xFFFF40FFu, true);
        meter_text(10 * s, 10 * s + 7 * big + 3 * s, l2, small, 0xE0E0E0FFu, true);
    }
    if (toast != NULL) {
        float x;
        w = meter_text(0, 0, toast, big, 0, false);
        x = (ww - w) * 0.5f;
        meter_quad(x - 4 * s, 6 * s, x + w + 4 * s, 6 * s + 7 * big + 8 * s, 0x000000C0u);
        meter_text(x, 10 * s, toast, big, 0xFFFFFFFFu, true);
    }

    r36sgl_use_program(s_meterProg);
    glUniform2f(s_meterScale, 2.0f / (float)ww, 2.0f / (float)wh);
    if (g.blend != 1) { glEnable(GL_BLEND); g.blend = 1; }
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    g.blendEq[0] = g.blendEq[1] = GL_FUNC_ADD;
    g.blendFn[0] = GL_SRC_ALPHA; g.blendFn[1] = GL_ONE_MINUS_SRC_ALPHA;
    g.blendFn[2] = GL_ZERO; g.blendFn[3] = GL_ONE;
    r36sgl_client_arrays();
    r36sgl_attrib_mask(3);
    r36sgl_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeterVertex), &s_meterVerts[0].x);
    r36sgl_attrib_pointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(MeterVertex), s_meterVerts[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, s_meterN);
}

static void present(void)
{
    int ww, wh;
    bool meter;
    const char* toast;
    present_screens();
    meter = r36s_fps_meter_on();
    toast = r36s_toast_text();
    if (s_presentProg == 0 || (!meter && toast == NULL)) return;
    r36s_video_size(&ww, &wh);
    bind_fbo(0);
    set_viewport(0, 0, ww, wh);
    draw_meter(ww, wh, meter, toast);
}

// The scripted input's SHOT (ctru_hid.c): the next frame saved.
static volatile bool s_shotNext;
void r36s_shot_next(void) { s_shotNext = true; }

// NFSHP2_SHOTS: "every:N" or "120,600,1800", frames saved to shot_NNNNN.ppm.
static void maybe_shoot(u32 frame)
{
    static int parsed;
    static u32 every, list[32];
    static int nList;
    const char* e;
    bool want = s_shotNext;
    s_shotNext = false;
    if (!parsed) {
        parsed = 1;
        e = getenv("NFSHP2_SHOTS");
        if (e != NULL) {
            if (strncmp(e, "every:", 6) == 0) every = (u32)strtoul(e + 6, NULL, 10);
            else
                while (*e && nList < 32) {
                    list[nList++] = (u32)strtoul(e, (char**)&e, 10);
                    while (*e == ',' || *e == ' ') e++;
                }
        }
    }
    if (every) want = frame % every == 0;
    for (int i = 0; i < nList && !want; i++) want = list[i] == frame;
    if (!want) return;
    {
        int w, h;
        char name[64];
        u8* px;
        FILE* f;
        r36s_video_size(&w, &h);
        px = (u8*)malloc((size_t)w * h * 4);
        if (px == NULL) return;
        // The window, and with NFSHP2_SHOTS_FBO the top target itself too.
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                C3D_RenderTarget* t = screen_target(GFX_TOP);
                if (!getenv("NFSHP2_SHOTS_FBO") || t == NULL) break;
                bind_fbo(t->frameBuf.r36s_fbo);
                w = t->r36s_w;
                h = t->r36s_h;
            }
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            bind_fbo(0);
            snprintf(name, sizeof name, pass ? "fbo_%05u.ppm" : "shot_%05u.ppm", (unsigned)frame);
            f = fopen(name, "wb");
            if (f != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; y--)
                    for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
                fclose(f);
                printf("shot: %s\n", name);
            }
        }
        free(px);
    }
}

void C3D_FrameEnd(u8 flags)
{
    u64 end;
    (void)flags;
    if (!s.inFrame) return;
    r36sgl_flush2d();
    s.inFrame = false;
    end = svcGetSystemTick();
    s.procMs = (float)((double)(end - s.frameStartTick) / CPU_TICKS_PER_MSEC);
    // The vblank wait is the swap's (SDL_GL_SetSwapInterval(1)); without a
    // display, pace at 60 Hz as the 3DS's GSP would.
    if (s_gl && !gl_skip()) {
        r36sgl_flush2d();
        r36sgl_flush_held();
        pix_bury();
        ds_report();
        r36sgl_buffers_frame();
        present();
        maybe_shoot(s.frames);
        rgl_frame_is(s.frames);
        if (s_dbg.every == 0) {
            const char* e = getenv("NFSHP2_GLDEBUG");
            s_dbg.every = e ? atoi(e) : -1;
            if (s_dbg.every == 0) s_dbg.every = -1;
        }
        if (s_dbg.every > 0 && s.frames % (u32)s_dbg.every == 0) {
            GLenum err = glGetError();
            printf("gl: frame %u: 3D %u asked, %u drawn (no target %u, program %u, twin %u, link %u);"
                   " 2D %u quads in %u flushes; error 0x%x\n", (unsigned)s.frames, s_dbg.asked,
                   s_dbg.drawn, s_dbg.noTarget, s_dbg.noProgram, s_dbg.noTwin, s_dbg.noLink,
                   s_dbg.quads2d, s_dbg.flush2d, err);
            printf("gl: not held: shader/buffers %u, order %u, mesh upload %u, texture %u,"
                   " target %u, no model uniforms %u, no twin %u, not a mesh %u, layout %u,"
                   " uniforms %u; order: no depth test %u, no depth write %u, blend %u\n",
                   s_holdWhy[1], s_holdWhy[2], s_holdWhy[3], s_holdWhy[4],
                   s_holdWhy[5], s_holdWhy[6], s_holdWhy[7], s_holdWhy[8], s_holdWhy[9],
                   s_holdWhy[10], s_holdWhy[0] & 0x3FF, (s_holdWhy[0] >> 10) & 0x3FF,
                   s_holdWhy[0] >> 20);
            memset(s_holdWhy, 0, sizeof s_holdWhy);
            { u32 up = s_dbg.uploads; memset(&s_dbg.asked, 0, sizeof s_dbg - sizeof s_dbg.every); s_dbg.uploads = up; }
        }
        {
            // The swap and the frame's log: on the GL thread when it runs
            // (glthread.c), here otherwise.
            R36sGlCounts c = r36s_glcounts;
            memset(&r36s_glcounts, 0, sizeof r36s_glcounts);
            rgl_frame_end(&c);
        }
    } else {
        static u64 next;
        u64 now = svcGetSystemTick(), period = SYSCLOCK_ARM11 / 60;
        if (next == 0 || now > next + period) next = now;
        next += period;
        if (next > now)
            svcSleepThread((s64)((next - now) * 1000000000ull / SYSCLOCK_ARM11));
    }
    s.drawMs = (float)((double)(svcGetSystemTick() - s.frameStartTick) / CPU_TICKS_PER_MSEC);
    for (C3D_RenderTarget* t = s.targets; t != NULL; t = t->next)
        t->used = false;
    s.frames++;
}

void C3D_FrameEndHook(void (*hook)(void*), void* param) { (void)hook; (void)param; }
void C3D_FrameSync(void) {}

void r36s_wait_track_rendering(void)
{
    const char* enabled = getenv("NFSHP2_TRACK_SYNC");
    if (enabled && strcmp(enabled, "0") == 0) return;
    rgl_direct_begin();
    glFinish();
    rgl_direct_end();
}
u32 C3D_FrameCounter(int id) { (void)id; return s.frames; }
float C3D_GetDrawingTime(void) { return s.drawMs; }
float C3D_GetProcessingTime(void) { return s.procMs; }

// ---------------------------------------------------------- shaders

// The R36S's "shbin" (r36s/scripts/gen_embedded.py): "R36SPICA", the DVLE
// count, then each DVLE's name and uniforms.
DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize)
{
    const u8* p = (const u8*)shbinData;
    const u8* end = p + shbinSize;
    DVLB_s* d;
    u32 n;

    if (shbinSize < 12 || memcmp(p, "R36SPICA", 8) != 0) return NULL;
    memcpy(&n, p + 8, 4);
    p += 12;
    d = (DVLB_s*)calloc(1, sizeof *d);
    if (d == NULL) return NULL;
    d->numDVLE = n;
    d->DVLE = (DVLE_s*)calloc(n ? n : 1, sizeof(DVLE_s));
    for (u32 i = 0; i < n && p < end; i++) {
        DVLE_s* e = &d->DVLE[i];
        u8 len = *p++;
        u32 nu;
        memcpy(e->name, p, len < sizeof e->name - 1 ? len : sizeof e->name - 1);
        p += len;
        memcpy(&nu, p, 4);
        p += 4;
        e->numUniforms = (int)nu;
        e->uniforms = (R36SPicaUniform*)calloc(nu ? nu : 1, sizeof(R36SPicaUniform));
        for (u32 k = 0; k < nu && p < end; k++) {
            R36SPicaUniform* u = &e->uniforms[k];
            u8 ul = *p++;
            memcpy(u->name, p, ul < sizeof u->name - 1 ? ul : sizeof u->name - 1);
            p += ul;
            u->kind = (char)p[0];
            u->reg = p[1];
            u->count = p[2];
            p += 3;
        }
    }
    return d;
}

void DVLB_Free(DVLB_s* dvlb)
{
    if (dvlb == NULL) return;
    s_vsLast = NULL;
    for (u32 i = 0; i < dvlb->numDVLE; i++)
        free(dvlb->DVLE[i].uniforms);
    free(dvlb->DVLE);
    free(dvlb);
}

Result shaderProgramInit(shaderProgram_s* sp)
{
    memset(sp, 0, sizeof *sp);
    return 0;
}

Result shaderProgramFree(shaderProgram_s* sp)
{
    if (sp == NULL) return 0;
    free(sp->vertexShader);
    free(sp->geometryShader);
    sp->vertexShader = sp->geometryShader = NULL;
    return 0;
}

Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle)
{
    if (sp == NULL || dvle == NULL) return R36S_RESULT_UNSUPPORTED;
    free(sp->vertexShader);
    sp->vertexShader = (shaderInstance_s*)calloc(1, sizeof(shaderInstance_s));
    if (sp->vertexShader == NULL) return R36S_RESULT_UNSUPPORTED;
    sp->vertexShader->dvle = dvle;
    return 0;
}

Result shaderProgramSetGsh(shaderProgram_s* sp, DVLE_s* dvle, u8 stride)
{
    (void)sp; (void)dvle; (void)stride;
    return R36S_RESULT_UNSUPPORTED;
}

// As citro3d's: the register less 0x10, so c0 is 0, i0 0x60, b0 0x68;
// -1 for a name the shader does not declare.
s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name)
{
    if (si == NULL || si->dvle == NULL || name == NULL) return -1;
    for (int i = 0; i < si->dvle->numUniforms; i++) {
        const R36SPicaUniform* u = &si->dvle->uniforms[i];
        if (strcmp(u->name, name) == 0)
            return (s8)(u->kind == 'f' ? u->reg : u->kind == 'i' ? 0x60 + u->reg : 0x68 + u->reg);
    }
    return -1;
}

// ------------------------------------------------- gfx / gsp (frames)

void gfxInitDefault(void) { r36s_video_init(); }
void gfxExit(void) {}
void gfxFlushBuffers(void) {}
void gfxSwapBuffers(void) {}
void gfxSet3D(bool enable) { (void)enable; }
u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height)
{
    static u8 dummy[400 * 240 * 4];
    (void)side;
    if (width) *width = 240;
    if (height) *height = screen == GFX_TOP ? 400 : 320;
    return dummy;
}

void gspWaitForVBlank(void)
{
    svcSleepThread(16666667);
}
