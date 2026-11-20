// flat.h's app: what the VR app's shell does round the game (quest_xr.c setup_files, game_main), for a screen. The
// files folder holds arcade/ (the ROM set, hotd.zip or hotd.7z, and its images), save/ (settings and scores),
// options.cfg (the menu's), online.cfg, port.log and env.txt (KEY=VALUE lines: the switches of the tests).
//
// Test switches: FLAT_TEST_ONLINE=seconds,host|hostlan|join:CODE|joinip:IP|game:ROW|leave does that ONLINE action
// that long after the start (the line is then taken out of env.txt, so the restart it makes doesn't do it again; ROW:
// a row of the browser, open by QMENU_TEST_SCREEN); FLAT_TEST_PLAY=1 plays: coins and Start every few seconds, and
// shots round the screen, every eighth off it; FLAT_TEST_TALK=1 talks: push to talk, held the whole time (voice frames
// sent without a voice); FLAT_QUIT_AT=seconds closes the app then; FLAT_MENU_AT=seconds[,tab] opens the menu then (on
// that tab: quest_menu.h's QMENU_TAB_*; QMENU_TEST_SCREEN=browse|lan then opens that page of the ONLINE tab,
// controls|host the CONTROLS page or HOST A ROOM's confirmation);
// FLAT_SHOT=seconds[,...] writes the picture to flat_shot.ppm (flat.c); FLAT_TEST_HIDDEN=1
// (Windows) never shows the window: the picture is still drawn, for M2_SHOTS and FLAT_SHOT; FLAT_TEST_LOOK (flat.c)
// looks round as a headset does (with M2_GL_ORDER=0, the headset's depth: the app's own is the arcade's order;
// FLAT_TEST_NEARFAR=1.818,9091 its depth planes, the Quest's); M2_TEST_RESTART_AT=frame presses RESTART then
// (arcade.cpp); FLAT_TEST_UPDATE=state|offered|-|message|tests|installed shows the SYSTEM tab's update line with
// that status (flat_menu.c; the Quest's updater, docs/vr.md).
#include "flat.h"
#include "platform/platform.h"
#include "quest.h"
#include "quest_options.h"
#include "quest_menu.h"
#include "voice.h"
#include "m2_vr.h"
#include "online.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

int SDL_main(int argc, char **argv);   // the game thread (arcade.cpp)

// what the shared code asks of a shell
volatile int arcade_reload_step;       // the VR app's reload gestures: none here
static char files[600];
const char *quest_files_dir(void) { return files; }

static struct {
    int opt[QOPT_COUNT];
    volatile int restart, game_done, game_failed;
} A;

static void request_restart(void) { A.restart = 1; }

// ---------------------------------------------------------------- the settings (the menu's)
static void apply_options(void) {
    extern float port_music_gain, port_sfx_gain;
    quest_volume = A.opt[QOPT_VOLUME];
    port_music_gain = (float)A.opt[QOPT_MUSIC] / 100.0f;
    port_sfx_gain = (float)A.opt[QOPT_SFX] / 100.0f;
    online_set_volume(A.opt[QOPT_VOICE]);   // the partner's voice (online.c keeps it for voice.c)
    flat_set_picture(A.opt[QOPT_PICTURE] == QPICTURE_ORIGINAL ? FLAT_PICTURE_ORIGINAL : FLAT_PICTURE_HD);
    m2_gl_brightness(A.opt[QOPT_BRIGHTNESS]);
    flat_redraw();
}
int flat_opt_get(int o) { return o >= 0 && o < QOPT_COUNT ? A.opt[o] : 0; }

// ---------------------------------------------------------------- gyro aim
static float gyro_x = -1, gyro_y;   // the sight, window pixels
int flat_gyro_on(void) { return A.opt[QOPT_GYRO] != 0 && !flat_menu_is_open(); }
void flat_gyro(float rate_right, float rate_up, double dt) {
    if (!flat_gyro_on()) { gyro_x = -1; return; }
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0) return;
    if (gyro_x < 0) { gyro_x = vx + vw * 0.5f; gyro_y = vy + vh * 0.5f; }   // from the middle
    // 100%: about 40 degrees (0.7 rad) across the picture
    const float k = (float)vw / 0.7f * (float)A.opt[QOPT_GYRO_SPEED] / 100.0f;
    gyro_x -= rate_up * k * (float)dt;
    gyro_y -= rate_right * k * (float)dt;
    // (a little past the picture's edges: aiming off it reloads, as on the cabinet)
    if (gyro_x < vx - vw * 0.06f) gyro_x = vx - vw * 0.06f;
    if (gyro_x > vx + vw * 1.06f) gyro_x = vx + vw * 1.06f;
    if (gyro_y < vy - vh * 0.06f) gyro_y = vy - vh * 0.06f;
    if (gyro_y > vy + vh * 1.06f) gyro_y = vy + vh * 1.06f;
    flat_dev_pointer(FLAT_DEV_TOUCH, (int)gyro_x, (int)gyro_y);
}
void flat_opt_set(int o, int v) {
    if (o < 0 || o >= QOPT_COUNT) return;
    if (v < qopt_defs[o].min) v = qopt_defs[o].min;
    if (v > qopt_defs[o].max) v = qopt_defs[o].max;
    if (A.opt[o] == v) return;
    A.opt[o] = v;
    apply_options();
    if (o == QOPT_FULLSCREEN) flat_os_fullscreen(v);
    char p[700];
    snprintf(p, sizeof p, "%s/options.cfg", files);
    qoptions_save(p, A.opt);
}
int flat_app_restart_requested(void) { return A.restart; }

// ---------------------------------------------------------------- the game thread
static int game_main(void *arg) {
    (void)arg;
    char game[700], save[700];
    snprintf(game, sizeof game, "%s/arcade", files);
    snprintf(save, sizeof save, "%s/save", files);
    // online play (and QUEST_FRESH_SAVE=1, the determinism tests): every player's game starts from the same files, so
    // it saves into a folder of its own that starts empty each time (the game makes its default settings and scores)
    const char *e = getenv("QUEST_FRESH_SAVE");
    if ((e && atoi(e)) || online_session()) {
        snprintf(save, sizeof save, "%s/online-save", files);
        plat_rmtree(save);
        plat_mkdir(save);
        LOGI("fresh save folder %s", save);
    }
    char *argv[] = { "hotdvr", game, save, NULL };
    const int r = SDL_main(3, argv);
    LOGI("game returned %d", r);
    if (r) {
        char msg[1600];
        snprintf(msg, sizeof msg,
                 "The game did not start (exit code %d).\n\nYour ROM set belongs in\n%s\n\n"
                 "(hotd.zip or hotd.7z, MAME's set, Revision A).\n\nport.log in %s has the details.", r, game, files);
        quest_show_message(msg);
        A.game_failed = 1;
    } else {
        A.game_done = 1;
    }
    return 0;
}

static int def_opt[QOPT_COUNT], def_set[QOPT_COUNT];
void flat_app_default(int o, int v) {
    if (o >= 0 && o < QOPT_COUNT) { def_opt[o] = v; def_set[o] = 1; }
}
int flat_opt_default(int o) {
    if (o < 0 || o >= QOPT_COUNT) return 0;
    return def_set[o] ? def_opt[o] : qopt_defs[o].def;
}

int flat_app_start(const char *files_dir) {
    extern int arcade_flat;
    arcade_flat = 1;                        // (arcade.cpp: no cabinet room, so no courtyard for it)
    snprintf(files, sizeof files, "%s", files_dir);
    char p[700];
    plat_mkdir(files);
    snprintf(p, sizeof p, "%s/arcade", files); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/save", files); plat_mkdir(p);
    if (plat_chdir(files)) LOGE("cannot go into %s", files);   // port.log, PORT_SHOT pictures land here
    // env.txt: KEY=VALUE per line
    FILE *f = fopen("env.txt", "r");
    static char line[8192];                 // (PORT_INPUT scripts are long)
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        plat_setenv(line, eq + 1);
        LOGI("env %s=%s", line, eq + 1);
    }
    if (f) fclose(f);
    LOGI("files: %s", files);
    snprintf(p, sizeof p, "%s/options.cfg", files);
    qoptions_load(p, A.opt);
    {   // a mouse needs the sight: Crosshair is ON unless the file says otherwise (the cabinet view's default is OFF)
        FILE *o = fopen(p, "r");
        char t[4096] = "";
        if (o) { t[fread(t, 1, sizeof t - 1, o)] = 0; fclose(o); }
        if (!strstr(t, qopt_defs[QOPT_CROSSHAIR].key)) A.opt[QOPT_CROSSHAIR] = 1;
        for (int o = 0; o < QOPT_COUNT; o++)   // the front end's defaults (flat_app_default)
            if (def_set[o] && !strstr(t, qopt_defs[o].key)) A.opt[o] = def_opt[o];
    }
    flat_os_crash_reports(files, A.opt);   // (a crash of the last run: reported before this run's port.log)
    // online co-op (src/net/online.c): this run may be the restart into an online game (online-session.cfg), whose
    // game starts from fresh saves (game_main); voice chat beside it
    voice_init();
    online_restart = request_restart;
    if (online_boot(files)) online_start();
    apply_options();
    if (!plat_thread_start_stack(game_main, NULL, (size_t)64 << 20)) {   // recompiled code nests deeply
        LOGE("cannot start the game thread");
        return 0;
    }
    return 1;
}

// FLAT_TEST_ONLINE and FLAT_TEST_PLAY (tests)
static void env_txt_drop(const char *key) {
    FILE *f = fopen("env.txt", "r");
    if (!f) return;
    static char all[16384];
    size_t n = 0;
    char line[8192];
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, strlen(key)) && n + strlen(line) < sizeof all) { memcpy(all + n, line, strlen(line)); n += strlen(line); }
    fclose(f);
    if ((f = fopen("env.txt", "w"))) { fwrite(all, 1, n, f); fclose(f); }
}
static void test_hooks(void) {
    static uint64_t t0;
    static double online_at = -2;
    static char what[64];
    if (!t0) t0 = plat_now_ns();
    const double t = (double)(plat_now_ns() - t0) * 1e-9;
    if (online_at == -2) {
        const char *e = getenv("FLAT_TEST_ONLINE");
        online_at = e && *e ? atof(e) : -1;
        if (e && strchr(e, ',')) snprintf(what, sizeof what, "%s", strchr(e, ',') + 1);
    }
    if (online_at >= 0 && t >= online_at) {
        online_at = -1;
        env_txt_drop("FLAT_TEST_ONLINE=");
        plat_setenv("FLAT_TEST_ONLINE", "");   // (nor from this process's environment, which a restart inherits)
        LOGI("test: online %s", what);
        if (!strcmp(what, "host")) online_host_room();
        else if (!strcmp(what, "hostlan")) online_host_lan();
        else if (!strncmp(what, "join:", 5)) online_join_code(what + 5);
        else if (!strncmp(what, "joinip:", 7)) { online_set_join_ip(what + 7); online_join_ip(); }
        else if (!strcmp(what, "leave")) online_leave();
        else if (!strncmp(what, "game:", 5)) online_join_game(atoi(what + 5));
    }
    static double quit_at = -2, menu_at = -2;
    static int menu_tab = -1;
    if (quit_at == -2) {
        const char *q = getenv("FLAT_QUIT_AT"), *m = getenv("FLAT_MENU_AT");
        quit_at = q && *q ? atof(q) : -1;
        menu_at = -1;
        if (m && *m) sscanf(m, "%lf,%d", &menu_at, &menu_tab);
    }
    if (quit_at >= 0 && t >= quit_at) { quit_at = -1; flat_os_quit(); }
    if (menu_at >= 0 && t >= menu_at) { menu_at = -1; flat_menu_set_open(1); if (menu_tab >= 0) qmenu_open(menu_tab); }
    static int talk = -1;
    if (talk < 0) { talk = getenv("FLAT_TEST_TALK") && atoi(getenv("FLAT_TEST_TALK")); if (talk) online_set_voice_mode(2); }
    if (talk) online_talk_key(true);
    static int play = -1;
    if (play < 0) play = getenv("FLAT_TEST_PLAY") && atoi(getenv("FLAT_TEST_PLAY"));
    if (!play) return;
    static double next_start = 3, next_shot = 4;
    static int down, shots;
    static uint32_t rng = 12345;
    if (t >= next_start) {   // a tap of the skip button: coins and Start
        quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 1);
        quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 0);
        next_start = t + 2.5;
    }
    if (down) { quest_mouse_button(SDL_BUTTON_LEFT, 0); down = 0; }
    else if (t >= next_shot) {
        rng = rng * 1103515245u + 12345u;
        int x = 40 + (int)((rng >> 8) % 416), y = 40 + (int)((rng >> 18) % 304);
        if (++shots % 8 == 0) x = -20;   // off the screen: reload
        quest_pointer(x, y);
        quest_mouse_button(SDL_BUTTON_LEFT, 1);
        down = 1;
        next_shot = t + 0.3;
    }
}

void flat_app_frame(int minimized) {
    test_hooks();
    flat_players_frame();
    extern int port_draw_crosshairs;
    extern volatile int arcade_view_cabinet;
    // the game's own sights: yours where you aim (Crosshair ON) and, online, the partner's regardless
    port_draw_crosshairs = A.opt[QOPT_CROSSHAIR] ? 2 : online_session() ? 1 : 0;
    arcade_view_cabinet = 1;                // the arcade's picture: a zombie's hit is in it, as on the cabinet
    // the game pauses while the menu is open or nothing shows; not online (a partner's game would stall)
    quest_set_paused((minimized || flat_menu_is_open()) && !online_session());
    quest_audio_service();
}

void flat_app_quit(void) {
    online_quit();                          // the partner hears us go at once (else they wait out a timeout)
    quest_quit();
    quest_audio_shutdown();
}
