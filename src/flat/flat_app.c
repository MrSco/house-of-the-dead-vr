// flat.h's app: what the VR app's shell does round the game (quest_xr.c setup_files, game_main), for a screen. The
// files folder holds arcade/ (the ROM set, hotd.zip or hotd.7z, and its images), save/ (settings and scores),
// options.cfg (the menu's), online.cfg, port.log and env.txt (KEY=VALUE lines: the switches of the tests).
#include "flat.h"
#include "platform/platform.h"
#include "quest.h"
#include "quest_options.h"
#include "voice.h"
#include "online.h"
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
}
int flat_opt_get(int o) { return o >= 0 && o < QOPT_COUNT ? A.opt[o] : 0; }
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

int flat_app_start(const char *files_dir) {
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
    }
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

void flat_app_frame(int minimized) {
    extern int port_draw_crosshairs;
    extern volatile int arcade_view_cabinet;
    // the game's own sight where you aim (Crosshair); online the partner's shows regardless
    port_draw_crosshairs = A.opt[QOPT_CROSSHAIR] || online_session();
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
