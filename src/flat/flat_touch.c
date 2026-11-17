// flat.h's touch controls: on a touch screen the picture itself is the gun (a tap shoots there), and three buttons sit
// beside it, in the bars a wide screen leaves round the 4:3 picture: RELOAD on the left, START and MENU on the right
// (over the picture's corners, faint, when there are no bars). Drawn on the CPU canvas (quest_canvas.c) once.
#include "flat.h"
#include "platform/gl.h"
#include "quest_canvas.h"
#include "ui_font_data.h"
#include <stdlib.h>
#include <string.h>

enum { CELL = 160, NB = 3 };
static struct {
    int on;
    GLuint tex, prog;
    GLint u_mvp, u_alpha;
    float x[NB + 1], y[NB + 1], s;   // each button's top-left and the size (window pixels), by FLAT_BTN_*
    int down[NB + 1];
    float alpha;
} T;

static const char *VS =
    "uniform mat4 mvp;\nlayout(location=0) in vec3 pos;\nlayout(location=1) in vec2 uv;\nout vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_uv = uv; }\n";
static const char *FS =
    "precision mediump float;\nuniform sampler2D tex;\nuniform float alpha;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { vec4 c = texture(tex, v_uv); o = vec4(c.rgb, c.a * alpha); }\n";

void flat_touch_controls(int on) { T.on = on; }

static void make_texture(void) {
    uint8_t *px = calloc((size_t)CELL * NB * CELL * 2, 4);   // two rows: up, pressed
    QCanvas c;
    qc_init(&c, px, CELL * NB, CELL * 2);
    qc_fill(&c, qrgba(0, 0, 0, 0));
    static const char *const labels[NB] = { "START", "RELOAD", "MENU" };
    for (int row = 0; row < 2; row++)
        for (int i = 0; i < NB; i++) {
            const float x = (float)(i * CELL) + 8, y = (float)(row * CELL) + 8, s = CELL - 16;
            qc_rrect(&c, x, y, s, s, 28, row ? qrgba(255, 202, 110, 230) : qrgba(12, 19, 29, 200));
            qc_rrect_line(&c, x, y, s, s, 28, 4, row ? qrgba(255, 230, 180, 255) : qrgba(54, 183, 207, 255));
            qc_text_fit(&c, UI_B24, x, y + s / 2 + 9, s, labels[i], row ? qrgba(26, 18, 6, 255) : qrgba(238, 243, 246, 255), 1);
        }
    glGenTextures(1, &T.tex);
    glBindTexture(GL_TEXTURE_2D, T.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CELL * NB, CELL * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    T.prog = flat_gl_program(VS, FS);
    T.u_mvp = glGetUniformLocation(T.prog, "mvp");
    T.u_alpha = glGetUniformLocation(T.prog, "alpha");
    glUseProgram(T.prog);
    glUniform1i(glGetUniformLocation(T.prog, "tex"), 0);
}

// the buttons' places for a w x h window with the picture at vx, vw
static void layout(int w, int h) {
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    const float bar = (float)vx, pad = (float)h * 0.04f;
    if (bar >= (float)h * 0.16f) {   // in the bars
        T.s = bar * 0.8f < (float)h * 0.26f ? bar * 0.8f : (float)h * 0.26f;
        T.alpha = 1;
        T.x[FLAT_BTN_RELOAD] = (bar - T.s) / 2;            T.y[FLAT_BTN_RELOAD] = (float)h - pad - T.s;
        T.x[FLAT_BTN_START] = (float)w - bar + (bar - T.s) / 2; T.y[FLAT_BTN_START] = (float)h - pad - T.s;
        T.x[FLAT_BTN_MENU] = T.x[FLAT_BTN_START];          T.y[FLAT_BTN_MENU] = pad;
    } else {                         // over the picture's corners
        T.s = (float)(w < h ? w : h) * 0.15f;
        T.alpha = 0.55f;
        T.x[FLAT_BTN_RELOAD] = pad;                        T.y[FLAT_BTN_RELOAD] = (float)h - pad - T.s;
        T.x[FLAT_BTN_START] = (float)w - pad - T.s;        T.y[FLAT_BTN_START] = (float)h - pad - T.s;
        T.x[FLAT_BTN_MENU] = (float)w - pad - T.s;         T.y[FLAT_BTN_MENU] = pad;
    }
}

int flat_touch_button(int x, int y) {
    if (!T.on) return FLAT_BTN_NONE;
    for (int b = 1; b <= NB; b++)
        if (x >= T.x[b] && x < T.x[b] + T.s && y >= T.y[b] && y < T.y[b] + T.s) return b;
    return FLAT_BTN_NONE;
}
void flat_touch_pressed(int button, int down) { if (button > 0 && button <= NB) T.down[button] = down; }

void flat_touch_draw(int w, int h) {
    if (!T.on) return;
    if (!T.tex) make_texture();
    layout(w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, T.tex);
    glUseProgram(T.prog);
    glUniform1f(T.u_alpha, T.alpha);
    // (flat_gl_rect draws uv 0..1 over the whole atlas: each button's cell is chosen by its own uv range)
    for (int b = 1; b <= NB; b++) {
        const int cell = b == FLAT_BTN_START ? 0 : b == FLAT_BTN_RELOAD ? 1 : 2;
        flat_gl_rect_uv(T.prog, T.u_mvp, T.x[b], T.y[b], T.s, T.s, w, h,
                        (float)cell / NB, T.down[b] ? 0.5f : 0.0f, (float)(cell + 1) / NB, T.down[b] ? 1.0f : 0.5f);
    }
    glDisable(GL_BLEND);
}
