/* OpenGL 3.3 core for vrde. macOS declares and exports all of it. Windows' opengl32.dll has
   only OpenGL 1.1: the newer functions vrde calls are loaded from the driver once the context
   exists (gl_load_functions), each into a pointer that its name is #defined to. */
#ifndef VRDE_GL_H
#define VRDE_GL_H

#ifdef __APPLE__

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

static int gl_load_functions(void) { return 1; }

#else

#include <SDL.h>
#include <SDL_opengl.h>

/* The functions after OpenGL 1.1 that vrde calls */
#define VRDE_GL_FUNCTIONS(X) \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
  X(PFNGLATTACHSHADERPROC, glAttachShader) \
  X(PFNGLBINDBUFFERPROC, glBindBuffer) \
  X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
  X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
  X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
  X(PFNGLBUFFERDATAPROC, glBufferData) \
  X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
  X(PFNGLCLEARBUFFERUIVPROC, glClearBufferuiv) \
  X(PFNGLCOMPILESHADERPROC, glCompileShader) \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
  X(PFNGLCREATESHADERPROC, glCreateShader) \
  X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
  X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
  X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
  X(PFNGLGENBUFFERSPROC, glGenBuffers) \
  X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
  X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
  X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
  X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
  X(PFNGLSHADERSOURCEPROC, glShaderSource) \
  X(PFNGLUNIFORM1FPROC, glUniform1f) \
  X(PFNGLUNIFORM1IPROC, glUniform1i) \
  X(PFNGLUNIFORM2FPROC, glUniform2f) \
  X(PFNGLUNIFORM2IPROC, glUniform2i) \
  X(PFNGLUSEPROGRAMPROC, glUseProgram) \
  X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer) \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define VRDE_GL_POINTER(type, name) static type vrde_##name;
VRDE_GL_FUNCTIONS(VRDE_GL_POINTER)

/* Prints the first one the driver lacks and returns 0 */
static int gl_load_functions(void)
{
#define VRDE_GL_LOAD(type, name) \
  if (!(vrde_##name = (type)SDL_GL_GetProcAddress(#name))) \
  { \
    fprintf(stderr, "OpenGL: no %s\n", #name); \
    return 0; \
  }
  VRDE_GL_FUNCTIONS(VRDE_GL_LOAD)
  return 1;
}

#define glActiveTexture vrde_glActiveTexture
#define glAttachShader vrde_glAttachShader
#define glBindBuffer vrde_glBindBuffer
#define glBindFramebuffer vrde_glBindFramebuffer
#define glBindRenderbuffer vrde_glBindRenderbuffer
#define glBindVertexArray vrde_glBindVertexArray
#define glBlitFramebuffer vrde_glBlitFramebuffer
#define glBufferData vrde_glBufferData
#define glCheckFramebufferStatus vrde_glCheckFramebufferStatus
#define glClearBufferuiv vrde_glClearBufferuiv
#define glCompileShader vrde_glCompileShader
#define glCreateProgram vrde_glCreateProgram
#define glCreateShader vrde_glCreateShader
#define glDeleteFramebuffers vrde_glDeleteFramebuffers
#define glDrawBuffers vrde_glDrawBuffers
#define glEnableVertexAttribArray vrde_glEnableVertexAttribArray
#define glFramebufferRenderbuffer vrde_glFramebufferRenderbuffer
#define glFramebufferTexture2D vrde_glFramebufferTexture2D
#define glGenBuffers vrde_glGenBuffers
#define glGenFramebuffers vrde_glGenFramebuffers
#define glGenRenderbuffers vrde_glGenRenderbuffers
#define glGenVertexArrays vrde_glGenVertexArrays
#define glGetShaderInfoLog vrde_glGetShaderInfoLog
#define glGetShaderiv vrde_glGetShaderiv
#define glGetUniformLocation vrde_glGetUniformLocation
#define glLinkProgram vrde_glLinkProgram
#define glRenderbufferStorage vrde_glRenderbufferStorage
#define glRenderbufferStorageMultisample vrde_glRenderbufferStorageMultisample
#define glShaderSource vrde_glShaderSource
#define glUniform1f vrde_glUniform1f
#define glUniform1i vrde_glUniform1i
#define glUniform2f vrde_glUniform2f
#define glUniform2i vrde_glUniform2i
#define glUseProgram vrde_glUseProgram
#define glVertexAttribIPointer vrde_glVertexAttribIPointer
#define glVertexAttribPointer vrde_glVertexAttribPointer

#endif

#endif
