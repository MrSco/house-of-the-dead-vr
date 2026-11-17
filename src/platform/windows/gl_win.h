// OpenGL ES 3.0's API on Windows' desktop OpenGL (3.3 core or later): opengl32.dll exports only GL 1.1, so the
// rest is loaded with wglGetProcAddress once a context is current (plat_gl_load, gl_win.c). The shared code calls
// these by their GL ES names; GLSL starts with PLAT_GLSL_VERSION instead of "#version 300 es".
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAT_GLSL_VERSION "#version 330 core\n"

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef float GLfloat;

#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_R8 0x8229
#define GL_R8UI 0x8232
#define GL_R32UI 0x8236
#define GL_RED_INTEGER 0x8D94
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA8 0x8058
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MAX_SAMPLES 0x8D57
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367

// the functions past GL 1.1: X(return type, name, parameters)
#define PLAT_GL_FUNCS(X) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(void, glDisableVertexAttribArray, (GLuint index)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(void, glGenerateMipmap, (GLenum target)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
    X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)) \
    X(void, glUseProgram, (GLuint program)) \
    X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))

#define PLAT_GL_DECLARE(ret, name, params) typedef ret (APIENTRY *PFN_##name) params; extern PFN_##name plat_##name;
PLAT_GL_FUNCS(PLAT_GL_DECLARE)
#undef PLAT_GL_DECLARE

#define glActiveTexture plat_glActiveTexture
#define glAttachShader plat_glAttachShader
#define glBindBuffer plat_glBindBuffer
#define glBindFramebuffer plat_glBindFramebuffer
#define glBindRenderbuffer plat_glBindRenderbuffer
#define glBindVertexArray plat_glBindVertexArray
#define glBlitFramebuffer plat_glBlitFramebuffer
#define glBufferData plat_glBufferData
#define glCheckFramebufferStatus plat_glCheckFramebufferStatus
#define glCompileShader plat_glCompileShader
#define glCreateProgram plat_glCreateProgram
#define glCreateShader plat_glCreateShader
#define glDeleteFramebuffers plat_glDeleteFramebuffers
#define glDeleteRenderbuffers plat_glDeleteRenderbuffers
#define glDeleteShader plat_glDeleteShader
#define glDisableVertexAttribArray plat_glDisableVertexAttribArray
#define glEnableVertexAttribArray plat_glEnableVertexAttribArray
#define glFramebufferRenderbuffer plat_glFramebufferRenderbuffer
#define glFramebufferTexture2D plat_glFramebufferTexture2D
#define glGenBuffers plat_glGenBuffers
#define glGenFramebuffers plat_glGenFramebuffers
#define glGenRenderbuffers plat_glGenRenderbuffers
#define glGenVertexArrays plat_glGenVertexArrays
#define glGenerateMipmap plat_glGenerateMipmap
#define glGetProgramInfoLog plat_glGetProgramInfoLog
#define glGetProgramiv plat_glGetProgramiv
#define glGetShaderInfoLog plat_glGetShaderInfoLog
#define glGetShaderiv plat_glGetShaderiv
#define glGetUniformLocation plat_glGetUniformLocation
#define glLinkProgram plat_glLinkProgram
#define glRenderbufferStorage plat_glRenderbufferStorage
#define glRenderbufferStorageMultisample plat_glRenderbufferStorageMultisample
#define glShaderSource plat_glShaderSource
#define glTexStorage2D plat_glTexStorage2D
#define glUniform1f plat_glUniform1f
#define glUniform1i plat_glUniform1i
#define glUniform2f plat_glUniform2f
#define glUniform4f plat_glUniform4f
#define glUniformMatrix4fv plat_glUniformMatrix4fv
#define glUseProgram plat_glUseProgram
#define glVertexAttribIPointer plat_glVertexAttribIPointer
#define glVertexAttribPointer plat_glVertexAttribPointer
#define glClearDepthf(d) glClearDepth((GLclampd)(d))

// with the context current: load the functions; 0 and the missing one's name when one is missing
int plat_gl_load(const char **missing);

#ifdef __cplusplus
}
#endif
