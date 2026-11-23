// The subset of libctru the port uses, declared for the R36S build and
// implemented on SDL2 and pthreads in r36s/compat/src/ctru.c.
//
// The game's 3DS paths are compiled as they are (__3DS__ stays defined), so
// what they ask of the console's OS is answered here. Names, types and
// semantics follow libctru; where the R36S has nothing to offer (the
// wireless, the HOME Menu, the system's memory regions) a call does nothing
// or fails the way a console without that service would.

#ifndef R36S_COMPAT_3DS_H
#define R36S_COMPAT_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------ types

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef volatile u32 vu32;
typedef volatile s32 vs32;
typedef u32 Handle;
typedef s32 Result;

#define R_FAILED(res)    ((res) < 0)
#define R_SUCCEEDED(res) ((res) >= 0)
#define R_LEVEL(res)       (((res) >> 27) & 0x1F)
#define R_SUMMARY(res)     (((res) >> 21) & 0x3F)
#define R_MODULE(res)      (((res) >> 10) & 0xFF)
#define R_DESCRIPTION(res) ((res) & 0x3FF)

// A failure this layer reports for a service the R36S doesn't have.
#define R36S_RESULT_UNSUPPORTED ((Result)0xE0C17FF8)

#define U64_MAX UINT64_MAX
#define BIT(n) (1U << (n))
#define ALIGN(m) __attribute__((aligned(m)))
#define PACKED __attribute__((packed))
#ifndef DEPRECATED
#define DEPRECATED __attribute__((deprecated))
#endif

// ------------------------------------------------------------------- svc

typedef enum {
    MEMOP_FREE = 1, MEMOP_RESERVE = 2, MEMOP_ALLOC = 3, MEMOP_MAP = 4,
    MEMOP_UNMAP = 5, MEMOP_PROT = 6,
    MEMOP_REGION_APP = 0x100, MEMOP_REGION_SYSTEM = 0x200,
    MEMOP_REGION_BASE = 0x300, MEMOP_OP_MASK = 0xFF,
    MEMOP_REGION_MASK = 0xF00, MEMOP_LINEAR_FLAG = 0x10000,
    MEMOP_ALLOC_LINEAR = MEMOP_LINEAR_FLAG | MEMOP_ALLOC,
} MemOp;

typedef enum {
    MEMPERM_READ = 1, MEMPERM_WRITE = 2, MEMPERM_EXECUTE = 4,
    MEMPERM_READWRITE = MEMPERM_READ | MEMPERM_WRITE,
    MEMPERM_READEXECUTE = MEMPERM_READ | MEMPERM_EXECUTE,
    MEMPERM_DONTCARE = 0x10000000,
} MemPerm;

typedef enum {
    MEMSTATE_FREE = 0, MEMSTATE_RESERVED = 1, MEMSTATE_IO = 2,
    MEMSTATE_STATIC = 3, MEMSTATE_CODE = 4, MEMSTATE_PRIVATE = 5,
    MEMSTATE_SHARED = 6, MEMSTATE_CONTINUOUS = 7, MEMSTATE_ALIASED = 8,
    MEMSTATE_ALIAS = 9, MEMSTATE_ALIASCODE = 10, MEMSTATE_LOCKED = 11,
} MemState;

typedef struct { u32 base_addr; u32 size; u32 perm; u32 state; } MemInfo;
typedef struct { u32 flags; } PageInfo;

typedef enum {
    RESLIMIT_PRIORITY = 0, RESLIMIT_COMMIT = 1, RESLIMIT_THREAD = 2,
    RESLIMIT_EVENT = 3, RESLIMIT_MUTEX = 4, RESLIMIT_SEMAPHORE = 5,
    RESLIMIT_TIMER = 6, RESLIMIT_SHAREDMEMORY = 7,
    RESLIMIT_ADDRESSARBITER = 8, RESLIMIT_CPUTIME = 9,
} ResourceLimitType;

#define CUR_THREAD_HANDLE  0xFFFF8000
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define OS_VRAM_VADDR 0x1F000000
#define OS_VRAM_SIZE  0x00600000
#define USERBREAK_PANIC 0
#define USERBREAK_ASSERT 1
typedef enum { USERBREAK_USER = 2 } UserBreakTypeExtra;

Result svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size,
                        MemOp op, MemPerm perm);
Result svcQueryMemory(MemInfo* info, PageInfo* out, u32 addr);
Result svcGetSystemInfo(s64* out, u32 type, s32 param);
Result svcGetResourceLimit(Handle* resourceLimit, Handle process);
Result svcGetResourceLimitLimitValues(s64* values, Handle resourceLimit,
                                      ResourceLimitType* names, s32 nameCount);
Result svcGetResourceLimitCurrentValues(s64* values, Handle resourceLimit,
                                        ResourceLimitType* names, s32 nameCount);
Result svcCloseHandle(Handle handle);
Result svcGetThreadPriority(s32* out, Handle handle);
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size);
void   svcSleepThread(s64 ns);
void   svcBreak(u32 breakReason) __attribute__((noreturn));
u64    svcGetSystemTick(void);

// ------------------------------------------------------------------- os

// The 3DS's ARM11 tick: svcGetSystemTick counts at this rate here too, so
// the port's tick arithmetic holds.
#define SYSCLOCK_ARM11 268111856ULL
#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)
#define CPU_TICKS_PER_USEC (SYSCLOCK_ARM11 / 1000000.0)

u64  osGetTime(void);                 // ms since 1900-01-01, as libctru
u32  osConvertVirtToPhys(const void* vaddr);
void osSetSpeedupEnable(bool enable);
u32  osGetApplicationMemType(void);
bool envIsHomebrew(void);

typedef enum {
    APPMEMTYPE_OLD3DS_64MB = 0, APPMEMTYPE_OLD3DS_72MB = 2,
    APPMEMTYPE_OLD3DS_80MB = 3, APPMEMTYPE_OLD3DS_32MB = 4,
    APPMEMTYPE_OLD3DS_64MB_ALT = 5, APPMEMTYPE_NEW3DS_124MB = 6,
    APPMEMTYPE_NEW3DS_178MB = 7,
} AppMemType;

// --------------------------------------------------- threads and locking

typedef struct R36SThread* Thread;
typedef void (*ThreadFunc)(void*);

Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size,
                    int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void   threadFree(Thread thread);
Thread threadGetCurrent(void);
Handle threadGetHandle(Thread thread);
void   threadExit(int rc) __attribute__((noreturn));

typedef struct { pthread_mutex_t m; int ready; } LightLock;
void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
int  LightLock_TryLock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             state;
    ResetType       type;
} LightEvent;
void LightEvent_Init(LightEvent* event, ResetType reset_type);
void LightEvent_Clear(LightEvent* event);
void LightEvent_Pulse(LightEvent* event);
void LightEvent_Signal(LightEvent* event);
int  LightEvent_TryWait(LightEvent* event);
void LightEvent_Wait(LightEvent* event);
int  LightEvent_WaitTimeout(LightEvent* event, s64 timeout_ns);

// ---------------------------------------------------- linear and VRAM

// One heap here: there is no separate GPU memory to manage. The game's
// accounting of how much is free keeps working, against the R36S's budget.
void* linearAlloc(size_t size);
void* linearMemAlign(size_t size, size_t alignment);
void* linearRealloc(void* mem, size_t size);
size_t linearGetSize(void* mem);
void  linearFree(void* mem);
u32   linearSpaceFree(void);

void* vramAlloc(size_t size);
void* vramAllocAt(size_t size, int pos);
void* vramMemAlign(size_t size, size_t alignment);
void  vramFree(void* mem);
u32   vramSpaceFree(void);

// -------------------------------------------------------------- APT

typedef enum { APTPOS_NONE = -1 } APT_AppletPos;
bool   aptMainLoop(void);
Result APT_CheckNew3DS(bool* out);
Result APT_SetAppCpuTimeLimit(u32 percent);
Result APT_GetAppCpuTimeLimit(u32* percent);
void   aptSetHomeAllowed(bool allowed);
void   aptSetSleepAllowed(bool allowed);

// -------------------------------------------------------- gfx and GSP

typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum {
    GSP_RGBA8_OES = 0, GSP_BGR8_OES = 1, GSP_RGB565_OES = 2,
    GSP_RGB5_A1_OES = 3, GSP_RGBA4_OES = 4,
} GSPGPU_FramebufferFormat;

void gfxInitDefault(void);
void gfxExit(void);
void gfxFlushBuffers(void);
void gfxSwapBuffers(void);
void gfxSet3D(bool enable);
u8*  gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width,
                       u16* height);
void gspWaitForVBlank(void);
#define gspWaitForVBlank0 gspWaitForVBlank
Result GSPGPU_FlushDataCache(const void* adr, u32 size);
Result GSPGPU_InvalidateDataCache(const void* adr, u32 size);
Result GSPGPU_ReadHWRegs(u32 regAddr, u32* data, u8 size);

typedef struct PrintConsole PrintConsole;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console);

// The GX command queue: the R36S's GPU layer has no such queue, so these
// run nothing and report done.
typedef struct { u32 data[8]; } gxCmdEntry_s;
typedef struct gxCmdQueue_s {
    gxCmdEntry_s* entries;
    u16 maxEntries, numEntries, curEntry, lastEntry;
    void (*callback)(struct gxCmdQueue_s*);
    void* user;
} gxCmdQueue_s;
void gxCmdQueueClear(gxCmdQueue_s* queue);
void gxCmdQueueRun(gxCmdQueue_s* queue);
void gxCmdQueueStop(gxCmdQueue_s* queue);
bool gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);

// libctru's GPU command buffer. The R36S's GL layer has none of its own;
// these exist so the 3DS code's bookkeeping of it compiles and reads 0.
extern u32* gpuCmdBuf;
extern u32  gpuCmdBufSize;
extern u32  gpuCmdBufOffset;

#define GX_BUFFER_DIM(w, h) (((h) << 16) | ((w) & 0xFFFF))
typedef enum {
    GX_TRANSFER_FMT_RGBA8 = 0, GX_TRANSFER_FMT_RGB8 = 1,
    GX_TRANSFER_FMT_RGB565 = 2, GX_TRANSFER_FMT_RGB5A1 = 3,
    GX_TRANSFER_FMT_RGBA4 = 4,
} GX_TRANSFER_FORMAT;
typedef enum {
    GX_TRANSFER_SCALE_NO = 0, GX_TRANSFER_SCALE_X = 1,
    GX_TRANSFER_SCALE_XY = 2,
} GX_TRANSFER_SCALE;
typedef enum {
    GX_FILL_TRIGGER = 0x001, GX_FILL_FINISHED = 0x002,
    GX_FILL_16BIT_DEPTH = 0x000, GX_FILL_24BIT_DEPTH = 0x100,
    GX_FILL_32BIT_DEPTH = 0x200,
} GX_FILL_CONTROL;
#define GX_TRANSFER_FLIP_VERT(x)  ((x) << 0)
#define GX_TRANSFER_OUT_TILED(x)  ((x) << 1)
#define GX_TRANSFER_RAW_COPY(x)   ((x) << 3)
#define GX_TRANSFER_IN_FORMAT(x)  ((x) << 8)
#define GX_TRANSFER_OUT_FORMAT(x) ((x) << 12)
#define GX_TRANSFER_SCALING(x)    ((x) << 24)
Result GX_ProcessCommandList(u32* buf0a, u32 buf0s, u8 flags);
Result GX_MemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0,
                     u32* buf1a, u32 buf1v, u32* buf1e, u16 control1);
Result GX_DisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim,
                          u32 flags);
Result GX_TextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim,
                      u32 size, u32 flags);

// ---------------------------------------------------------------- HID

enum {
    KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
    KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6),
    KEY_DDOWN = BIT(7), KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10),
    KEY_Y = BIT(11), KEY_L3 = BIT(12), KEY_R3 = BIT(13),
    KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
    KEY_CSTICK_RIGHT = BIT(24), KEY_CSTICK_LEFT = BIT(25),
    KEY_CSTICK_UP = BIT(26), KEY_CSTICK_DOWN = BIT(27),
    KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29),
    KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
    KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN,
    KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT,
    KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT,
};

typedef struct { s16 dx, dy; } circlePosition;
typedef struct { u16 px, py; } touchPosition;

void hidScanInput(void);
u32  hidKeysHeld(void);
u32  hidKeysDown(void);
u32  hidKeysUp(void);
void hidCircleRead(circlePosition* pos);
void hidCstickRead(circlePosition* pos);
void hidTouchRead(touchPosition* pos);

// ------------------------------------------------------------- NDSP

typedef enum {
    NDSP_OUTPUT_MONO = 0, NDSP_OUTPUT_STEREO = 1, NDSP_OUTPUT_SURROUND = 2,
} ndspOutputMode;
typedef enum {
    NDSP_INTERP_POLYPHASE = 0, NDSP_INTERP_LINEAR = 1, NDSP_INTERP_NONE = 2,
} ndspInterpType;
enum {
    NDSP_ENCODING_PCM8 = 0, NDSP_ENCODING_PCM16 = 1, NDSP_ENCODING_ADPCM = 2,
};
#define NDSP_CHANNELS(n)  ((u32)(n) & 3)
#define NDSP_ENCODING(n) (((u32)(n) & 3) << 2)
enum {
    NDSP_FORMAT_MONO_PCM8    = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM8),
    NDSP_FORMAT_MONO_PCM16   = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM16),
    NDSP_FORMAT_MONO_ADPCM   = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_ADPCM),
    NDSP_FORMAT_STEREO_PCM8  = NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM8),
    NDSP_FORMAT_STEREO_PCM16 = NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM16),
    NDSP_FORMAT_PCM8  = NDSP_FORMAT_MONO_PCM8,
    NDSP_FORMAT_PCM16 = NDSP_FORMAT_MONO_PCM16,
    NDSP_FORMAT_ADPCM = NDSP_FORMAT_MONO_ADPCM,
    NDSP_FRONT_BYPASS = BIT(4), NDSP_3D_SURROUND_PREPROCESSED = BIT(6),
};
enum {
    NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED = 1, NDSP_WBUF_PLAYING = 2,
    NDSP_WBUF_DONE = 3,
};

typedef struct { u16 index; s16 history0; s16 history1; } ndspAdpcmData;

typedef struct tag_ndspWaveBuf ndspWaveBuf;
struct tag_ndspWaveBuf {
    union {
        s8*         data_pcm8;
        s16*        data_pcm16;
        u8*         data_adpcm;
        const void* data_vaddr;
    };
    u32            nsamples;
    ndspAdpcmData* adpcm_data;
    u32            offset;
    bool           looping;
    volatile u8    status;
    u16            sequence_id;
    ndspWaveBuf*   next;
};

Result ndspInit(void);
void   ndspExit(void);
void   ndspSetOutputMode(ndspOutputMode mode);
void   ndspSetMasterVol(float volume);
void   ndspChnReset(int id);
void   ndspChnSetFormat(int id, u16 format);
void   ndspChnSetInterp(int id, ndspInterpType type);
void   ndspChnSetRate(int id, float rate);
void   ndspChnSetMix(int id, float mix[12]);
void   ndspChnSetAdpcmCoefs(int id, u16 coefs[16]);
void   ndspChnSetPaused(int id, bool paused);
bool   ndspChnIsPaused(int id);
bool   ndspChnIsPlaying(int id);
u32    ndspChnGetSamplePos(int id);
u16    ndspChnGetWaveBufSeq(int id);
void   ndspChnWaveBufClear(int id);
void   ndspChnWaveBufAdd(int id, ndspWaveBuf* buf);
Result DSP_FlushDataCache(const void* address, u32 size);

// ------------------------------------------------------- FS and CFG

typedef struct {
    u32 sectorSize, clusterSize, totalClusters, freeClusters;
} FS_ArchiveResource;
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource* archiveResource);

typedef enum {
    CFG_MODEL_3DS = 0, CFG_MODEL_3DSXL = 1, CFG_MODEL_N3DS = 2,
    CFG_MODEL_2DS = 3, CFG_MODEL_N3DSXL = 4, CFG_MODEL_N2DSXL = 5,
} CFG_SystemModel;
Result cfguInit(void);
void   cfguExit(void);
Result CFGU_GetSystemModel(u8* model);

Result ptmSysmInit(void);
void   ptmSysmExit(void);

// ------------------------------------------------------------ swkbd

// The R36S has no software keyboard applet: swkbdInputText answers
// SWKBD_BUTTON_NONE, as when the user cancels, and the game keeps the name
// it had.
typedef enum {
    SWKBD_TYPE_NORMAL = 0, SWKBD_TYPE_QWERTY, SWKBD_TYPE_NUMPAD,
    SWKBD_TYPE_WESTERN,
} SwkbdType;
typedef enum {
    SWKBD_ANYTHING = 0, SWKBD_NOTEMPTY, SWKBD_NOTEMPTY_NOTBLANK,
    SWKBD_NOTBLANK_NOTEMPTY = SWKBD_NOTEMPTY_NOTBLANK, SWKBD_NOTBLANK,
    SWKBD_FIXEDLEN,
} SwkbdValidInput;
typedef enum {
    SWKBD_BUTTON_LEFT = 0, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT,
    SWKBD_BUTTON_CONFIRM = SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_NONE,
} SwkbdButton;
typedef enum {
    SWKBD_CALLBACK_OK = 0, SWKBD_CALLBACK_CLOSE, SWKBD_CALLBACK_CONTINUE,
} SwkbdCallbackResult;
typedef enum {
    SWKBD_NONE = 0, SWKBD_INVALID_INPUT = -1, SWKBD_OUTOFMEM = -2,
    SWKBD_HOMEPRESSED = -3, SWKBD_RESETPRESSED = -4, SWKBD_POWERPRESSED = -5,
    SWKBD_D0_CLICK = 0, SWKBD_D1_CLICK0, SWKBD_D1_CLICK1, SWKBD_D2_CLICK0,
    SWKBD_D2_CLICK1, SWKBD_D2_CLICK2,
} SwkbdResult;
enum {
    SWKBD_PARENTAL = BIT(0), SWKBD_DARKEN_TOP_SCREEN = BIT(1),
    SWKBD_PREDICTIVE_INPUT = BIT(2), SWKBD_MULTILINE = BIT(3),
    SWKBD_FIXED_WIDTH = BIT(4), SWKBD_ALLOW_HOME = BIT(5),
    SWKBD_ALLOW_RESET = BIT(6), SWKBD_ALLOW_POWER = BIT(7),
    SWKBD_DEFAULT_QWERTY = BIT(9),
};
enum {
    SWKBD_FILTER_DIGITS = BIT(0), SWKBD_FILTER_AT = BIT(1),
    SWKBD_FILTER_PERCENT = BIT(2), SWKBD_FILTER_BACKSLASH = BIT(3),
    SWKBD_FILTER_PROFANITY = BIT(4), SWKBD_FILTER_CALLBACK = BIT(5),
};
typedef SwkbdCallbackResult (*SwkbdCallbackFn)(void* user, const char** ppMessage,
                                               const char* text, size_t textlen);
typedef struct {
    int             type;
    int             numButtons;
    int             validInput;
    u32             features;
    u32             filterFlags;
    int             maxDigits;
    int             maxTextLength;
    SwkbdCallbackFn filterCallback;
    void*           filterUser;
    char            hint[256];
    char            initial[256];
    SwkbdResult     result;
} SwkbdState;
void swkbdInit(SwkbdState* swkbd, SwkbdType type, int numButtons,
               int maxTextLength);
void swkbdSetFeatures(SwkbdState* swkbd, u32 features);
void swkbdSetHintText(SwkbdState* swkbd, const char* text);
void swkbdSetInitialText(SwkbdState* swkbd, const char* text);
void swkbdSetValidation(SwkbdState* swkbd, SwkbdValidInput validInput,
                        u32 filterFlags, int maxDigits);
void swkbdSetFilterCallback(SwkbdState* swkbd, SwkbdCallbackFn callback,
                            void* user);
SwkbdButton swkbdInputText(SwkbdState* swkbd, char* buf, size_t bufsize);
SwkbdResult swkbdGetResult(SwkbdState* swkbd);

// -------------------------------------------------- the R36S's own

// The directory that stands for the 3DS's SD card root: "sdmc:/X" opens
// <root>/X. Set from NFSHP2_ROOT, else the working directory.
const char* r36s_sdmc_root(void);

// Drain queued GLES work before the race frees its render resources.
void r36s_wait_track_rendering(void);
// "sdmc:/..." to a host path, in `out`; any other path is copied as it is.
const char* r36s_path(const char* in, char* out, size_t n);

// The R36S has one screen, laid out as the PS2's (screen.h). What the 3DS
// draws on its bottom screen is shown, as a panel over the main screen,
// only in the frames that ask for it: the port's own screens (the
// controller setup, the replay's keys, the debug menus).
void r36s_bottom_popup(bool on);

#ifdef __cplusplus
}
#endif

#endif
