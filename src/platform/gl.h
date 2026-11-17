// OpenGL ES 3.0, the API the shared renderer (src/core/m2_gl.c) and the flat shell are written for, on each platform:
// native on Android; Windows' desktop GL 3.3 core through windows/gl_win.h (its functions loaded at run time);
// Apple (ANGLE on Metal) comes with its builds (docs/platforms.md, "Rendering"). Shaders start with
// PLAT_GLSL_VERSION.
#pragma once
#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#define PLAT_GLSL_VERSION "#version 300 es\n"
#elif defined(_WIN32)
#include "windows/gl_win.h"
#else
#error "src/platform/gl.h: no OpenGL ES 3.0 for this platform yet"
#endif
