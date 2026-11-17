// gl_win.h's loader: the GL functions past 1.1, from the current context's driver. glTexStorage2D (GL 4.2) falls
// back to glTexImage2D on a 3.3 driver without it.
#include "gl_win.h"

#define PLAT_GL_DEFINE(ret, name, params) PFN_##name plat_##name;
PLAT_GL_FUNCS(PLAT_GL_DEFINE)
#undef PLAT_GL_DEFINE

static void APIENTRY tex_storage_fallback(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    if (internalformat == GL_R32UI) format = GL_RED_INTEGER, type = GL_UNSIGNED_INT;
    else if (internalformat == GL_R8UI) format = GL_RED_INTEGER;
    else if (internalformat == GL_R8) format = GL_RED;
    for (GLsizei i = 0; i < levels; i++) {
        glTexImage2D(target, i, (GLint)internalformat, width, height, 0, format, type, NULL);
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
}

static PROC get(const char *name) {
    PROC p = wglGetProcAddress(name);
    // (some drivers answer 1, 2, 3 or -1 for "none")
    if (p == (PROC)0 || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1) return NULL;
    return p;
}

int plat_gl_load(const char **missing) {
#define PLAT_GL_LOAD(ret, name, params) plat_##name = (PFN_##name)get(#name);
    PLAT_GL_FUNCS(PLAT_GL_LOAD)
#undef PLAT_GL_LOAD
    if (!plat_glTexStorage2D) plat_glTexStorage2D = tex_storage_fallback;
#define PLAT_GL_CHECK(ret, name, params) if (!plat_##name) { if (missing) *missing = #name; return 0; }
    PLAT_GL_FUNCS(PLAT_GL_CHECK)
#undef PLAT_GL_CHECK
    return 1;
}
