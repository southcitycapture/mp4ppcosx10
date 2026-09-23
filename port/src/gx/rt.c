/* The render thread (M27, PLAN.md 42).
 *
 * §32.1's arithmetic: presented fps = 60/(k+1), k consumed frames paying for
 * one drawn frame inside (k+1)·16.7 ms.  The board's drawn frame was ~39 ms
 * -- 21 of them the game thread's (game 11, decode 7, state/tex 3.5) and 13
 * the GL driver's (the issue: gldUpdateDispatch, the copying path, the
 * command-buffer traffic, §36.3) -- so k = 2 and the cap was 20.  Overlapped
 * on the two cores the frame costs max(21, 13) plus what the overlap costs,
 * and the 30 cap needs drawn + consumed under 33 ms.
 *
 * The seam is the GL call.  Everything the port decides -- the batching, the
 * shadow's elision, the TEV mapping, the vertex program's variant and
 * parameters, the texture cache's hash/decode/bind -- stays on the game
 * thread; what moves is the *emission*.  gx_rt.h gives every GL entry point
 * the port calls a twin that appends {op, args} to the stream below, and
 * this thread replays the stream in order with the real GL.  The same calls
 * in the same order under the same arguments: the md5s hold by construction,
 * and every stage of the build was witnessed by them (PLAN.md 42.2).
 *
 * The rules (§39, restated):
 *   - no GL call from any thread but this one once it is up; the twins are
 *     the one door and rt.c the one file that calls the real functions;
 *   - the replay reads nothing of the game's: the records carry their
 *     arguments by value (matrices, parameters, multi-draw arrays), the
 *     vertex arrays point into the ring (fixed, fenced: rt_ring_*), a
 *     texture upload owns its malloc'd texels and frees them here, an
 *     RT_CALL target takes its inputs in the record;
 *   - the inline twin: --renderthread 1 records the same stream and runs the
 *     same replay switch on the game thread, at every publish;
 *   - anything that returns data is a join (counted by name, timed).
 *
 * The stream is one ring of bytes.  Positions are monotonic u32 (differences,
 * never comparisons); a record is {op, len} + args, 8-byte aligned, and never
 * straddles the end (an OP_WRAP pads to it).  The writer publishes `wr_pub`
 * after every record (a store and a barrier; the reader spins briefly before
 * sleeping, so the common case wakes nobody); the reader publishes `rd` after
 * every record.  Waiting is rare and named: the gate, the ring's reuse, a
 * full stream, a read-back, a compile, a snapshot.
 */
#include "port.h"

#include "gx_internal.h"

#include <pthread.h>
#include <stddef.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#ifndef PORT_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#endif

int rt_recording;

#ifndef PORT_NO_SDL

/* The barriers.  `sync` (what __sync_synchronize emits) orders everything,
 * including a store before a later load, and costs the 7450 a pipeline
 * drain; `lwsync` orders load-load, load-store and store-store, which is all
 * an acquire (a position, then the records behind it) or a release (the
 * records, then the position) needs.  The two Dekker handshakes -- the
 * writer's publish against the reader's `asleep`, the reader's `rd` against
 * the writer's `waiting` -- are store-then-load and keep the full sync. */
#if defined(__ppc__) || defined(__powerpc__)
#define RT_ACQ_REL() __asm__ volatile("lwsync" ::: "memory")
#else
#define RT_ACQ_REL() __sync_synchronize()
#endif
#define RT_FULL() __sync_synchronize()

/* ---- the records ---------------------------------------------------------- */

enum {
    OP_NOP = 0,
    OP_WRAP,
    OP_ENABLE, OP_DISABLE, OP_ENABLE_CS, OP_DISABLE_CS,
    OP_ACTIVE_TEX, OP_CLIENT_ACTIVE_TEX, OP_BIND_TEX,
    OP_TEXPARAM_I, OP_TEXPARAM_F, OP_TEXENV_I, OP_TEXENV_F, OP_TEXENV_FV,
    OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_PUSH, OP_POP, OP_ORTHO,
    OP_DEPTH_MASK, OP_DEPTH_FUNC, OP_DEPTH_RANGE, OP_COLOR_MASK, OP_CULL, OP_FRONT,
    OP_SHADE, OP_POLYMODE, OP_HINT, OP_BLEND_FUNC, OP_BLEND_EQ, OP_ALPHA_FUNC,
    OP_FOG_I, OP_FOG_F, OP_FOG_FV, OP_LINE_WIDTH, /* M30: GXSetLineWidth (PLAN.md 45) */ OP_LIGHT_F, OP_LIGHT_FV, OP_LIGHTMODEL_I, OP_LIGHTMODEL_FV,
    OP_MATERIAL_FV, OP_COLOR_MATERIAL,
    OP_CLEAR_COLOR, OP_CLEAR_DEPTH, OP_CLEAR, OP_VIEWPORT, OP_SCISSOR, OP_PIXELSTORE,
    OP_READ_BUFFER, OP_FINISH,
    OP_BEGIN, OP_END, OP_TEXCOORD2F, OP_VERTEX2F,
    OP_VERTEX_PTR, OP_COLOR_PTR, OP_NORMAL_PTR, OP_TEXCOORD_PTR, OP_FOGCOORD_PTR,
    OP_DRAW_ARRAYS, OP_MULTI_DRAW, OP_DRAW_RANGE,
    OP_DELETE_TEX, OP_TEXIMAGE, OP_COPY_TEX_SUB, OP_TEXSUBIMAGE,
    OP_READ_PIXELS, OP_GET_TEX_IMAGE, OP_GET_ERROR,
    OP_FLUSH_VAR, OP_SET_FENCE, OP_WAIT_FENCE,
    OP_BIND_PROG, OP_ENV4, OP_ENVN,
    OP_CALL, OP_PRESENT,
    OP_DECODE, /* M29: a display-list run decoded into the ring (PLAN.md 44) */
    OP_BIND_BUFFER, OP_BUFFER_SUB, /* M40: the vertex cache's buffer object (PLAN.md 55) */
    OP_N
};

static const char* const op_name[OP_N] = {
    "nop", "wrap", "Enable", "Disable", "EnableClientState", "DisableClientState",
    "ActiveTexture", "ClientActiveTexture", "BindTexture",
    "TexParameteri", "TexParameterf", "TexEnvi", "TexEnvf", "TexEnvfv",
    "MatrixMode", "LoadIdentity", "LoadMatrixf", "PushMatrix", "PopMatrix", "Ortho",
    "DepthMask", "DepthFunc", "DepthRange", "ColorMask", "CullFace", "FrontFace",
    "ShadeModel", "PolygonMode", "Hint", "BlendFunc", "BlendEquation", "AlphaFunc",
    "Fogi", "Fogf", "Fogfv", "LineWidth", "Lightf", "Lightfv", "LightModeli", "LightModelfv",
    "Materialfv", "ColorMaterial",
    "ClearColor", "ClearDepth", "Clear", "Viewport", "Scissor", "PixelStorei",
    "ReadBuffer", "Finish",
    "Begin", "End", "TexCoord2f", "Vertex2f",
    "VertexPointer", "ColorPointer", "NormalPointer", "TexCoordPointer", "FogCoordPointerEXT",
    "DrawArrays", "MultiDrawArraysEXT", "DrawRangeElements",
    "DeleteTextures", "TexImage2D", "CopyTexSubImage2D", "TexSubImage2D",
    "ReadPixels", "GetTexImage", "GetError",
    "FlushVertexArrayRangeAPPLE", "SetFenceAPPLE", "wait fence",
    "BindProgramARB", "ProgramEnvParameter4fvARB", "ProgramEnvParameters4fvEXT",
    "call", "present",
    "decode", "BindBufferARB", "BufferSubDataARB",
};

typedef struct { u32 op, len; } Hdr;

typedef struct { GLenum a; } A1e;
typedef struct { GLenum a; GLuint b; } A_eu;
typedef struct { GLenum a, b; GLint v; } A_eei;
typedef struct { GLenum a, b; GLfloat v; } A_eef;
typedef struct { GLenum a, b; GLfloat v[4]; } A_eef4;
typedef struct { GLenum a; GLint v; } A_ei;
typedef struct { GLenum a; GLfloat v; } A_ef;
typedef struct { GLenum a; GLfloat v[4]; } A_ef4;
typedef struct { GLenum a, b; } A_ee;
typedef struct { GLfloat m[16]; } A_m16;
typedef struct { GLdouble v[6]; } A_d6;
typedef struct { GLdouble a, b; } A_d2;
typedef struct { GLboolean r, g, b, a; } A_b4;
typedef struct { GLenum f; GLclampf ref; } A_alpha;
typedef struct { GLclampf v[4]; } A_f4;
typedef struct { GLdouble d; } A_d;
typedef struct { GLbitfield m; } A_bits;
typedef struct { GLint x, y; GLsizei w, h; } A_rect;
typedef struct { GLfloat x, y; } A_f2;
typedef struct { GLint size; GLenum type; GLsizei stride; const GLvoid* p; } A_ptr;
typedef struct { GLenum type; GLsizei stride; const GLvoid* p; } A_ptr2;
typedef struct { GLenum mode; GLint first; GLsizei count; } A_draw;
typedef struct { GLenum mode; GLsizei n; /* GLint first[n]; GLsizei count[n] follow */ } A_multi;
typedef struct { GLenum mode; GLuint lo, hi; GLsizei n; GLenum type; u32 bytes; /* indices follow */ } A_range;
typedef struct { GLsizei n; /* GLuint names[n] follow */ } A_del;
typedef struct {
    GLenum target; GLint level, ifmt; GLsizei w, h; GLint border; GLenum fmt, type;
    void* px; u32 owned; /* owned: free(px) after the call; else px is in the stream or NULL */
} A_teximg;
/* M38: a sub-image upload whose pixels the replay frees (the movie texture) */
typedef struct {
    GLenum target; GLint level, xo, yo; GLsizei w, h; GLenum fmt, type; void* px;
    volatile int* done; /* NULL: free(px) after the call; else *done = 1 (a pool's buffer) */
} A_texsub;
typedef struct { GLenum target; GLint level, xo, yo, x, y; GLsizei w, h; } A_copysub;
typedef struct { GLint x, y; GLsizei w, h; GLenum fmt, type; GLvoid* out; } A_readpx;
typedef struct { GLenum target; GLint level; GLenum fmt, type; GLvoid* out; } A_gettex;
typedef struct { GLenum* out; } A_geterr;
typedef struct { GLsizei len; const GLvoid* p; } A_flushvar;
typedef struct { GLuint id; } A_bindbuf;
typedef struct { long off, n; const void* p; } A_bufsub;
typedef struct { GLuint f; int chunk; u32 epoch; } A_fence;
typedef struct { GLenum target; GLuint idx; GLfloat v[4]; } A_env4;
typedef struct { GLenum target; GLuint idx; GLsizei n; /* n*4 floats follow */ } A_envn;
typedef struct { void (*fn)(void*); u32 n; /* args follow */ } A_call;
typedef struct { unsigned frame; double t_rec; } A_present;
typedef struct { u32 done; u32 verts; GxDecJob job; } A_decode; /* done: set by the reader */

/* ---- the stream ------------------------------------------------------------ */

#define RT_BYTES (16u << 20) /* 16 MB: a drawn frame is ~300 KB of records; a CPU-path
                              * draw stashes up to MAX_VERTS x OUT_MAX_STRIDE (5 MB) */
#define RT_MASK (RT_BYTES - 1)
#define RT_ALIGN 8u

static u8* buf;
static u32 wr;                /* the writer's private position */
static volatile u32 wr_pub;   /* published to the reader */
static volatile u32 rd;       /* published by the reader */
/* M29: the decode cursor (PLAN.md 44.1).  The reader runs it ahead of `rd`
 * through every published record, executing the OP_DECODE ones as soon as
 * they exist and marking them done; the replay behind it skips those.
 * `dec_pub` is what the game thread's decode joins wait on. */
static u32 dec;
static volatile u32 dec_pub;
static int decmode;           /* --rtdecode: 0 off, 1 joined at once, 2 at the retrace */
/* M33: --rtdecode auto (3).  Per drawn frame a share of the display-list
 * decode stays on the game thread, sized from the last drawn frame's two
 * halves so that the game thread's cycle (its drawn frame + a consumed one)
 * and the render thread's (replay + the decode left to it) come out equal
 * (PLAN.md 48.2).  Everything here is the game thread's except what the
 * reader publishes at the present (st_last_frame_ms, st_last_dec_ms). */
static double au_share;        /* of this frame's vertices, the game thread's fraction */
static double au_acc;          /* the spread: an accumulator in vertices */
static unsigned long au_fr_gverts, au_fr_rverts; /* this frame's split, in vertices */
static double au_fr_gdec_s;    /* this frame's decode time on the game thread */
static double au_gd_ms, au_cc_ms; /* the game thread's drawn frame (less its decode), consumed frame */
static double au_rate_g, au_rate_r; /* ms a vertex, each side (a running mean) */
static double au_prev_gdec_ms;     /* M40: the last drawn frame's game-thread decode */
static double au_last_gd, au_last_cc, au_last_r, au_last_d, au_last_want; /* the last plan's inputs */
static unsigned long au_frames, au_frames_split, au_gverts, au_rverts;
static double au_gdec_s, au_share_sum, au_share_max;
static int mode;              /* 0 direct, 1 inline, 2 join per frame, 3 overlap */
static pthread_t thread;
static int thread_up, quit;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER; /* writer -> reader */
static pthread_cond_t cv_done = PTHREAD_COND_INITIALIZER; /* reader -> writer */
static volatile int reader_asleep;
static volatile int writer_waiting;
static SDL_Window* win;
static SDL_GLContext ctx;

/* the extensions, resolved here for the replay (the writers keep their own) */
typedef void (*fn_multidraw_t)(GLenum, const GLint*, const GLsizei*, GLsizei);
typedef void (*fn_range_t)(GLsizei, const GLvoid*);
typedef void (*fn_fence_t)(GLuint);
typedef GLboolean (*fn_fence_test_t)(GLuint);
typedef void (*fn_bindprog_t)(GLenum, GLuint);
typedef void (*fn_env4_t)(GLenum, GLuint, const GLfloat*);
typedef void (*fn_envn_t)(GLenum, GLuint, GLsizei, const GLfloat*);
typedef void (*fn_fogptr_t)(GLenum, GLsizei, const void*);
static fn_multidraw_t x_MultiDrawArraysEXT;
static fn_range_t x_FlushVertexArrayRangeAPPLE;
typedef void (*fn_bindbuf_t)(GLenum, GLuint);
typedef void (*fn_bufsub_t)(GLenum, long, long, const void*);
static fn_bindbuf_t x_BindBufferARB;
static fn_bufsub_t x_BufferSubDataARB;
#define RT_ARRAY_BUFFER_ARB 0x8892
static fn_fence_t x_SetFenceAPPLE, x_FinishFenceAPPLE;
static fn_fence_test_t x_TestFenceAPPLE;
static fn_bindprog_t x_BindProgramARB;
static fn_env4_t x_ProgramEnvParameter4fvARB;
static fn_envn_t x_ProgramEnvParameters4fvEXT;
static fn_fogptr_t x_FogCoordPointerEXT;

/* ---- the statistics ---------------------------------------------------------- */

static unsigned long st_records, st_bytes, st_frames;
static u32 st_frame_bytes, st_frame_bytes_peak, st_frame_records;
static double st_replay_s;         /* time the reader spent replaying */
static double st_frame_replay_s;   /* ... in the frame being replayed */
static double st_last_frame_ms;    /* the last presented frame's replay */
static double st_frame_ms_sum; static unsigned long st_frame_ms_n; static double st_frame_ms_max;
static double st_tail_s, st_tail_max; /* present executed - present recorded */
static unsigned long st_fence_waits, st_fence_blocked;
static unsigned long st_gate_ok, st_gate_waited, st_gate_busy;
static double st_gate_wait_s, st_gate_wait_max;
static unsigned long st_ring_waits; static double st_ring_wait_s, st_ring_wait_max;
static unsigned long st_full_waits; static double st_full_wait_s;
static unsigned long st_reader_sleeps, st_writer_wakes;
static unsigned long st_names;
static unsigned long st_owned_frees, st_stash_bytes;
static unsigned long st_dec_records, st_dec_ahead, st_dec_late; /* by the decode cursor / by the replay */
static unsigned long st_dec_verts;
static double st_dec_s, st_frame_dec_s, st_last_dec_ms, st_dec_ms_sum, st_dec_ms_max;
#define JOIN_KINDS 12
static struct { const char* why; unsigned long n; double s, max; } joins[JOIN_KINDS];
static int njoins;
/* --rtsplit: the replay timed by class of record (two timer reads a record,
 * ~2,800 records a frame: an instrument, not a default) */
enum { RC_STATE, RC_DRAW, RC_TEX, RC_PRESENT, RC_OTHER, RC_N };
static double st_class_s[RC_N];
static const char* const class_name[RC_N] = { "state", "draw", "tex", "present", "other" };

/* The vertex ring's chunks (gl13.c VAR_CHUNKS), two fences each (PLAN.md
 * 42.1c).  The writer leaving chunk c records a SET_FENCE carrying c and an
 * epoch; re-entering it a lap later it must know (1) the render thread has
 * *issued* every draw that read the chunk -- rd past `left_pos[c]`, the
 * stream position of that SET_FENCE -- and (2) the GPU has *finished* them
 * -- the reader tested that fence and published `gpu_epoch[c]`.  The reader
 * tests its pending fences after every present, every 256 records, and
 * while idle, so the common case (a chunk left three frames ago) never
 * waits.  The direct path finished the fence before writing; the inline
 * twin replays the WAIT_FENCE record at once, which is the same thing. */
#define RING_CHUNKS 64
static u32 ring_left_pos[RING_CHUNKS];
static u32 ring_set_epoch[RING_CHUNKS];        /* the writer's */
static volatile u32 ring_gpu_epoch[RING_CHUNKS]; /* the reader's: this epoch's fence finished */
static struct { GLuint f; u32 epoch; int pending; } ring_fence[RING_CHUNKS]; /* the reader's */
static unsigned long st_ring_gpu_waits;

static double now(void) { return port_now_seconds(); }

/* ---- the writer ------------------------------------------------------------ */

static void reader_wake(void) {
    if (reader_asleep) {
        pthread_mutex_lock(&mu);
        if (reader_asleep) { /* still: it has not woken and cleared the flag */
            pthread_cond_signal(&cv_work);
            st_writer_wakes++;
        }
        pthread_mutex_unlock(&mu);
    }
}

static void publish(void) {
    RT_ACQ_REL(); /* release: the record's bytes before the position */
    wr_pub = wr;
    if (mode >= 2) {
        RT_FULL(); /* the store above before the load in reader_wake (Dekker) */
        reader_wake();
    }
}

static void replay_upto(u32 to);

/* wait until the reader's cursor `var` (rd, or the decode cursor) has passed
 * `pos` (var - pos >= 0 in difference arithmetic) */
static void wait_var(volatile u32* var, u32 pos, const char* why, double* acc_s, double* acc_max) {
    double t0;
    int spins = 0;
    if ((s32)(*var - pos) >= 0) {
        return;
    }
    if (mode == 1) {
        replay_upto(pos);
        return;
    }
    t0 = now();
    publish();
    while ((s32)(*var - pos) < 0) {
        if (spins++ < 4000) {
            RT_ACQ_REL();
            continue;
        }
        pthread_mutex_lock(&mu);
        writer_waiting = 1;
        RT_FULL(); /* the store above before the load below (Dekker) */
        while ((s32)(*var - pos) < 0) {
            pthread_cond_wait(&cv_done, &mu);
        }
        writer_waiting = 0;
        pthread_mutex_unlock(&mu);
    }
    RT_ACQ_REL(); /* acquire: what the reader wrote (a read-back's pixels) after rd */
    {
        double d = now() - t0;
        if (acc_s) {
            *acc_s += d;
        }
        if (acc_max && d > *acc_max) {
            *acc_max = d;
        }
        (void)why;
    }
}
static void wait_pos(u32 pos, const char* why, double* acc_s, double* acc_max) {
    wait_var(&rd, pos, why, acc_s, acc_max);
}

static u32 last_op; /* the op of the record being built (done() publishes by it) */
static void* rec(u32 op, size_t argbytes) {
    u32 len = (u32)((sizeof(Hdr) + argbytes + RT_ALIGN - 1) & ~(RT_ALIGN - 1));
    u32 off = wr & RT_MASK;
    Hdr* h;
    if (len > RT_BYTES / 2) {
        port_fatal("render thread: a %u-byte record does not fit the stream", len);
    }
    if (off + len > RT_BYTES) {
        /* pad to the end with a WRAP so the record is contiguous */
        u32 pad = RT_BYTES - off;
        if ((u32)(wr + pad - rd) > RT_BYTES) {
            double t0 = now();
            st_full_waits++;
            wait_pos(wr + pad - RT_BYTES, "full", NULL, NULL);
            st_full_wait_s += now() - t0;
        }
        h = (Hdr*)(buf + off);
        h->op = OP_WRAP;
        h->len = pad;
        wr += pad;
        off = 0;
    }
    if ((u32)(wr + len - rd) > RT_BYTES) {
        /* the stream is full: the reader has this much left to consume */
        double t0 = now();
        st_full_waits++;
        wait_pos(wr + len - RT_BYTES, "full", NULL, NULL);
        st_full_wait_s += now() - t0;
    }
    h = (Hdr*)(buf + off);
    h->op = op;
    h->len = len;
    wr += len;
    last_op = op;
    st_records++;
    st_bytes += len;
    st_frame_bytes += len;
    st_frame_records++;
    return h + 1;
}

/* every twin ends here: the record is complete.  A draw, an upload, a
 * present, a call or a read publishes at once (the reader's next unit of
 * work is behind it); a state record waits for the next one of those, or
 * for the 32nd state record, so the two barriers of a publish are paid a few
 * times per batch rather than ~2,900 times a frame.  In inline mode the
 * record is replayed now. */
static u32 unpublished;
static void done_op(u32 op) {
    if (mode == 1) {
        replay_upto(wr);
        return;
    }
    switch (op) {
        case OP_DRAW_ARRAYS: case OP_MULTI_DRAW: case OP_DRAW_RANGE: case OP_PRESENT:
        case OP_CALL: case OP_READ_PIXELS: case OP_GET_TEX_IMAGE: case OP_GET_ERROR:
        case OP_TEXIMAGE: case OP_COPY_TEX_SUB: case OP_TEXSUBIMAGE: case OP_CLEAR: case OP_FINISH:
        case OP_BEGIN: case OP_END: case OP_WAIT_FENCE: case OP_SET_FENCE: case OP_DECODE:
            unpublished = 0;
            publish();
            return;
        default:
            if (++unpublished >= 32) {
                unpublished = 0;
                publish();
            }
            return;
    }
}
static void done(void) { done_op(last_op); }

#define REC(op, T) T* a = (T*)rec(op, sizeof(T))

/* ---- the twins ---------------------------------------------------------------- */

#define TWIN1(name, glfn, T, e1)                                               \
    void rt_##name(GLenum x) {                                                 \
        if (!rt_recording) { glfn(x); return; }                                \
        { REC(e1, T); a->a = x; }                                              \
        done();                                                                \
    }
TWIN1(glEnable, glEnable, A1e, OP_ENABLE)
TWIN1(glDisable, glDisable, A1e, OP_DISABLE)
TWIN1(glEnableClientState, glEnableClientState, A1e, OP_ENABLE_CS)
TWIN1(glDisableClientState, glDisableClientState, A1e, OP_DISABLE_CS)
TWIN1(glActiveTexture, glActiveTexture, A1e, OP_ACTIVE_TEX)
TWIN1(glClientActiveTexture, glClientActiveTexture, A1e, OP_CLIENT_ACTIVE_TEX)
TWIN1(glMatrixMode, glMatrixMode, A1e, OP_MATRIX_MODE)
TWIN1(glDepthFunc, glDepthFunc, A1e, OP_DEPTH_FUNC)
TWIN1(glCullFace, glCullFace, A1e, OP_CULL)
TWIN1(glFrontFace, glFrontFace, A1e, OP_FRONT)
TWIN1(glShadeModel, glShadeModel, A1e, OP_SHADE)
TWIN1(glBlendEquation, glBlendEquation, A1e, OP_BLEND_EQ)
TWIN1(glReadBuffer, glReadBuffer, A1e, OP_READ_BUFFER)
TWIN1(glBegin, glBegin, A1e, OP_BEGIN)

void rt_glBindTexture(GLenum t, GLuint n) {
    if (!rt_recording) { glBindTexture(t, n); return; }
    { REC(OP_BIND_TEX, A_eu); a->a = t; a->b = n; }
    done();
}
void rt_glTexParameteri(GLenum t, GLenum p, GLint v) {
    if (!rt_recording) { glTexParameteri(t, p, v); return; }
    { REC(OP_TEXPARAM_I, A_eei); a->a = t; a->b = p; a->v = v; }
    done();
}
void rt_glTexParameterf(GLenum t, GLenum p, GLfloat v) {
    if (!rt_recording) { glTexParameterf(t, p, v); return; }
    { REC(OP_TEXPARAM_F, A_eef); a->a = t; a->b = p; a->v = v; }
    done();
}
void rt_glTexEnvi(GLenum t, GLenum p, GLint v) {
    if (!rt_recording) { glTexEnvi(t, p, v); return; }
    { REC(OP_TEXENV_I, A_eei); a->a = t; a->b = p; a->v = v; }
    done();
}
void rt_glTexEnvf(GLenum t, GLenum p, GLfloat v) {
    if (!rt_recording) { glTexEnvf(t, p, v); return; }
    { REC(OP_TEXENV_F, A_eef); a->a = t; a->b = p; a->v = v; }
    done();
}
void rt_glTexEnvfv(GLenum t, GLenum p, const GLfloat* v) {
    if (!rt_recording) { glTexEnvfv(t, p, v); return; }
    { REC(OP_TEXENV_FV, A_eef4); a->a = t; a->b = p; memcpy(a->v, v, sizeof(a->v)); }
    done();
}
void rt_glLoadIdentity(void) {
    if (!rt_recording) { glLoadIdentity(); return; }
    rec(OP_LOAD_IDENTITY, 0);
    done();
}
void rt_glLoadMatrixf(const GLfloat* m) {
    if (!rt_recording) { glLoadMatrixf(m); return; }
    { REC(OP_LOAD_MATRIX, A_m16); memcpy(a->m, m, sizeof(a->m)); }
    done();
}
void rt_glPushMatrix(void) {
    if (!rt_recording) { glPushMatrix(); return; }
    rec(OP_PUSH, 0);
    done();
}
void rt_glPopMatrix(void) {
    if (!rt_recording) { glPopMatrix(); return; }
    rec(OP_POP, 0);
    done();
}
void rt_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (!rt_recording) { glOrtho(l, r, b, t, n, f); return; }
    { REC(OP_ORTHO, A_d6); a->v[0] = l; a->v[1] = r; a->v[2] = b; a->v[3] = t; a->v[4] = n; a->v[5] = f; }
    done();
}
void rt_glDepthMask(GLboolean b) {
    if (!rt_recording) { glDepthMask(b); return; }
    { REC(OP_DEPTH_MASK, A_b4); a->r = b; }
    done();
}
void rt_glDepthRange(GLclampd n, GLclampd f) {
    if (!rt_recording) { glDepthRange(n, f); return; }
    { REC(OP_DEPTH_RANGE, A_d2); a->a = n; a->b = f; }
    done();
}
void rt_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean al) {
    if (!rt_recording) { glColorMask(r, g, b, al); return; }
    { REC(OP_COLOR_MASK, A_b4); a->r = r; a->g = g; a->b = b; a->a = al; }
    done();
}
void rt_glPolygonMode(GLenum f, GLenum m) {
    if (!rt_recording) { glPolygonMode(f, m); return; }
    { REC(OP_POLYMODE, A_ee); a->a = f; a->b = m; }
    done();
}
void rt_glHint(GLenum t, GLenum m) {
    if (!rt_recording) { glHint(t, m); return; }
    { REC(OP_HINT, A_ee); a->a = t; a->b = m; }
    done();
}
void rt_glBlendFunc(GLenum s, GLenum d) {
    if (!rt_recording) { glBlendFunc(s, d); return; }
    { REC(OP_BLEND_FUNC, A_ee); a->a = s; a->b = d; }
    done();
}
void rt_glAlphaFunc(GLenum f, GLclampf ref) {
    if (!rt_recording) { glAlphaFunc(f, ref); return; }
    { REC(OP_ALPHA_FUNC, A_alpha); a->f = f; a->ref = ref; }
    done();
}
void rt_glFogi(GLenum p, GLint v) {
    if (!rt_recording) { glFogi(p, v); return; }
    { REC(OP_FOG_I, A_ei); a->a = p; a->v = v; }
    done();
}
void rt_glLineWidth(GLfloat w) {
    if (!rt_recording) { glLineWidth(w); return; }
    { REC(OP_LINE_WIDTH, A_ef); a->a = 0; a->v = w; }
    done();
}
void rt_glFogf(GLenum p, GLfloat v) {
    if (!rt_recording) { glFogf(p, v); return; }
    { REC(OP_FOG_F, A_ef); a->a = p; a->v = v; }
    done();
}
void rt_glFogfv(GLenum p, const GLfloat* v) {
    if (!rt_recording) { glFogfv(p, v); return; }
    { REC(OP_FOG_FV, A_ef4); a->a = p; memcpy(a->v, v, p == GL_FOG_COLOR ? 16 : 4); }
    done();
}
void rt_glLightf(GLenum l, GLenum p, GLfloat v) {
    if (!rt_recording) { glLightf(l, p, v); return; }
    { REC(OP_LIGHT_F, A_eef); a->a = l; a->b = p; a->v = v; }
    done();
}
void rt_glLightfv(GLenum l, GLenum p, const GLfloat* v) {
    if (!rt_recording) { glLightfv(l, p, v); return; }
    { REC(OP_LIGHT_FV, A_eef4); a->a = l; a->b = p; memcpy(a->v, v, sizeof(a->v)); }
    done();
}
void rt_glLightModeli(GLenum p, GLint v) {
    if (!rt_recording) { glLightModeli(p, v); return; }
    { REC(OP_LIGHTMODEL_I, A_ei); a->a = p; a->v = v; }
    done();
}
void rt_glLightModelfv(GLenum p, const GLfloat* v) {
    if (!rt_recording) { glLightModelfv(p, v); return; }
    { REC(OP_LIGHTMODEL_FV, A_ef4); a->a = p; memcpy(a->v, v, sizeof(a->v)); }
    done();
}
void rt_glMaterialfv(GLenum face, GLenum p, const GLfloat* v) {
    if (!rt_recording) { glMaterialfv(face, p, v); return; }
    { REC(OP_MATERIAL_FV, A_eef4); a->a = face; a->b = p; memcpy(a->v, v, sizeof(a->v)); }
    done();
}
void rt_glColorMaterial(GLenum face, GLenum m) {
    if (!rt_recording) { glColorMaterial(face, m); return; }
    { REC(OP_COLOR_MATERIAL, A_ee); a->a = face; a->b = m; }
    done();
}
void rt_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf al) {
    if (!rt_recording) { glClearColor(r, g, b, al); return; }
    { REC(OP_CLEAR_COLOR, A_f4); a->v[0] = r; a->v[1] = g; a->v[2] = b; a->v[3] = al; }
    done();
}
void rt_glClearDepth(GLclampd d) {
    if (!rt_recording) { glClearDepth(d); return; }
    { REC(OP_CLEAR_DEPTH, A_d); a->d = d; }
    done();
}
void rt_glClear(GLbitfield m) {
    if (!rt_recording) { glClear(m); return; }
    { REC(OP_CLEAR, A_bits); a->m = m; }
    done();
}
void rt_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!rt_recording) { glViewport(x, y, w, h); return; }
    { REC(OP_VIEWPORT, A_rect); a->x = x; a->y = y; a->w = w; a->h = h; }
    done();
}
void rt_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!rt_recording) { glScissor(x, y, w, h); return; }
    { REC(OP_SCISSOR, A_rect); a->x = x; a->y = y; a->w = w; a->h = h; }
    done();
}
void rt_glPixelStorei(GLenum p, GLint v) {
    if (!rt_recording) { glPixelStorei(p, v); return; }
    { REC(OP_PIXELSTORE, A_ei); a->a = p; a->v = v; }
    done();
}
void rt_glFinish(void) {
    if (!rt_recording) { glFinish(); return; }
    rec(OP_FINISH, 0);
    done();
}
void rt_glEnd(void) {
    if (!rt_recording) { glEnd(); return; }
    rec(OP_END, 0);
    done();
}
void rt_glTexCoord2f(GLfloat s, GLfloat t) {
    if (!rt_recording) { glTexCoord2f(s, t); return; }
    { REC(OP_TEXCOORD2F, A_f2); a->x = s; a->y = t; }
    done();
}
void rt_glVertex2f(GLfloat x, GLfloat y) {
    if (!rt_recording) { glVertex2f(x, y); return; }
    { REC(OP_VERTEX2F, A_f2); a->x = x; a->y = y; }
    done();
}
void rt_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p) {
    if (!rt_recording) { glVertexPointer(size, type, stride, p); return; }
    { REC(OP_VERTEX_PTR, A_ptr); a->size = size; a->type = type; a->stride = stride; a->p = p; }
    done();
}
void rt_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p) {
    if (!rt_recording) { glColorPointer(size, type, stride, p); return; }
    { REC(OP_COLOR_PTR, A_ptr); a->size = size; a->type = type; a->stride = stride; a->p = p; }
    done();
}
void rt_glNormalPointer(GLenum type, GLsizei stride, const GLvoid* p) {
    if (!rt_recording) { glNormalPointer(type, stride, p); return; }
    { REC(OP_NORMAL_PTR, A_ptr2); a->type = type; a->stride = stride; a->p = p; }
    done();
}
void rt_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p) {
    if (!rt_recording) { glTexCoordPointer(size, type, stride, p); return; }
    { REC(OP_TEXCOORD_PTR, A_ptr); a->size = size; a->type = type; a->stride = stride; a->p = p; }
    done();
}
void rt_ext_fogcoord_pointer(GLenum type, GLsizei stride, const GLvoid* p) {
    if (!rt_recording) {
        if (x_FogCoordPointerEXT) { x_FogCoordPointerEXT(type, stride, p); }
        return;
    }
    { REC(OP_FOGCOORD_PTR, A_ptr2); a->type = type; a->stride = stride; a->p = p; }
    done();
}
void rt_glDrawArrays(GLenum mode_, GLint first, GLsizei count) {
    if (!rt_recording) { glDrawArrays(mode_, first, count); return; }
    { REC(OP_DRAW_ARRAYS, A_draw); a->mode = mode_; a->first = first; a->count = count; }
    done();
}
void rt_ext_multi_draw_arrays(GLenum mode_, const GLint* first, const GLsizei* count, GLsizei n) {
    if (!rt_recording) { x_MultiDrawArraysEXT(mode_, first, count, n); return; }
    {
        A_multi* a = (A_multi*)rec(OP_MULTI_DRAW, sizeof(A_multi) + (size_t)n * 8);
        GLint* f = (GLint*)(a + 1);
        GLsizei* c = (GLsizei*)(f + n);
        a->mode = mode_;
        a->n = n;
        memcpy(f, first, (size_t)n * sizeof(GLint));
        memcpy(c, count, (size_t)n * sizeof(GLsizei));
    }
    done();
}
void rt_glDrawRangeElements(GLenum mode_, GLuint lo, GLuint hi, GLsizei n, GLenum type,
                            const GLvoid* idx) {
    if (!rt_recording) { glDrawRangeElements(mode_, lo, hi, n, type, idx); return; }
    {
        u32 bytes = (u32)n * (type == GL_UNSIGNED_INT ? 4u : type == GL_UNSIGNED_SHORT ? 2u : 1u);
        A_range* a = (A_range*)rec(OP_DRAW_RANGE, sizeof(A_range) + bytes);
        a->mode = mode_; a->lo = lo; a->hi = hi; a->n = n; a->type = type; a->bytes = bytes;
        memcpy(a + 1, idx, bytes);
        st_stash_bytes += bytes;
    }
    done();
}
/* GL names: an unused name bound with glBindTexture is a new object (GL 1.3
 * §3.8.11), so once the stream owns GL the game thread hands out names from
 * a counter well past anything the driver generated at init, and a cold
 * scene's hundreds of new slots cost no join. */
void rt_glGenTextures(GLsizei n, GLuint* out) {
    static GLuint next_name = 0x100000u;
    GLsizei i;
    if (!rt_recording) { glGenTextures(n, out); return; }
    for (i = 0; i < n; i++) {
        out[i] = next_name++;
        st_names++;
    }
}
void rt_glDeleteTextures(GLsizei n, const GLuint* names) {
    if (!rt_recording) { glDeleteTextures(n, names); return; }
    {
        A_del* a = (A_del*)rec(OP_DELETE_TEX, sizeof(A_del) + (size_t)n * sizeof(GLuint));
        a->n = n;
        memcpy(a + 1, names, (size_t)n * sizeof(GLuint));
    }
    done();
}
static u32 pixel_bytes(GLenum fmt, GLenum type, GLsizei w, GLsizei h) {
    u32 bpp = (fmt == GL_RGBA || fmt == GL_BGRA) ? 4 : fmt == GL_RGB ? 3 : 1;
    if (type != GL_UNSIGNED_BYTE) {
        port_fatal("render thread: glTexImage2D with a pixel type the stream does not size (0x%x)",
                   (unsigned)type);
    }
    return (u32)w * (u32)h * bpp;
}
void rt_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                     GLenum fmt, GLenum type, const GLvoid* px) {
    if (!rt_recording) { glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px); return; }
    {
        u32 bytes = px ? pixel_bytes(fmt, type, w, h) : 0;
        A_teximg* a = (A_teximg*)rec(OP_TEXIMAGE, sizeof(A_teximg) + bytes);
        a->target = target; a->level = level; a->ifmt = ifmt; a->w = w; a->h = h;
        a->border = border; a->fmt = fmt; a->type = type; a->owned = 0;
        if (px) {
            memcpy(a + 1, px, bytes);
            a->px = (void*)(a + 1);
            st_stash_bytes += bytes;
        } else {
            a->px = NULL;
        }
    }
    done();
}
void rt_teximage2d_owned(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                         GLint border, GLenum fmt, GLenum type, void* px) {
    if (!rt_recording) {
        glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
        free(px);
        return;
    }
    {
        REC(OP_TEXIMAGE, A_teximg);
        a->target = target; a->level = level; a->ifmt = ifmt; a->w = w; a->h = h;
        a->border = border; a->fmt = fmt; a->type = type; a->px = px; a->owned = 1;
    }
    done();
}
/* M38 (PLAN.md 53): the movie frame, into the POT texture made for it once */
void rt_texsubimage2d_owned(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                            GLenum fmt, GLenum type, void* px, volatile int* done_flag) {
    if (!rt_recording) {
        glTexSubImage2D(target, level, xo, yo, w, h, fmt, type, px);
        if (done_flag) {
            *done_flag = 1;
        } else {
            free(px);
        }
        return;
    }
    {
        REC(OP_TEXSUBIMAGE, A_texsub);
        a->target = target; a->level = level; a->xo = xo; a->yo = yo; a->w = w; a->h = h;
        a->fmt = fmt; a->type = type; a->px = px; a->done = done_flag;
    }
    done();
}
void rt_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,
                            GLsizei w, GLsizei h) {
    if (!rt_recording) { glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h); return; }
    { REC(OP_COPY_TEX_SUB, A_copysub); a->target = target; a->level = level; a->xo = xo; a->yo = yo; a->x = x; a->y = y; a->w = w; a->h = h; }
    done();
}

/* the joins the game thread asks for, by name */
static void join_count(const char* why, double d) {
    int i;
    for (i = 0; i < njoins; i++) {
        if (joins[i].why == why || strcmp(joins[i].why, why) == 0) {
            break;
        }
    }
    if (i == njoins) {
        if (njoins == JOIN_KINDS) {
            i = JOIN_KINDS - 1;
        } else {
            joins[njoins++].why = why;
        }
    }
    joins[i].n++;
    joins[i].s += d;
    if (d > joins[i].max) {
        joins[i].max = d;
    }
}

void rt_join(const char* why) {
    double t0;
    if (!rt_recording || mode < 2) {
        if (mode == 1) {
            replay_upto(wr);
        }
        return;
    }
    if ((s32)(rd - wr) >= 0) {
        join_count(why, 0.0);
        return;
    }
    t0 = now();
    wait_pos(wr, why, NULL, NULL);
    join_count(why, now() - t0);
}

void rt_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, GLvoid* out) {
    if (!rt_recording) { glReadPixels(x, y, w, h, fmt, type, out); return; }
    { REC(OP_READ_PIXELS, A_readpx); a->x = x; a->y = y; a->w = w; a->h = h; a->fmt = fmt; a->type = type; a->out = out; }
    done();
    rt_join("glReadPixels");
}
void rt_glGetTexImage(GLenum target, GLint level, GLenum fmt, GLenum type, GLvoid* out) {
    if (!rt_recording) { glGetTexImage(target, level, fmt, type, out); return; }
    { REC(OP_GET_TEX_IMAGE, A_gettex); a->target = target; a->level = level; a->fmt = fmt; a->type = type; a->out = out; }
    done();
    rt_join("glGetTexImage");
}
GLenum rt_glGetError(void) {
    GLenum e = GL_NO_ERROR;
    if (!rt_recording) { return glGetError(); }
    { REC(OP_GET_ERROR, A_geterr); a->out = &e; }
    done();
    rt_join("glGetError");
    return e;
}
const GLubyte* rt_glGetString(GLenum name) {
    if (rt_recording) {
        static int said;
        if (!said++) {
            port_log("port> render thread: glGetString(0x%x) while recording -- an init-time "
                     "probe reached at run time; answered directly (a bug to fix)\n", (unsigned)name);
        }
        rt_join("glGetString");
    }
    return glGetString(name);
}
void rt_glGetIntegerv(GLenum p, GLint* v) {
    if (rt_recording) {
        static int said;
        if (!said++) {
            port_log("port> render thread: glGetIntegerv(0x%x) while recording -- answered "
                     "directly after a join (a bug to fix)\n", (unsigned)p);
        }
        rt_join("glGetIntegerv");
    }
    glGetIntegerv(p, v);
}
/* M40: the vertex cache's buffer object -- a bind, and an upload whose bytes
 * (the cache's own staging copy) stay put until the cache's reset, which
 * joins the replay first */
void rt_ext_bind_buffer(GLuint id) {
    if (!x_BindBufferARB) { return; }
    if (!rt_recording) { x_BindBufferARB(RT_ARRAY_BUFFER_ARB, id); return; }
    { REC(OP_BIND_BUFFER, A_bindbuf); a->id = id; }
    done();
}
void rt_ext_buffer_subdata(long off, long n, const void* p) {
    if (!x_BufferSubDataARB) { return; }
    if (!rt_recording) { x_BufferSubDataARB(RT_ARRAY_BUFFER_ARB, off, n, p); return; }
    { REC(OP_BUFFER_SUB, A_bufsub); a->off = off; a->n = n; a->p = p; }
    done();
}
void rt_ext_flush_var(GLsizei len, const GLvoid* p) {
    if (!rt_recording) { x_FlushVertexArrayRangeAPPLE(len, p); return; }
    { REC(OP_FLUSH_VAR, A_flushvar); a->len = len; a->p = p; }
    done();
}
void rt_ext_set_fence(GLuint f, int chunk) {
    if (!rt_recording) { x_SetFenceAPPLE(f); return; }
    if (chunk >= 0 && chunk < RING_CHUNKS) {
        ring_set_epoch[chunk]++;
    }
    {
        REC(OP_SET_FENCE, A_fence);
        a->f = f;
        a->chunk = chunk;
        a->epoch = chunk >= 0 && chunk < RING_CHUNKS ? ring_set_epoch[chunk] : 0;
    }
    if (chunk >= 0 && chunk < RING_CHUNKS) {
        ring_left_pos[chunk] = wr; /* every draw reading the chunk is before this */
    }
    done();
}
void rt_ext_wait_fence(GLuint f, int chunk) {
    if (!rt_recording) {
        st_fence_waits++;
        if (!x_TestFenceAPPLE(f)) {
            st_fence_blocked++;
            x_FinishFenceAPPLE(f);
        }
        return;
    }
    if (mode == 1) {
        /* the inline twin: the same test-then-finish, replayed at once */
        REC(OP_WAIT_FENCE, A_fence);
        a->f = f;
        a->chunk = chunk;
        a->epoch = 0;
        done();
        return;
    }
    /* mode >= 2: rt_ring_enter has already waited for the reader to see the
     * fence finished; nothing to record */
    (void)f;
    (void)chunk;
}
void rt_ext_bind_program(GLenum target, GLuint id) {
    if (!rt_recording) { x_BindProgramARB(target, id); return; }
    { REC(OP_BIND_PROG, A_eu); a->a = target; a->b = id; }
    done();
}
void rt_ext_env_param4fv(GLenum target, GLuint idx, const GLfloat* v) {
    if (!rt_recording) { x_ProgramEnvParameter4fvARB(target, idx, v); return; }
    { REC(OP_ENV4, A_env4); a->target = target; a->idx = idx; memcpy(a->v, v, sizeof(a->v)); }
    done();
}
void rt_ext_env_params4fv(GLenum target, GLuint idx, GLsizei n, const GLfloat* v) {
    if (!rt_recording) { x_ProgramEnvParameters4fvEXT(target, idx, n, v); return; }
    {
        A_envn* a = (A_envn*)rec(OP_ENVN, sizeof(A_envn) + (size_t)n * 16);
        a->target = target; a->idx = idx; a->n = n;
        memcpy(a + 1, v, (size_t)n * 16);
    }
    done();
}

const void* rt_stash(const void* p, size_t n) {
    if (!rt_recording) {
        return p;
    }
    {
        void* a = rec(OP_NOP, n);
        memcpy(a, p, n);
        st_stash_bytes += n;
        done();
        return a;
    }
}

void rt_call(void (*fn)(void*), const void* args, size_t n, int sync) {
    if (!rt_recording) {
        void* copy = malloc(n ? n : 1);
        if (copy) {
            memcpy(copy, args, n);
            fn(copy);
            free(copy);
        }
        return;
    }
    {
        A_call* a = (A_call*)rec(OP_CALL, sizeof(A_call) + n);
        a->fn = fn;
        a->n = (u32)n;
        memcpy(a + 1, args, n);
    }
    done();
    if (sync) {
        rt_join("call");
    }
}

/* ---- M29: the decode records (PLAN.md 44) ---------------------------------- */

int rt_decode_on(void) { return rt_recording && decmode > 0; }
unsigned rt_pos(void) { return wr; }

/* M33: is this run the render thread's?  Modes 1 and 2: always.  Auto: the
 * frame's share is spread over its runs by vertex count (a Bresenham
 * accumulator, so a share of 0.3 takes about every third run's worth of
 * vertices rather than the first 30% of the frame -- the render thread's
 * backlog is what the balance is about, not where in the frame it sits). */
int rt_decode_want(unsigned verts) {
    if (!rt_recording || decmode == 0) {
        return 0;
    }
    if (decmode != 3 || au_share <= 0.0) {
        return 1;
    }
    au_acc += au_share * (double)verts;
    if (au_acc >= (double)verts) {
        au_acc -= (double)verts;
        return 0;
    }
    return 1;
}

/* the game thread decoded a run itself under auto (timed by the caller) */
void rt_decode_here(unsigned verts, double seconds) {
    au_fr_gverts += verts;
    au_fr_gdec_s += seconds;
}
void rt_decode_there(unsigned verts) { au_fr_rverts += verts; }

/* The retrace: the frame that just ended, drawn or consumed, and what the
 * game thread spent on it (entry to entry, the sleep and the joins out).
 * Then, for a drawn frame about to start, the plan.  Both from vi.c. */
#define AU_EMA(v, x) ((v) = (v) > 0.0 ? 0.5 * (v) + 0.5 * (x) : (x))
void rt_auto_frame_end(int drawn, double seconds) {
    if (decmode != 3) {
        return;
    }
    if (drawn) {
        double ms = seconds * 1000.0;
        double gd = ms - au_fr_gdec_s * 1000.0;
        if (gd < 0.0) {
            gd = 0.0;
        }
        AU_EMA(au_gd_ms, gd);
        if (au_fr_gverts) {
            AU_EMA(au_rate_g, au_fr_gdec_s * 1000.0 / (double)au_fr_gverts);
        }
        au_gverts += au_fr_gverts;
        au_rverts += au_fr_rverts;
        au_gdec_s += au_fr_gdec_s;
    } else {
        AU_EMA(au_cc_ms, seconds * 1000.0);
    }
}
void rt_auto_frame_begin(void) {
    double v, want, rg, rr, r, d;
    unsigned long vtot;
    if (decmode != 3) {
        return;
    }
    /* the gate drained the reader before this frame was allowed, so the
     * reader's last presented frame is the last drawn one: its decode time
     * over the vertices handed to it is the render thread's rate */
    if (au_fr_rverts && st_last_dec_ms > 0.0) {
        AU_EMA(au_rate_r, st_last_dec_ms / (double)au_fr_rverts);
    }
    vtot = au_fr_gverts + au_fr_rverts;
    au_prev_gdec_ms = au_fr_gdec_s * 1000.0;
    au_fr_gverts = au_fr_rverts = 0;
    au_fr_gdec_s = 0.0;
    au_acc = 0.0;
    au_frames++;
    /* the rates: a side that decoded nothing lately borrows the other's */
    rg = au_rate_g > 0.0 ? au_rate_g : au_rate_r;
    rr = au_rate_r > 0.0 ? au_rate_r : au_rate_g;
    r = st_last_frame_ms;      /* the reader's replay of the last presented frame */
    if (vtot == 0 || rr <= 0.0 || rg <= 0.0 || au_gd_ms <= 0.0) {
        au_share = 0.0;
        return;
    }
    d = (double)vtot * rr;     /* the whole decode, were it all the render thread's */
    au_last_gd = au_gd_ms;
    au_last_cc = au_cc_ms;
    au_last_r = r;
    au_last_d = d;
    /* the render thread's frame fits two retraces with room: leave it */
    if (r + d <= port_opt.rtauto_fit_ms) {
        au_share = 0.0;
        au_last_want = 0.0;
        return;
    }
    /* balance: gd + v*rg + cc = r + (vtot - v)*rr  ->  v */
    want = (r + d - au_gd_ms - au_cc_ms) / (rg + rr);
    au_last_want = want * rg;
    if (want <= 0.0) {
        au_share = 0.0;
        return;
    }
    v = want / (double)vtot;
    if (v > port_opt.rtauto_max) {
        v = port_opt.rtauto_max;
    }
    if (v * d < 1.0) {
        au_share = 0.0; /* under a millisecond: not worth the timers */
        return;
    }
    au_share = v;
    au_frames_split++;
    au_share_sum += v;
    if (v > au_share_max) {
        au_share_max = v;
    }
}
double rt_auto_last_share(void) { return decmode == 3 ? au_share : 0.0; }

/* M40 (PLAN.md 55): what --vcache auto weighs at a drawn frame's start -- the
 * render thread's last replay, the last frame's whole decode (both threads)
 * and the decode's cost a vertex.  0 when there is no split to read (one
 * CPU, the inline replay, --rtdecode 0..2): the cache is then always on. */
int rt_vcache_inputs(double* replay_ms, double* dec_ms, double* rate_ms, double* game_ms) {
    if (decmode != 3 || !rt_recording) {
        return 0;
    }
    *game_ms = au_gd_ms + au_cc_ms + au_prev_gdec_ms; /* the game thread's cycle */
    *replay_ms = st_last_frame_ms;
    *dec_ms = st_last_dec_ms + au_prev_gdec_ms;
    *rate_ms = au_rate_r > 0.0 ? au_rate_r : au_rate_g;
    return *rate_ms > 0.0;
}
double rt_auto_frame_gdec_ms(void) { return au_fr_gdec_s * 1000.0; }

/* the game thread hands a run to the render thread; the record is published
 * at once (the reader's decode cursor is what waits for it) */
void rt_decode_record(const GxDecJob* j) {
    /* `plan` is the job's last field: only the steps in use travel */
    size_t jb = offsetof(GxDecJob, plan) + (size_t)j->nplan * sizeof(DecStep);
    A_decode* a = (A_decode*)rec(OP_DECODE, offsetof(A_decode, job) + jb);
    a->done = 0;
    a->verts = 0;
    memcpy(&a->job, j, jb);
    st_dec_records++;
    done();
    if (decmode == 1) {
        rt_decode_join("after record (stage 1)");
    }
}

/* wait until the render thread has executed every decode recorded so far
 * (the decode cursor, not the replay: the draws may still be pending) */
void rt_decode_join(const char* why) {
    double t0;
    if (!rt_recording || decmode == 0) {
        return;
    }
    if (mode == 1) {
        replay_upto(wr);
        return;
    }
    if ((s32)(dec_pub - wr) >= 0) {
        join_count(why, 0.0);
        return;
    }
    t0 = now();
    wait_var(&dec_pub, wr, why, NULL, NULL);
    join_count(why, now() - t0);
}

/* the same, up to a stamped position only (a skinned HSF's last record) */
void rt_decode_join_pos(unsigned pos, const char* why) {
    double t0;
    if (!rt_recording || decmode == 0) {
        return;
    }
    if (mode == 1) {
        replay_upto(pos);
        return;
    }
    if ((s32)(dec_pub - pos) >= 0) {
        join_count(why, 0.0);
        return;
    }
    t0 = now();
    wait_var(&dec_pub, pos, why, NULL, NULL);
    join_count(why, now() - t0);
}

void port_vtx_rewrite(const char* who) {
    rt_decode_join(who);
    gx_vc_epoch++; /* M40: the vertex cache re-hashes what is about to be rewritten */
}

/* The vertex program compile, on the GL thread (gx_vprog.c's vp_compile):
 * the text in, the id and the driver's verdicts out.  Written here because a
 * call target must use the real GL -- gx_vprog.c's `gl*` are the twins, and
 * a twin reached from the replaying side would record into the stream it is
 * replaying.  Same sequence as the M11 code it replaces: gen, bind, load,
 * error position; native count and the under-native-limits query; on any
 * refusal bind 0 and delete. */
typedef void (*fn_genprog_t)(GLsizei, GLuint*);
typedef void (*fn_delprog_t)(GLsizei, const GLuint*);
typedef void (*fn_progstr_t)(GLenum, GLenum, GLsizei, const void*);
typedef void (*fn_getprogiv_t)(GLenum, GLenum, GLint*);
static fn_genprog_t x_GenProgramsARB;
static fn_delprog_t x_DeleteProgramsARB;
static fn_progstr_t x_ProgramStringARB;
static fn_getprogiv_t x_GetProgramivARB;
#define RT_VP 0x8620u          /* GL_VERTEX_PROGRAM_ARB */
#define RT_VP_ASCII 0x8875u    /* GL_PROGRAM_FORMAT_ASCII_ARB */
#define RT_VP_ERRPOS 0x864Bu   /* GL_PROGRAM_ERROR_POSITION_ARB */
#define RT_VP_ERRSTR 0x8874u   /* GL_PROGRAM_ERROR_STRING_ARB */
#define RT_VP_NATIVE 0x88A2u   /* GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB */
#define RT_VP_UNDER 0x88B6u    /* GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB */
static void compile_fn(void* args) {
    RtCompile* c = *(RtCompile**)args;
    GLuint id = 0;
    GLint errpos = -1;
    if (!x_GenProgramsARB) {
        x_GenProgramsARB = (fn_genprog_t)SDL_GL_GetProcAddress("glGenProgramsARB");
        x_DeleteProgramsARB = (fn_delprog_t)SDL_GL_GetProcAddress("glDeleteProgramsARB");
        x_ProgramStringARB = (fn_progstr_t)SDL_GL_GetProcAddress("glProgramStringARB");
        x_GetProgramivARB = (fn_getprogiv_t)SDL_GL_GetProcAddress("glGetProgramivARB");
        if (!x_BindProgramARB) {
            x_BindProgramARB = (fn_bindprog_t)SDL_GL_GetProcAddress("glBindProgramARB");
        }
    }
    /* M35: the same sequence for a GL_TEXT_FRAGMENT_SHADER_ATI program (gx_tfs.c);
     * that target has no native-limit query (the text is either accepted or
     * refused at a character), so it is accepted on errpos alone. */
    GLenum tgt = c->target ? (GLenum)c->target : (GLenum)RT_VP;
    c->id = 0;
    c->errpos = -1;
    c->native = 0;
    c->under_native = 0;
    c->msg[0] = '\0';
    x_GenProgramsARB(1, &id);
    x_BindProgramARB(tgt, id);
    x_ProgramStringARB(tgt, RT_VP_ASCII, (GLsizei)c->len, c->text);
    glGetIntegerv(RT_VP_ERRPOS, &errpos);
    c->errpos = (int)errpos;
    if (errpos != -1) {
        const char* m = (const char*)glGetString(RT_VP_ERRSTR);
        if (m) {
            strncpy(c->msg, m, sizeof(c->msg) - 1);
            c->msg[sizeof(c->msg) - 1] = '\0';
        }
        x_BindProgramARB(tgt, 0);
        x_DeleteProgramsARB(1, &id);
        return;
    }
    if (tgt != RT_VP) {
        c->under_native = 1;
        c->id = id; /* bound */
        return;
    }
    {
        GLint nat = 0, under = 0;
        x_GetProgramivARB(RT_VP, RT_VP_NATIVE, &nat);
        x_GetProgramivARB(RT_VP, RT_VP_UNDER, &under);
        c->native = (int)nat;
        c->under_native = (int)under;
    }
    if (!c->under_native) {
        x_BindProgramARB(RT_VP, 0);
        x_DeleteProgramsARB(1, &id);
        return;
    }
    c->id = id; /* and it stays bound, as before */
}
void rt_compile_vprog(RtCompile* c) {
    RtCompile* p = c;
    rt_call(compile_fn, &p, sizeof(p), 1);
}

void rt_present(unsigned frame) {
    if (!rt_recording) {
        SDL_GL_SwapWindow(win);
        return;
    }
    { REC(OP_PRESENT, A_present); a->frame = frame; a->t_rec = now(); }
    st_frames++;
    if (st_frame_bytes > st_frame_bytes_peak) {
        st_frame_bytes_peak = st_frame_bytes;
    }
    st_frame_bytes = 0;
    st_frame_records = 0;
    done();
    if (mode == 2) {
        rt_join("frame end (mode 2)");
    }
}

void rt_frame_end(void) {
    if (mode == 2) {
        rt_join("frame end (mode 2)");
    }
}

/* the gate: is the reader drained, or does it drain within max_s */
int rt_gate(double max_s) {
    double t0, d;
    int spins = 0;
    if (!rt_recording || mode < 2) {
        return 1;
    }
    publish();
    if ((s32)(rd - wr) >= 0) {
        st_gate_ok++;
        return 1;
    }
    t0 = now();
    for (;;) {
        RT_ACQ_REL();
        if ((s32)(rd - wr) >= 0) {
            d = now() - t0;
            st_gate_waited++;
            st_gate_wait_s += d;
            if (d > st_gate_wait_max) {
                st_gate_wait_max = d;
            }
            return 1;
        }
        if (++spins >= 64) {
            spins = 0;
            d = now() - t0;
            if (d >= max_s) {
                st_gate_busy++;
                return 0;
            }
            sched_yield();
        }
    }
}

/* the writer is about to reuse chunk c: issued, then finished (above) */
void rt_ring_enter(int chunk) {
    double t0;
    if (!rt_recording || mode < 2 || chunk < 0 || chunk >= RING_CHUNKS) {
        return;
    }
    if ((s32)(rd - ring_left_pos[chunk]) < 0) {
        st_ring_waits++;
        wait_pos(ring_left_pos[chunk], "ring", &st_ring_wait_s, &st_ring_wait_max);
    }
    if (ring_gpu_epoch[chunk] != ring_set_epoch[chunk]) {
        int spins = 0;
        t0 = now();
        st_ring_gpu_waits++;
        while (ring_gpu_epoch[chunk] != ring_set_epoch[chunk]) {
            RT_ACQ_REL();
            if (++spins >= 64) {
                spins = 0;
                sched_yield();
            }
        }
        RT_ACQ_REL();
        {
            double d = now() - t0;
            st_ring_wait_s += d;
            if (d > st_ring_wait_max) {
                st_ring_wait_max = d;
            }
        }
    }
}

/* the reader: test the fences it has set and publish the finished ones */
static void reader_test_fences(void) {
    int c;
    for (c = 0; c < RING_CHUNKS; c++) {
        if (ring_fence[c].pending) {
            if (x_TestFenceAPPLE(ring_fence[c].f)) {
                ring_fence[c].pending = 0;
                RT_ACQ_REL();
                ring_gpu_epoch[c] = ring_fence[c].epoch;
            }
        }
    }
}

/* ---- the reader ---------------------------------------------------------------- */

static double t_class0;
static void class_begin(void) {
    if (port_opt.rtsplit) {
        t_class0 = now();
    }
}
static void class_end(int c) {
    if (port_opt.rtsplit) {
        st_class_s[c] += now() - t_class0;
    }
}

/* replay one record; returns its class for the split */
static void replay_one(const Hdr* h) {
    const void* p = h + 1;
    int cls = RC_STATE;
    class_begin();
    switch (h->op) {
        case OP_NOP: case OP_WRAP: cls = RC_OTHER; break;
        case OP_ENABLE: glEnable(((const A1e*)p)->a); break;
        case OP_DISABLE: glDisable(((const A1e*)p)->a); break;
        case OP_ENABLE_CS: glEnableClientState(((const A1e*)p)->a); break;
        case OP_DISABLE_CS: glDisableClientState(((const A1e*)p)->a); break;
        case OP_ACTIVE_TEX: glActiveTexture(((const A1e*)p)->a); break;
        case OP_CLIENT_ACTIVE_TEX: glClientActiveTexture(((const A1e*)p)->a); break;
        case OP_BIND_TEX: { const A_eu* a = p; glBindTexture(a->a, a->b); cls = RC_TEX; break; }
        case OP_TEXPARAM_I: { const A_eei* a = p; glTexParameteri(a->a, a->b, a->v); cls = RC_TEX; break; }
        case OP_TEXPARAM_F: { const A_eef* a = p; glTexParameterf(a->a, a->b, a->v); cls = RC_TEX; break; }
        case OP_TEXENV_I: { const A_eei* a = p; glTexEnvi(a->a, a->b, a->v); break; }
        case OP_TEXENV_F: { const A_eef* a = p; glTexEnvf(a->a, a->b, a->v); break; }
        case OP_TEXENV_FV: { const A_eef4* a = p; glTexEnvfv(a->a, a->b, a->v); break; }
        case OP_MATRIX_MODE: glMatrixMode(((const A1e*)p)->a); break;
        case OP_LOAD_IDENTITY: glLoadIdentity(); break;
        case OP_LOAD_MATRIX: glLoadMatrixf(((const A_m16*)p)->m); break;
        case OP_PUSH: glPushMatrix(); break;
        case OP_POP: glPopMatrix(); break;
        case OP_ORTHO: { const A_d6* a = p; glOrtho(a->v[0], a->v[1], a->v[2], a->v[3], a->v[4], a->v[5]); break; }
        case OP_DEPTH_MASK: glDepthMask(((const A_b4*)p)->r); break;
        case OP_DEPTH_FUNC: glDepthFunc(((const A1e*)p)->a); break;
        case OP_DEPTH_RANGE: { const A_d2* a = p; glDepthRange(a->a, a->b); break; }
        case OP_COLOR_MASK: { const A_b4* a = p; glColorMask(a->r, a->g, a->b, a->a); break; }
        case OP_CULL: glCullFace(((const A1e*)p)->a); break;
        case OP_FRONT: glFrontFace(((const A1e*)p)->a); break;
        case OP_SHADE: glShadeModel(((const A1e*)p)->a); break;
        case OP_POLYMODE: { const A_ee* a = p; glPolygonMode(a->a, a->b); break; }
        case OP_HINT: { const A_ee* a = p; glHint(a->a, a->b); break; }
        case OP_BLEND_FUNC: { const A_ee* a = p; glBlendFunc(a->a, a->b); break; }
        case OP_BLEND_EQ: glBlendEquation(((const A1e*)p)->a); break;
        case OP_ALPHA_FUNC: { const A_alpha* a = p; glAlphaFunc(a->f, a->ref); break; }
        case OP_FOG_I: { const A_ei* a = p; glFogi(a->a, a->v); break; }
        case OP_FOG_F: { const A_ef* a = p; glFogf(a->a, a->v); break; }
        case OP_LINE_WIDTH: glLineWidth(((const A_ef*)p)->v); break;
        case OP_FOG_FV: { const A_ef4* a = p; glFogfv(a->a, a->v); break; }
        case OP_LIGHT_F: { const A_eef* a = p; glLightf(a->a, a->b, a->v); break; }
        case OP_LIGHT_FV: { const A_eef4* a = p; glLightfv(a->a, a->b, a->v); break; }
        case OP_LIGHTMODEL_I: { const A_ei* a = p; glLightModeli(a->a, a->v); break; }
        case OP_LIGHTMODEL_FV: { const A_ef4* a = p; glLightModelfv(a->a, a->v); break; }
        case OP_MATERIAL_FV: { const A_eef4* a = p; glMaterialfv(a->a, a->b, a->v); break; }
        case OP_COLOR_MATERIAL: { const A_ee* a = p; glColorMaterial(a->a, a->b); break; }
        case OP_CLEAR_COLOR: { const A_f4* a = p; glClearColor(a->v[0], a->v[1], a->v[2], a->v[3]); break; }
        case OP_CLEAR_DEPTH: glClearDepth(((const A_d*)p)->d); break;
        case OP_CLEAR: glClear(((const A_bits*)p)->m); cls = RC_DRAW; break;
        case OP_VIEWPORT: { const A_rect* a = p; glViewport(a->x, a->y, a->w, a->h); break; }
        case OP_SCISSOR: { const A_rect* a = p; glScissor(a->x, a->y, a->w, a->h); break; }
        case OP_PIXELSTORE: { const A_ei* a = p; glPixelStorei(a->a, a->v); break; }
        case OP_READ_BUFFER: glReadBuffer(((const A1e*)p)->a); break;
        case OP_FINISH: glFinish(); cls = RC_DRAW; break;
        case OP_BEGIN: glBegin(((const A1e*)p)->a); cls = RC_DRAW; break;
        case OP_END: glEnd(); cls = RC_DRAW; break;
        case OP_TEXCOORD2F: { const A_f2* a = p; glTexCoord2f(a->x, a->y); cls = RC_DRAW; break; }
        case OP_VERTEX2F: { const A_f2* a = p; glVertex2f(a->x, a->y); cls = RC_DRAW; break; }
        case OP_VERTEX_PTR: { const A_ptr* a = p; glVertexPointer(a->size, a->type, a->stride, a->p); cls = RC_DRAW; break; }
        case OP_COLOR_PTR: { const A_ptr* a = p; glColorPointer(a->size, a->type, a->stride, a->p); cls = RC_DRAW; break; }
        case OP_NORMAL_PTR: { const A_ptr2* a = p; glNormalPointer(a->type, a->stride, a->p); cls = RC_DRAW; break; }
        case OP_TEXCOORD_PTR: { const A_ptr* a = p; glTexCoordPointer(a->size, a->type, a->stride, a->p); cls = RC_DRAW; break; }
        case OP_FOGCOORD_PTR: { const A_ptr2* a = p; if (x_FogCoordPointerEXT) { x_FogCoordPointerEXT(a->type, a->stride, a->p); } cls = RC_DRAW; break; }
        case OP_DRAW_ARRAYS: { const A_draw* a = p; glDrawArrays(a->mode, a->first, a->count); cls = RC_DRAW; break; }
        case OP_MULTI_DRAW: {
            const A_multi* a = p;
            const GLint* f = (const GLint*)(a + 1);
            const GLsizei* c = (const GLsizei*)(f + a->n);
            x_MultiDrawArraysEXT(a->mode, f, c, a->n);
            cls = RC_DRAW;
            break;
        }
        case OP_DRAW_RANGE: { const A_range* a = p; glDrawRangeElements(a->mode, a->lo, a->hi, a->n, a->type, a + 1); cls = RC_DRAW; break; }
        case OP_DELETE_TEX: { const A_del* a = p; glDeleteTextures(a->n, (const GLuint*)(a + 1)); cls = RC_TEX; break; }
        case OP_TEXIMAGE: {
            const A_teximg* a = p;
            glTexImage2D(a->target, a->level, a->ifmt, a->w, a->h, a->border, a->fmt, a->type, a->px);
            if (a->owned) {
                free(a->px);
                st_owned_frees++;
            }
            cls = RC_TEX;
            break;
        }
        case OP_TEXSUBIMAGE: {
            const A_texsub* a = p;
            glTexSubImage2D(a->target, a->level, a->xo, a->yo, a->w, a->h, a->fmt, a->type, a->px);
            if (a->done) {
                __sync_synchronize();
                *a->done = 1; /* the owner's pool may reuse the pixels now */
            } else {
                free(a->px);
                st_owned_frees++;
            }
            cls = RC_TEX;
            break;
        }
        case OP_COPY_TEX_SUB: { const A_copysub* a = p; glCopyTexSubImage2D(a->target, a->level, a->xo, a->yo, a->x, a->y, a->w, a->h); cls = RC_TEX; break; }
        case OP_READ_PIXELS: { const A_readpx* a = p; glReadPixels(a->x, a->y, a->w, a->h, a->fmt, a->type, a->out); cls = RC_OTHER; break; }
        case OP_GET_TEX_IMAGE: { const A_gettex* a = p; glGetTexImage(a->target, a->level, a->fmt, a->type, a->out); cls = RC_OTHER; break; }
        case OP_GET_ERROR: { const A_geterr* a = p; *a->out = glGetError(); cls = RC_OTHER; break; }
        case OP_FLUSH_VAR: { const A_flushvar* a = p; x_FlushVertexArrayRangeAPPLE(a->len, a->p); cls = RC_DRAW; break; }
        case OP_BIND_BUFFER: { const A_bindbuf* a = p; x_BindBufferARB(RT_ARRAY_BUFFER_ARB, a->id); cls = RC_STATE; break; }
        case OP_BUFFER_SUB: { const A_bufsub* a = p; x_BufferSubDataARB(RT_ARRAY_BUFFER_ARB, a->off, a->n, a->p); cls = RC_DRAW; break; }
        case OP_SET_FENCE: {
            const A_fence* a = p;
            x_SetFenceAPPLE(a->f);
            if (a->chunk >= 0 && a->chunk < RING_CHUNKS && mode >= 2) {
                /* a fence set on a chunk still pending from the last lap
                 * cannot happen: the writer waited for that epoch on entry */
                ring_fence[a->chunk].f = a->f;
                ring_fence[a->chunk].epoch = a->epoch;
                ring_fence[a->chunk].pending = 1;
            }
            cls = RC_DRAW;
            break;
        }
        case OP_WAIT_FENCE: {
            const A_fence* a = p;
            st_fence_waits++;
            if (!x_TestFenceAPPLE(a->f)) {
                st_fence_blocked++;
                x_FinishFenceAPPLE(a->f);
            }
            cls = RC_DRAW;
            break;
        }
        case OP_BIND_PROG: { const A_eu* a = p; x_BindProgramARB(a->a, a->b); cls = RC_DRAW; break; }
        case OP_ENV4: { const A_env4* a = p; x_ProgramEnvParameter4fvARB(a->target, a->idx, a->v); cls = RC_DRAW; break; }
        case OP_ENVN: { const A_envn* a = p; x_ProgramEnvParameters4fvEXT(a->target, a->idx, a->n, (const GLfloat*)(a + 1)); cls = RC_DRAW; break; }
        case OP_CALL: { const A_call* a = p; a->fn((void*)(a + 1)); cls = RC_OTHER; break; }
        case OP_PRESENT: {
            const A_present* a = p;
            double t = now();
            double tail = t - a->t_rec;
            SDL_GL_SwapWindow(win);
            st_tail_s += tail;
            if (tail > st_tail_max) {
                st_tail_max = tail;
            }
            cls = RC_PRESENT;
            break;
        }
        case OP_DECODE: {
            /* the decode cursor normally got here first; if not (it is at
             * most `rd` itself), the run is decoded now, in stream order,
             * before the draw that reads it */
            A_decode* a = (A_decode*)h + 0, *d = (A_decode*)(void*)(h + 1);
            (void)a;
            if (!d->done) {
                double t0 = now();
                d->verts = gx_decode_job(&d->job);
                d->done = 1;
                st_dec_late++;
                st_dec_verts += d->verts;
                t0 = now() - t0;
                st_dec_s += t0;
                st_frame_dec_s += t0;
            }
            cls = RC_OTHER;
            break;
        }
        default:
            port_fatal("render thread: unknown record %u (%s) at %u", h->op,
                       h->op < OP_N ? op_name[h->op] : "?", rd);
    }
    class_end(cls);
}

/* M29: the decode cursor.  From max(dec, rd) to `upto` (a published
 * position): execute every OP_DECODE not yet done, skip everything else.
 * Runs before each replayed record with a fresh wr_pub, so a run is
 * decoded within a record of its emission and the game thread's join at
 * the retrace (rt_decode_join) waits for one primitive at most. */
static void decode_ahead(u32 upto) {
    int any = 0;
    if ((s32)(dec - rd) < 0) {
        dec = rd;
    }
    while ((s32)(upto - dec) > 0) {
        const Hdr* h = (const Hdr*)(buf + (dec & RT_MASK));
        u32 len = h->len;
        if (h->op == OP_DECODE) {
            A_decode* d = (A_decode*)(void*)(h + 1);
            if (!d->done) {
                double t0 = now();
                d->verts = gx_decode_job(&d->job);
                d->done = 1;
                st_dec_ahead++;
                st_dec_verts += d->verts;
                t0 = now() - t0;
                st_dec_s += t0;
                st_frame_dec_s += t0;
                any = 1;
            }
        }
        dec += len;
    }
    if (any || (s32)(dec - dec_pub) > 0) {
        RT_ACQ_REL(); /* release: the ring bytes before the position */
        dec_pub = dec;
        RT_FULL(); /* the store above before the load below (Dekker) */
        if (writer_waiting) {
            pthread_mutex_lock(&mu);
            pthread_cond_broadcast(&cv_done);
            pthread_mutex_unlock(&mu);
        }
    }
}

/* replay [rd, to) -- on the render thread, or on the game thread inline */
static unsigned replayed_since_test, handshakes;
static void replay_upto(u32 to) {
    double t0;
    if (mode == 1) {
        /* the inline twin: one thread, so no barriers, no handshake, and the
         * per-frame timing from the present records alone */
        while ((s32)(to - rd) > 0) {
            const Hdr* h = (const Hdr*)(buf + (rd & RT_MASK));
            u32 len = h->len;
            replay_one(h);
            rd += len;
        }
        dec = dec_pub = rd;
        return;
    }
    t0 = now();
    RT_ACQ_REL(); /* acquire: the records behind `to` after the position itself */
    while ((s32)(to - rd) > 0) {
        const Hdr* h;
        u32 len;
        int present;
        if (decmode) {
            /* the decode first, over everything published by now */
            u32 w;
            RT_ACQ_REL();
            w = wr_pub;
            if ((s32)(w - dec) > 0) {
                double td = now();
                decode_ahead(w);
                td = now() - td;
                t0 += td; /* the decode is timed on its own, not as replay */
            }
        }
        h = (const Hdr*)(buf + (rd & RT_MASK));
        len = h->len;
        present = h->op == OP_PRESENT;
        replay_one(h);
        if (present || ((++replayed_since_test & 255u) == 0 && mode >= 2)) {
            reader_test_fences();
        }
        if (present) {
            double t = now();
            st_frame_replay_s += t - t0;
            st_replay_s += t - t0;
            t0 = t;
            st_last_frame_ms = st_frame_replay_s * 1000.0;
            st_frame_ms_sum += st_last_frame_ms;
            st_frame_ms_n++;
            if (st_last_frame_ms > st_frame_ms_max) {
                st_frame_ms_max = st_last_frame_ms;
            }
            st_frame_replay_s = 0.0;
            st_last_dec_ms = st_frame_dec_s * 1000.0;
            st_dec_ms_sum += st_last_dec_ms;
            if (st_last_dec_ms > st_dec_ms_max) {
                st_dec_ms_max = st_last_dec_ms;
            }
            st_frame_dec_s = 0.0;
        }
        RT_ACQ_REL(); /* release: a read-back's pixels before the position */
        rd += len;
        if ((++handshakes & 15u) == 0) {
            RT_FULL(); /* the store above before the load below (Dekker) */
            if (writer_waiting) {
                pthread_mutex_lock(&mu);
                pthread_cond_broadcast(&cv_done);
                pthread_mutex_unlock(&mu);
            }
        }
    }
    /* the drain is over: a writer that went to sleep on a position inside it
     * must be told now, whatever the count above says */
    RT_FULL();
    if (writer_waiting) {
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
    {
        double d = now() - t0;
        st_replay_s += d;
        st_frame_replay_s += d;
    }
}

static void* thread_main(void* arg) {
    (void)arg;
    if (SDL_GL_MakeCurrent(win, ctx) != 0) {
        port_log("port> render thread: SDL_GL_MakeCurrent failed (%s)\n", SDL_GetError());
    }
    for (;;) {
        u32 w;
        int spins = 0;
        for (;;) {
            RT_ACQ_REL();
            w = wr_pub;
            if ((s32)(w - rd) > 0 || quit) {
                break;
            }
            if (++spins < 4000) {
                if ((spins & 1023) == 0) {
                    reader_test_fences(); /* a fence finishing while idle */
                }
                continue; /* a short spin covers the gaps inside a drawn frame; then the
                           * core is the mixer worker's until the next publish */
            }
            reader_test_fences();
            pthread_mutex_lock(&mu);
            reader_asleep = 1;
            RT_FULL(); /* the store above before the load below (Dekker) */
            w = wr_pub;
            if ((s32)(w - rd) <= 0 && !quit) {
                int c, pending = 0;
                for (c = 0; c < RING_CHUNKS; c++) {
                    pending |= ring_fence[c].pending;
                }
                st_reader_sleeps++;
                if (pending) {
                    /* a writer may be waiting on one of these: doze, re-test */
                    struct timespec ts;
                    struct timeval tv;
                    gettimeofday(&tv, NULL); /* the condvar's clock is the epoch's, not port_now's */
                    ts.tv_sec = tv.tv_sec;
                    ts.tv_nsec = (tv.tv_usec + 500) * 1000L;
                    if (ts.tv_nsec >= 1000000000L) {
                        ts.tv_sec++;
                        ts.tv_nsec -= 1000000000L;
                    }
                    pthread_cond_timedwait(&cv_work, &mu, &ts);
                } else {
                    pthread_cond_wait(&cv_work, &mu);
                }
            }
            reader_asleep = 0;
            pthread_mutex_unlock(&mu);
            spins = 0;
        }
        if ((s32)(w - rd) <= 0 && quit) {
            break;
        }
        replay_upto(w);
    }
    glFinish();
    SDL_GL_MakeCurrent(win, NULL);
    return NULL;
}

/* ---- lifecycle -------------------------------------------------------------------- */

int rt_mode(void) { return mode; }
int rt_decode_mode(void) { return decmode; } /* M32: for --defaults */
int rt_on(void) { return mode >= 2 && thread_up; }

void rt_start(void* sdl_window, void* sdl_glcontext) {
    const char* ext;
    win = (SDL_Window*)sdl_window;
    ctx = (SDL_GLContext)sdl_glcontext;
    mode = port_opt.renderthread;
    if (mode < 0) {
        mode = port_threads_on() ? 3 : 1;
    }
    if (mode >= 2 && !port_threads_on()) {
        port_log("port> render thread: --renderthread %d asks for a thread on one core; "
                 "the inline replay instead\n", mode);
        mode = 1;
    }
    if (!win || !ctx) {
        mode = 0;
    }
    decmode = port_opt.rtdecode;
    if (decmode < 0) {
        /* M33: auto with an overlapped render thread (PLAN.md 48.2: the
         * character select 20.0 -> 23.4 fps, the board and the title held) */
        decmode = mode >= 3 ? 3 : mode >= 2 ? 2 : 0;
    }
    if (mode == 0) {
        decmode = 0;
    }
    /* the extension pointers the twins call in every mode */
    ext = (win && ctx) ? (const char*)glGetString(GL_EXTENSIONS) : NULL;
    if (ext) {
        if (strstr(ext, "GL_EXT_multi_draw_arrays")) {
            x_MultiDrawArraysEXT = (fn_multidraw_t)SDL_GL_GetProcAddress("glMultiDrawArraysEXT");
        }
        if (strstr(ext, "GL_APPLE_vertex_array_range") && strstr(ext, "GL_APPLE_fence")) {
            x_FlushVertexArrayRangeAPPLE = (fn_range_t)SDL_GL_GetProcAddress("glFlushVertexArrayRangeAPPLE");
            x_SetFenceAPPLE = (fn_fence_t)SDL_GL_GetProcAddress("glSetFenceAPPLE");
            x_FinishFenceAPPLE = (fn_fence_t)SDL_GL_GetProcAddress("glFinishFenceAPPLE");
            x_TestFenceAPPLE = (fn_fence_test_t)SDL_GL_GetProcAddress("glTestFenceAPPLE");
        }
        if (strstr(ext, "GL_ARB_vertex_buffer_object")) {
            x_BindBufferARB = (fn_bindbuf_t)SDL_GL_GetProcAddress("glBindBufferARB");
            x_BufferSubDataARB = (fn_bufsub_t)SDL_GL_GetProcAddress("glBufferSubDataARB");
        }
        if (port_opt.vcache_vbo) {
            port_log("port> vcache: buffer object entry points: bind %p, subdata %p\n",
                     (void*)x_BindBufferARB, (void*)x_BufferSubDataARB);
        }
        if (strstr(ext, "GL_ARB_vertex_program")) {
            x_BindProgramARB = (fn_bindprog_t)SDL_GL_GetProcAddress("glBindProgramARB");
            x_ProgramEnvParameter4fvARB = (fn_env4_t)SDL_GL_GetProcAddress("glProgramEnvParameter4fvARB");
        }
        if (strstr(ext, "GL_EXT_gpu_program_parameters")) {
            x_ProgramEnvParameters4fvEXT = (fn_envn_t)SDL_GL_GetProcAddress("glProgramEnvParameters4fvEXT");
        }
        if (strstr(ext, "GL_EXT_fog_coord")) {
            x_FogCoordPointerEXT = (fn_fogptr_t)SDL_GL_GetProcAddress("glFogCoordPointerEXT");
        }
    }
    if (mode == 0) {
        port_log("port> render thread: off (the direct GL path)\n");
        return;
    }
    buf = (u8*)valloc(RT_BYTES);
    if (!buf) {
        port_fatal("render thread: cannot allocate the %u KB stream", RT_BYTES / 1024);
    }
    memset(buf, 0, RT_BYTES);
    /* everything GL that happened so far was the main thread's; from here
     * the stream owns it */
    glFinish();
    if (mode >= 2) {
        SDL_GL_MakeCurrent(win, NULL);
        if (pthread_create(&thread, NULL, thread_main, NULL) != 0) {
            port_log("port> render thread: pthread_create failed; the inline replay instead\n");
            SDL_GL_MakeCurrent(win, ctx);
            mode = 1;
        } else {
            thread_up = 1;
        }
    }
    rt_recording = 1;
    port_log("port> render thread: %s (--renderthread %d); stream %u KB\n",
             mode == 1 ? "the inline replay on the game thread (the single-core twin)"
             : mode == 2 ? "on, joined at every frame's end (no overlap)"
                         : "on, overlapped (the join at the gate)",
             mode, RT_BYTES / 1024);
    if (decmode == 3 && mode < 3) {
        decmode = mode >= 2 ? 2 : 0; /* auto balances against an overlapped reader only */
    }
    port_log("port> render thread: the display-list decode %s (--rtdecode %d)\n",
             decmode == 0 ? "on the game thread" :
             decmode == 1 ? "as records, the game thread joined after each (stage 1)" :
             decmode == 2 ? "as records, the game thread joined at the retrace (stage 2)" :
                            "split per drawn frame between the two threads (auto, M33)",
             decmode);
}

void rt_stop(void) {
    if (!rt_recording) {
        return;
    }
    if (mode == 1) {
        replay_upto(wr);
    } else if (thread_up) {
        rt_join("shutdown");
        pthread_mutex_lock(&mu);
        quit = 1;
        pthread_cond_broadcast(&cv_work);
        pthread_mutex_unlock(&mu);
        pthread_join(thread, NULL);
        thread_up = 0;
        SDL_GL_MakeCurrent(win, ctx);
    }
    rt_recording = 0;
}

void rt_status(char* out, size_t n) {
    if (!rt_recording || mode < 2) {
        out[0] = '\0';
        return;
    }
    if (decmode == 3) {
        snprintf(out, n, "  rt %.1f ms dec %.1f+%.1f", st_last_frame_ms, st_last_dec_ms,
                 au_fr_gdec_s * 1000.0);
    } else if (decmode) {
        snprintf(out, n, "  rt %.1f ms dec %.1f", st_last_frame_ms, st_last_dec_ms);
    } else {
        snprintf(out, n, "  rt %.1f ms", st_last_frame_ms);
    }
}
double rt_last_dec_ms(void) { return st_last_dec_ms; }

void rt_report(void) {
    int i;
    if (mode == 0) {
        return;
    }
    port_log("\nport> render thread: mode %d (%s)\n", mode,
             mode == 1 ? "inline replay" : mode == 2 ? "thread, joined per frame" : "thread, overlapped");
    port_log("  stream   %lu records, %lu KB (%.0f B/record); %lu frames presented, peak %u KB a "
             "frame; %lu KB of payload copies; %lu names handed out; %lu owned uploads freed\n",
             st_records, st_bytes / 1024, st_records ? (double)st_bytes / (double)st_records : 0.0,
             st_frames, st_frame_bytes_peak / 1024, st_stash_bytes / 1024, st_names, st_owned_frees);
    port_log("  replay   %.0f ms in all; per presented frame mean %.2f ms, worst %.1f ms (%lu frames); "
             "present tail (swap executed - swap recorded) mean %.1f ms, worst %.1f ms\n",
             st_replay_s * 1000.0, st_frame_ms_n ? st_frame_ms_sum / (double)st_frame_ms_n : 0.0,
             st_frame_ms_max, st_frame_ms_n,
             st_frames ? st_tail_s / (double)st_frames * 1000.0 : 0.0, st_tail_max * 1000.0);
    if (decmode) {
        port_log("  decode   %lu runs recorded, %lu decoded ahead by the decode cursor, %lu by the "
                 "replay (late); %lu vertices; %.0f ms in all, per presented frame mean %.2f ms, "
                 "worst %.1f ms (--rtdecode %d)\n",
                 st_dec_records, st_dec_ahead, st_dec_late, st_dec_verts, st_dec_s * 1000.0,
                 st_frame_ms_n ? st_dec_ms_sum / (double)st_frame_ms_n : 0.0, st_dec_ms_max,
                 decmode);
    }
    if (decmode == 3) {
        port_log("  auto     %lu drawn frames planned, %lu split (share mean %.2f, max %.2f); "
                 "%lu vertices decoded on the game thread (%.0f ms) vs %lu on the render thread; "
                 "last plan: game %.1f + consumed %.1f vs replay %.1f + decode %.1f -> %.1f ms "
                 "moved (--rtdecode auto, fit %.0f ms, max share %.2f)\n",
                 au_frames, au_frames_split, au_frames_split ? au_share_sum / (double)au_frames_split : 0.0,
                 au_share_max, au_gverts, au_gdec_s * 1000.0, au_rverts, au_last_gd, au_last_cc,
                 au_last_r, au_last_d, au_last_want, port_opt.rtauto_fit_ms, port_opt.rtauto_max);
    }
    if (port_opt.rtsplit) {
        port_log("  split    ");
        for (i = 0; i < RC_N; i++) {
            port_log("%s %.0f ms%s", class_name[i], st_class_s[i] * 1000.0, i + 1 < RC_N ? ", " : "\n");
        }
    }
    if (mode >= 2) {
        port_log("  gate     %lu drained, %lu waited (%.0f ms, worst %.1f ms), %lu busy (the frame "
                 "was consumed instead)\n",
                 st_gate_ok, st_gate_waited, st_gate_wait_s * 1000.0, st_gate_wait_max * 1000.0,
                 st_gate_busy);
        port_log("  ring     %lu reuse waits for the issue, %lu for the GPU (%.0f ms, worst %.1f ms); "
                 "stream full %lu times (%.0f ms); reader slept %lu times, woken %lu\n",
                 st_ring_waits, st_ring_gpu_waits, st_ring_wait_s * 1000.0, st_ring_wait_max * 1000.0, st_full_waits,
                 st_full_wait_s * 1000.0, st_reader_sleeps, st_writer_wakes);
        for (i = 0; i < njoins; i++) {
            port_log("  join     %-24s %lu (%.0f ms, worst %.1f ms)\n", joins[i].why, joins[i].n,
                     joins[i].s * 1000.0, joins[i].max * 1000.0);
        }
    }
    if (mode >= 2) {
        port_log("  fences   the ring's chunks by epoch: the writer waited %lu time(s) for the GPU "
                 "(the `ring' line above); the reader tests pending fences after every present, "
                 "every 256 records and while idle\n", st_ring_gpu_waits);
    } else {
        port_log("  fences   %lu waits (%lu blocked) on the replaying side\n", st_fence_waits,
                 st_fence_blocked);
    }
}

double rt_last_frame_ms(void) { return st_last_frame_ms; }

unsigned long rt_records_written(void) { return st_records; }

static void finish_fn(void* a) {
    (void)a;
    glFinish();
}
void rt_finish_join(const char* why) {
    (void)why;
    rt_call(finish_fn, NULL, 0, 1);
}

#else /* PORT_NO_SDL */

unsigned long rt_records_written(void) { return 0; }
void rt_finish_join(const char* why) { (void)why; }

void rt_start(void* w, void* c) { (void)w; (void)c; }
void rt_stop(void) {}
int rt_on(void) { return 0; }
int rt_mode(void) { return 0; }
int rt_decode_mode(void) { return 0; }
void rt_frame_end(void) {}
void rt_report(void) {}
void rt_status(char* buf, size_t n) { (void)n; buf[0] = '\0'; }
void rt_join(const char* why) { (void)why; }
int rt_gate(double s) { (void)s; return 1; }
int rt_decode_want(unsigned v) { (void)v; return 0; }
void rt_decode_here(unsigned v, double s) { (void)v; (void)s; }
void rt_decode_there(unsigned v) { (void)v; }
void rt_auto_frame_end(int d, double s) { (void)d; (void)s; }
void rt_auto_frame_begin(void) {}
double rt_auto_last_share(void) { return 0.0; }
int rt_vcache_inputs(double* a, double* b, double* c, double* d) { (void)a; (void)b; (void)c; (void)d; return 0; }
double rt_auto_frame_gdec_ms(void) { return 0.0; }
void rt_ring_enter(int c) { (void)c; }
void rt_call(void (*fn)(void*), const void* args, size_t n, int sync) {
    void* copy = malloc(n ? n : 1);
    (void)sync;
    if (copy) { memcpy(copy, args, n); fn(copy); free(copy); }
}
const void* rt_stash(const void* p, size_t n) { (void)n; return p; }
int rt_decode_on(void) { return 0; }
unsigned rt_pos(void) { return 0; }
void rt_decode_join(const char* why) { (void)why; }
void rt_decode_join_pos(unsigned pos, const char* why) { (void)pos; (void)why; }
double rt_last_dec_ms(void) { return 0.0; }
void port_vtx_rewrite(const char* who) { (void)who; gx_vc_epoch++; }
void rt_decode_record(const GxDecJob* j) { (void)j; }

#endif
