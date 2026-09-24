/* M42 (PLAN.md 57.4): does the Radeon 9000's Leopard driver run a vertex
 * program's relative addressing (ARL) on the card?
 *
 * §33.2 measured the matrix palette inside the game and found it slow; this
 * asks the driver the question alone, in the shape a skinned character
 * would take: B objects of V vertices, each with its own 3x4 matrix, drawn
 *
 *   split   one glDrawArrays per object, the matrix loaded as three env
 *           parameters before each (what the port does today)
 *   arl     one glDrawArrays for all B objects, every vertex naming its
 *           matrix in an attribute, the program reading pal[A0.x] (ARL)
 *   flat    one glDrawArrays, one matrix for all (the floor: no ARL, no
 *           per-object state) -- the same vertex count and program length
 *
 * Each arm draws F frames into a pbuffer with glFinish per frame; the times
 * are wall (mach_absolute_time) per frame.  A card that runs ARL natively
 * puts arl near flat; a driver that runs the program on the CPU when it
 * sees ARL puts arl far above both, growing with the vertex count.
 *
 *   vp_arl_test [B objects=32] [V vertices=30] [F frames=200] [P palette params=96]
 */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static GLuint load_prog(const char* src) {
    GLuint p; GLint err, native = 0, ninst = 0;
    glGenProgramsARB(1, &p);
    glBindProgramARB(GL_VERTEX_PROGRAM_ARB, p);
    glProgramStringARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(src), src);
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &err);
    if (err != -1) {
        printf("program error at %d: %s\n", (int)err, glGetString(GL_PROGRAM_ERROR_STRING_ARB));
        exit(1);
    }
    glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &native);
    glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB, &ninst);
    printf("  program %u: under native limits %s, %d native instructions\n", p, native ? "YES" : "NO", (int)ninst);
    return p;
}

/* both programs: position by a 3x4 matrix, colour passed, the same length */
static char VP_ARL[2048], VP_FLAT[2048];
static const char* VP_ARL_T =
    "!!ARBvp1.0\n"
    "PARAM pal[%d] = { program.env[0..%d] };\n"
    "PARAM proj[4] = { state.matrix.projection };\n"
    "ADDRESS a;\n"
    "TEMP p;\n"
    "ARL a.x, vertex.attrib[6].x;\n"
    "DP4 p.x, pal[a.x + 0], vertex.position;\n"
    "DP4 p.y, pal[a.x + 1], vertex.position;\n"
    "DP4 p.z, pal[a.x + 2], vertex.position;\n"
    "MOV p.w, 1.0;\n"
    "DP4 result.position.x, proj[0], p;\n"
    "DP4 result.position.y, proj[1], p;\n"
    "DP4 result.position.z, proj[2], p;\n"
    "DP4 result.position.w, proj[3], p;\n"
    "MOV result.color, vertex.color;\n"
    "END\n";
static const char* VP_FLAT_T =
    "!!ARBvp1.0\n"
    "PARAM pal[%d] = { program.env[0..%d] };\n"
    "PARAM proj[4] = { state.matrix.projection };\n"
    "TEMP p;\n"
    "MOV p, vertex.attrib[6];\n"
    "DP4 p.x, pal[0], vertex.position;\n"
    "DP4 p.y, pal[1], vertex.position;\n"
    "DP4 p.z, pal[2], vertex.position;\n"
    "MOV p.w, 1.0;\n"
    "DP4 result.position.x, proj[0], p;\n"
    "DP4 result.position.y, proj[1], p;\n"
    "DP4 result.position.z, proj[2], p;\n"
    "DP4 result.position.w, proj[3], p;\n"
    "MOV result.color, vertex.color;\n"
    "END\n";

int main(int argc, char** argv) {
    int B = argc > 1 ? atoi(argv[1]) : 32, V = argc > 2 ? atoi(argv[2]) : 30, F = argc > 3 ? atoi(argv[3]) : 200;
    int P = argc > 4 ? atoi(argv[4]) : 96; /* the palette's size in params */
    snprintf(VP_ARL, sizeof(VP_ARL), VP_ARL_T, P, P - 1);
    snprintf(VP_FLAT, sizeof(VP_FLAT), VP_FLAT_T, P, P - 1);
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFADepthSize, 24,
                                        kCGLPFAPBuffer, 0 };
    CGLPixelFormatObj pf; GLint npf; CGLContextObj ctx; CGLPBufferObj pb;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) || !pf) { printf("no pixel format\n"); return 1; }
    if (CGLCreateContext(pf, NULL, &ctx)) { printf("no context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    if (CGLCreatePBuffer(640, 480, GL_TEXTURE_RECTANGLE_EXT, GL_RGBA, 0, &pb)) { printf("no pbuffer\n"); return 1; }
    if (CGLSetPBuffer(ctx, pb, 0, 0, 0)) { printf("pbuffer not set\n"); return 1; }
    printf("%s / %s; B=%d objects x V=%d vertices, %d frames, a palette of %d params\n", glGetString(GL_RENDERER),
           glGetString(GL_VERSION), B, V, F, P);
    if (B > 32) { printf("B capped at 32 (96 env params)\n"); B = 32; }

    int n = B * V;
    float* pos = malloc(sizeof(float) * 4 * n);
    float* idx = malloc(sizeof(float) * 4 * n);
    unsigned char* col = malloc(4 * n);
    for (int b = 0; b < B; b++)
        for (int v = 0; v < V; v++) {
            int i = b * V + v;
            pos[4 * i + 0] = (float)(v % 7) * 0.1f - 0.3f;
            pos[4 * i + 1] = (float)(v / 7) * 0.1f - 0.2f;
            pos[4 * i + 2] = -2.0f; pos[4 * i + 3] = 1.0f;
            idx[4 * i + 0] = (float)(3 * b); idx[4 * i + 1] = idx[4 * i + 2] = 0; idx[4 * i + 3] = 1;
            col[4 * i + 0] = (unsigned char)(b * 5); col[4 * i + 1] = 128; col[4 * i + 2] = (unsigned char)v; col[4 * i + 3] = 255;
        }
    float mtx[32][12];
    for (int b = 0; b < B; b++) {
        memset(mtx[b], 0, sizeof(mtx[b]));
        mtx[b][0] = mtx[b][5] = mtx[b][10] = 1.0f;
        mtx[b][3] = (float)(b % 8) * 0.2f - 0.7f; mtx[b][7] = (float)(b / 8) * 0.2f - 0.5f;
    }
    glViewport(0, 0, 640, 480);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-1, 1, -0.75, 0.75, 1, 10);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(4, GL_FLOAT, 0, pos);
    glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
    glEnableVertexAttribArrayARB(6); glVertexAttribPointerARB(6, 4, GL_FLOAT, GL_FALSE, 0, idx);
    GLuint parl = load_prog(VP_ARL), pflat = load_prog(VP_FLAT);
    glEnable(GL_VERTEX_PROGRAM_ARB);

    const char* names[3] = { "split (a draw an object, 3 env params before it)", "arl (one draw, pal[A0.x])",
                             "flat (one draw, one matrix)" };
    for (int round = 0; round < 2; round++)
        for (int arm = 0; arm < 3; arm++) {
            glBindProgramARB(GL_VERTEX_PROGRAM_ARB, arm == 1 ? parl : pflat);
            for (int b = 0; b < B; b++)
                for (int r = 0; r < 3; r++) glProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, 3 * b + r, &mtx[b][4 * r]);
            glFinish();
            double t0 = now_ms(), sub = 0;
            for (int f = 0; f < F; f++) {
                double s0 = now_ms();
                glClear(GL_COLOR_BUFFER_BIT);
                if (arm == 0) {
                    for (int b = 0; b < B; b++) {
                        for (int r = 0; r < 3; r++) glProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, r, &mtx[b][4 * r]);
                        glDrawArrays(GL_TRIANGLE_STRIP, b * V, V);
                    }
                } else {
                    glDrawArrays(GL_TRIANGLES, 0, n - n % 3);
                }
                sub += now_ms() - s0;
                glFinish();
            }
            double t = now_ms() - t0;
            printf("round %d  %-52s %7.3f ms a frame (submit %.3f), %.1f ns a vertex\n", round, names[arm], t / F,
                   sub / F, t / F / n * 1e6);
        }
    return 0;
}
