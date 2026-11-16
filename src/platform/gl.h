// OpenGL ES 3.0, the API the shared renderer (src/core/m2_gl.c) is written for, on each platform: native on
// Android; Windows (desktop GL 3.3 core) and Apple (ANGLE on Metal) come with their builds (docs/platforms.md,
// "Rendering").
#pragma once
#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#else
#error "src/platform/gl.h: no OpenGL ES 3.0 for this platform yet"
#endif
