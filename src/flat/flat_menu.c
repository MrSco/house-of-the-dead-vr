// flat.h's menu: the VR app's menu (quest_menu.c, its flat layout: PICTURE, SYSTEM, ONLINE) on a panel in the
// middle of the window, the game dimmed behind it. The mouse points and clicks; arrows, Enter, Esc and Tab do what a
// controller's stick, A, B and grips do. It is drawn on the CPU only when it changed, then uploaded.
#include "flat.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "quest.h"
#include "quest_menu.h"
#include "quest_options.h"
#include "online.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int open;
    uint8_t *px;                  // QMENU_W x QMENU_H RGBA (straight alpha)
    GLuint tex, prog_tex, prog_dim;
    GLint u_tex_mvp, u_dim_mvp;
    float x, y, s;                // the panel's place in the window: top-left, scale
    int mx, my, down, was_down, pointer;
    int key[8];                   // keys pressed since the last frame
} M;

static const char *VS =
    "uniform mat4 mvp;\nlayout(location=0) in vec3 pos;\nlayout(location=1) in vec2 uv;\nout vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_uv = uv; }\n";
static const char *FS_PANEL =
    "precision mediump float;\nuniform sampler2D tex;\nin vec2 v_uv;\nout vec4 o;\nvoid main() { o = texture(tex, v_uv); }\n";
static const char *FS_DIM =
    "precision mediump float;\nin vec2 v_uv;\nout vec4 o;\nvoid main() { o = vec4(0.0, 0.0, 0.0, 0.55); }\n";

static void menu_action(int a);
static QMenuHost host(void) {
    QMenuHost h;
    memset(&h, 0, sizeof h);
    h.get = flat_opt_get;
    h.set = flat_opt_set;
    h.action = menu_action;
    h.saved = true;
    h.flat = true;
    return h;
}

void flat_menu_set_open(int open) {
    if (open == M.open) return;
    M.open = open;
    if (open) qmenu_open(-1);
    memset(M.key, 0, sizeof M.key);
    M.was_down = M.down;   // (the click that opened or closed it is not a click in it, or a shot)
    plat_log(PLAT_LOG_INFO, "menu %s", open ? "open" : "closed");
}
int flat_menu_is_open(void) { return M.open; }

static void menu_action(int a) {
    if (a == QACT_RESUME) flat_menu_set_open(0);
    else if (a == QACT_GAME_MENU) {   // the game's own pause: Esc, tapped
        flat_menu_set_open(0);
        quest_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 1);
        quest_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
    } else if (a == QACT_QUIT) flat_os_quit();
    else if (a == QACT_ARCADE_IMPORT) flat_os_import_rom();
}

void flat_menu_key(int key) {
    if (key == FLAT_KEY_BACK && !qmenu_modal()) { flat_menu_set_open(0); return; }   // Esc closes it
    if (key >= 0 && key < 8) M.key[key] = 1;
}
void flat_menu_mouse(int x, int y, int button_down) { M.mx = x; M.my = y; M.down = button_down; }

void flat_menu_draw(int w, int h) {
    if (!M.open) return;
    if (!M.px) {
        M.px = calloc((size_t)QMENU_W * QMENU_H, 4);
        glGenTextures(1, &M.tex);
        glBindTexture(GL_TEXTURE_2D, M.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, QMENU_W, QMENU_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        M.prog_tex = flat_gl_program(VS, FS_PANEL);
        M.u_tex_mvp = glGetUniformLocation(M.prog_tex, "mvp");
        M.prog_dim = flat_gl_program(VS, FS_DIM);
        M.u_dim_mvp = glGetUniformLocation(M.prog_dim, "mvp");
        glUseProgram(M.prog_tex);
        glUniform1i(glGetUniformLocation(M.prog_tex, "tex"), 0);
    }
    // the panel's place: as large as fits (up to 1.6x), in the middle
    float s = (float)w * 0.94f / QMENU_W;
    if ((float)h * 0.94f / QMENU_H < s) s = (float)h * 0.94f / QMENU_H;
    if (s > 1.6f) s = 1.6f;
    M.s = s;
    M.x = ((float)w - QMENU_W * s) * 0.5f;
    M.y = ((float)h - QMENU_H * s) * 0.5f;
    // this frame's input
    QMenuInput in;
    memset(&in, 0, sizeof in);
    const float px = ((float)M.mx - M.x) / s, py = ((float)M.my - M.y) / s;
    in.pointer = px >= 0 && py >= 0 && px < QMENU_W && py < QMENU_H;
    in.px = px; in.py = py;
    in.press = M.down && !M.was_down;
    in.release = !M.down && M.was_down;
    in.held = M.down;
    M.was_down = M.down;
    in.stick_x = M.key[FLAT_KEY_RIGHT] ? 1.0f : M.key[FLAT_KEY_LEFT] ? -1.0f : 0.0f;
    in.stick_y = M.key[FLAT_KEY_UP] ? 1.0f : M.key[FLAT_KEY_DOWN] ? -1.0f : 0.0f;
    in.select = M.key[FLAT_KEY_ENTER] != 0;
    in.back = M.key[FLAT_KEY_BACK] != 0;
    in.tab = M.key[FLAT_KEY_TAB] ? 1 : M.key[FLAT_KEY_TAB_BACK] ? -1 : 0;
    in.now = (double)plat_now_ns() * 1e-9;
    memset(M.key, 0, sizeof M.key);
    const QMenuHost hst = host();
    qmenu_update(&hst, &in);
    if (!M.open) return;   // (RESUME)
    int box[4];
    if (qmenu_render(&hst, M.px, in.now, box)) {
        glBindTexture(GL_TEXTURE_2D, M.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, QMENU_W, QMENU_H, GL_RGBA, GL_UNSIGNED_BYTE, M.px);
    }
    // the game dimmed, the panel over it
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    flat_gl_rect(M.prog_dim, M.u_dim_mvp, 0, 0, (float)w, (float)h, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, M.tex);
    flat_gl_rect(M.prog_tex, M.u_tex_mvp, M.x, M.y, QMENU_W * s, QMENU_H * s, w, h);
    glDisable(GL_BLEND);
}
