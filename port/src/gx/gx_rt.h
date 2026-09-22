/* The render thread's door (M27, PLAN.md 42).
 *
 * Every GL entry point the port calls has a twin here.  With the render
 * thread off the twin is the GL call; with it on the twin appends a record
 * to the command stream in src/gx/rt.c and the render thread makes the call
 * later, in order, from the record alone.  The redefinitions at the bottom
 * make the twins the only way a file that includes this header can reach GL,
 * so a new call written as `glFoo(...)` in gl13.c / gx_tev.c / gx_tex.c /
 * gx_vprog.c / gx_draw.c goes through the door without anyone remembering
 * to; a call with no twin is a link error, which is the point.
 *
 * Include AFTER <SDL_opengl.h> (it wants the GL types) and never from rt.c,
 * which is the one file that calls the real functions.
 */
#ifndef PORT_GX_RT_H
#define PORT_GX_RT_H

#ifndef PORT_NO_SDL

/* recording is on: the render thread (or the inline replay) owns GL */
extern int rt_recording;

/* ---- state ---- */
void rt_glEnable(GLenum cap);
void rt_glDisable(GLenum cap);
void rt_glEnableClientState(GLenum a);
void rt_glDisableClientState(GLenum a);
void rt_glActiveTexture(GLenum u);
void rt_glClientActiveTexture(GLenum u);
void rt_glBindTexture(GLenum target, GLuint name);
void rt_glTexParameteri(GLenum t, GLenum p, GLint v);
void rt_glTexParameterf(GLenum t, GLenum p, GLfloat v);
void rt_glTexEnvi(GLenum t, GLenum p, GLint v);
void rt_glTexEnvf(GLenum t, GLenum p, GLfloat v);
void rt_glTexEnvfv(GLenum t, GLenum p, const GLfloat* v);
void rt_glMatrixMode(GLenum m);
void rt_glLoadIdentity(void);
void rt_glLoadMatrixf(const GLfloat* m);
void rt_glPushMatrix(void);
void rt_glPopMatrix(void);
void rt_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void rt_glDepthMask(GLboolean b);
void rt_glDepthFunc(GLenum f);
void rt_glDepthRange(GLclampd n, GLclampd f);
void rt_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void rt_glCullFace(GLenum m);
void rt_glFrontFace(GLenum m);
void rt_glShadeModel(GLenum m);
void rt_glPolygonMode(GLenum f, GLenum m);
void rt_glHint(GLenum t, GLenum m);
void rt_glBlendFunc(GLenum s, GLenum d);
void rt_glBlendEquation(GLenum m);
void rt_glAlphaFunc(GLenum f, GLclampf ref);
void rt_glFogi(GLenum p, GLint v);
void rt_glFogf(GLenum p, GLfloat v);
void rt_glLineWidth(GLfloat w); /* M30 */
void rt_glFogfv(GLenum p, const GLfloat* v);
void rt_glLightf(GLenum l, GLenum p, GLfloat v);
void rt_glLightfv(GLenum l, GLenum p, const GLfloat* v);
void rt_glLightModeli(GLenum p, GLint v);
void rt_glLightModelfv(GLenum p, const GLfloat* v);
void rt_glMaterialfv(GLenum face, GLenum p, const GLfloat* v);
void rt_glColorMaterial(GLenum face, GLenum m);
void rt_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void rt_glClearDepth(GLclampd d);
void rt_glClear(GLbitfield mask);
void rt_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void rt_glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void rt_glPixelStorei(GLenum p, GLint v);
void rt_glReadBuffer(GLenum m);
void rt_glFinish(void);
/* ---- immediate mode (the downsample read, the fullscreen blit) ---- */
void rt_glBegin(GLenum m);
void rt_glEnd(void);
void rt_glTexCoord2f(GLfloat s, GLfloat t);
void rt_glVertex2f(GLfloat x, GLfloat y);
/* ---- arrays: the pointers are into the ring or into a payload (rt_stash) ---- */
void rt_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p);
void rt_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p);
void rt_glNormalPointer(GLenum type, GLsizei stride, const GLvoid* p);
void rt_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p);
void rt_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void rt_glDrawRangeElements(GLenum mode, GLuint lo, GLuint hi, GLsizei n, GLenum type,
                            const GLvoid* idx); /* copies the indices */
/* ---- textures ---- */
void rt_glGenTextures(GLsizei n, GLuint* out);
void rt_glDeleteTextures(GLsizei n, const GLuint* names);
/* glTexImage2D: the pixels are copied into the stream (small ones); the
 * `_owned` twin takes the malloc'd buffer and the replay frees it */
void rt_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                     GLenum fmt, GLenum type, const GLvoid* px);
void rt_teximage2d_owned(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                         GLint border, GLenum fmt, GLenum type, void* px_malloced);
void rt_texsubimage2d_owned(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                            GLenum fmt, GLenum type, void* px_malloced); /* M38 */
void rt_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,
                            GLsizei w, GLsizei h);
/* ---- reads: each is a join (the record, then a wait for the replay) ---- */
void rt_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                     GLvoid* out);
void rt_glGetTexImage(GLenum target, GLint level, GLenum fmt, GLenum type, GLvoid* out);
GLenum rt_glGetError(void);
/* init-time only: a bug if reached while recording (logged, direct call) */
const GLubyte* rt_glGetString(GLenum name);
void rt_glGetIntegerv(GLenum p, GLint* v);
/* ---- the extensions gl13.c / gx_vprog.c reach through pointers ---- */
void rt_ext_multi_draw_arrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n);
void rt_ext_flush_var(GLsizei len, const GLvoid* p);
void rt_ext_set_fence(GLuint f, int chunk); /* leaving chunk `chunk` (-1: none) */
/* test-then-finish, counted on the replaying side: gl13_var_enter's wait */
void rt_ext_wait_fence(GLuint f, int chunk);
void rt_ext_fogcoord_pointer(GLenum type, GLsizei stride, const GLvoid* p);
void rt_ext_bind_program(GLenum target, GLuint id);
void rt_ext_env_param4fv(GLenum target, GLuint idx, const GLfloat* v);
void rt_ext_env_params4fv(GLenum target, GLuint idx, GLsizei n, const GLfloat* v);

/* A payload: `n` bytes copied into the stream, the copy's address returned;
 * valid until the replay passes it.  For array pointers that would otherwise
 * name memory the game thread rewrites (out_buf).  Direct mode returns `p`. */
const void* rt_stash(const void* p, size_t n);

/* A function run on the render thread with a copy of `args` (n bytes), in
 * stream order.  `sync` waits for it (a join).  The function must read
 * nothing but its arguments.  Direct mode calls it at once. */
void rt_call(void (*fn)(void*), const void* args, size_t n, int sync);

/* the vertex program compile, rt_compile_vprog, and the lifecycle (rt_start,
 * rt_stop, rt_join, rt_gate, rt_ring_*, rt_present, rt_report, rt_status) are
 * declared in port.h: they carry no GL types and the platform files call them */

/* ---- the redefinitions: this file's GL calls go through the door ---- */
#define glEnable rt_glEnable
#define glDisable rt_glDisable
#define glEnableClientState rt_glEnableClientState
#define glDisableClientState rt_glDisableClientState
#define glActiveTexture rt_glActiveTexture
#define glClientActiveTexture rt_glClientActiveTexture
#define glBindTexture rt_glBindTexture
#define glTexParameteri rt_glTexParameteri
#define glTexParameterf rt_glTexParameterf
#define glTexEnvi rt_glTexEnvi
#define glTexEnvf rt_glTexEnvf
#define glTexEnvfv rt_glTexEnvfv
#define glMatrixMode rt_glMatrixMode
#define glLoadIdentity rt_glLoadIdentity
#define glLoadMatrixf rt_glLoadMatrixf
#define glPushMatrix rt_glPushMatrix
#define glPopMatrix rt_glPopMatrix
#define glOrtho rt_glOrtho
#define glDepthMask rt_glDepthMask
#define glDepthFunc rt_glDepthFunc
#define glDepthRange rt_glDepthRange
#define glColorMask rt_glColorMask
#define glCullFace rt_glCullFace
#define glFrontFace rt_glFrontFace
#define glShadeModel rt_glShadeModel
#define glPolygonMode rt_glPolygonMode
#define glHint rt_glHint
#define glBlendFunc rt_glBlendFunc
#define glBlendEquation rt_glBlendEquation
#define glAlphaFunc rt_glAlphaFunc
#define glFogi rt_glFogi
#define glFogf rt_glFogf
#define glLineWidth rt_glLineWidth
#define glFogfv rt_glFogfv
#define glLightf rt_glLightf
#define glLightfv rt_glLightfv
#define glLightModeli rt_glLightModeli
#define glLightModelfv rt_glLightModelfv
#define glMaterialfv rt_glMaterialfv
#define glColorMaterial rt_glColorMaterial
#define glClearColor rt_glClearColor
#define glClearDepth rt_glClearDepth
#define glClear rt_glClear
#define glViewport rt_glViewport
#define glScissor rt_glScissor
#define glPixelStorei rt_glPixelStorei
#define glReadBuffer rt_glReadBuffer
#define glFinish rt_glFinish
#define glBegin rt_glBegin
#define glEnd rt_glEnd
#define glTexCoord2f rt_glTexCoord2f
#define glVertex2f rt_glVertex2f
#define glVertexPointer rt_glVertexPointer
#define glColorPointer rt_glColorPointer
#define glNormalPointer rt_glNormalPointer
#define glTexCoordPointer rt_glTexCoordPointer
#define glDrawArrays rt_glDrawArrays
#define glDrawRangeElements rt_glDrawRangeElements
#define glGenTextures rt_glGenTextures
#define glDeleteTextures rt_glDeleteTextures
#define glTexImage2D rt_glTexImage2D
#define glCopyTexSubImage2D rt_glCopyTexSubImage2D
#define glReadPixels rt_glReadPixels
#define glGetTexImage rt_glGetTexImage
#define glGetError rt_glGetError
#define glGetString rt_glGetString
#define glGetIntegerv rt_glGetIntegerv

#endif /* !PORT_NO_SDL */
#endif /* PORT_GX_RT_H */
