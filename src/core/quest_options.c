// vr-options.cfg (quest_options.h)
#include "quest_options.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const QOptDef qopt_defs[QOPT_COUNT] = {
    [QOPT_AIM] = { "aim_guide", 0, 3, QAIM_TRACER },
    [QOPT_PITCH] = { "gun_pitch", -60, 60, 0 },          // (from quest_xr.c's GUN_PITCH_BASE)
    [QOPT_GUN_Y] = { "gun_offset_y_mm", -200, 200, 0 },
    [QOPT_GUN_Z] = { "gun_offset_z_mm", -200, 200, 0 },
    [QOPT_RUMBLE] = { "rumble", 0, 1, 1 },
    [QOPT_CROSSHAIR] = { "cabinet_crosshair", 0, 1, 0 },
    [QOPT_GUN_MODEL] = { "gun_model", 0, 1, QGUN_CABINET },
    [QOPT_GUN_SIZE] = { "gun_size_pct", 70, 130, 100 },  // (of quest_xr.c's fit for each model)
    [QOPT_HAND] = { "gun_hand", 0, 2, QHAND_RIGHT },
    [QOPT_GESTURE] = { "reload_gesture", 0, 2, QGESTURE_MAGAZINE },
    [QOPT_GRIP] = { "grip_reload", 0, 1, 1 },
    [QOPT_CAB_RELOAD] = { "cabinet_reload", 0, 1, QCABRELOAD_OFFSCREEN },
    [QOPT_VIEW] = { "view", 0, 1, QVIEW_IMMERSIVE },
    [QOPT_WORLD] = { "world_scale", 50, 200, 100 },
    [QOPT_HUD] = { "hud_distance_cm", 100, 300, 150 },
    [QOPT_ROOM] = { "cabinet_room", 0, 2, QROOM_COURTYARD },
    // (from quest_xr.c's CAB_BASE_*, the cabinet's default place)
    [QOPT_CAB_SIZE] = { "cabinet_scale_pct", 50, 200, 100 },
    [QOPT_CAB_DIST] = { "cabinet_offset_distance_cm", -150, 250, 0 },
    [QOPT_CAB_HEIGHT] = { "cabinet_offset_height_cm", -80, 80, 0 },
    [QOPT_CAB_SIDE] = { "cabinet_offset_side_cm", -300, 300, 0 },
    [QOPT_CAB_TURN] = { "cabinet_offset_turn_deg", -180, 180, 0 },
    [QOPT_HORIZON] = { "horizon", 0, 2, QHORIZON_LEVEL },
    [QOPT_VIGNETTE] = { "turn_vignette", 0, 2, QVIGNETTE_LOW },
    [QOPT_RESOLUTION] = { "resolution", 0, QRES_COUNT - 1, 2 },
    [QOPT_HZ] = { "refresh", 0, 2, 2 },
    [QOPT_VOLUME] = { "game_volume", 0, 100, 100 },
    [QOPT_CRASH_REPORTS] = { "crash_reports", 0, 1, 1 },
    [QOPT_MUSIC] = { "music_volume", 0, 100, 100 },
    [QOPT_SFX] = { "effects_volume", 0, 100, 100 },
    [QOPT_VOICE] = { "voice_chat_volume", 0, 200, 100 },
    [QOPT_PICTURE] = { "flat_picture", 0, 1, QPICTURE_HD },
    [QOPT_FULLSCREEN] = { "flat_fullscreen", 0, 1, 0 },
    [QOPT_GYRO] = { "flat_gyro", 0, 1, 0 },
    [QOPT_GYRO_SPEED] = { "flat_gyro_speed_pct", 25, 300, 100 },
    // (0: as the arcade drew it; 25 opens the darkest rooms and leaves the lit scenes near the arcade's. A new key:
    // brightness_pct's 50 was chosen while Windows drew the 3D too dark, and saved files carry it)
    [QOPT_BRIGHTNESS] = { "brightness_level_pct", 0, 100, 25 },
    [QOPT_REMOTE_SPEED] = { "tv_remote_speed_pct", 25, 300, 100 },
};

void qoptions_defaults(int *v) {
    for (int i = 0; i < QOPT_COUNT; i++) v[i] = qopt_defs[i].def;
}
void qoptions_load(const char *path, int *v) {
    qoptions_defaults(v);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        for (int i = 0; i < QOPT_COUNT; i++) {
            if (strcmp(line, qopt_defs[i].key)) continue;
            char *end;
            errno = 0;
            long n = strtol(eq + 1, &end, 10);
            while (isspace((unsigned char)*end)) end++;
            if (end != eq + 1 && !*end && !errno && n >= qopt_defs[i].min && n <= qopt_defs[i].max) v[i] = (int)n;
        }
    }
    fclose(f);
}
// Written to a new file and renamed over the old one: a crash mid-write keeps the old settings, and a file the app
// can't write (one copied in with adb belongs to the shell user) is still replaced, the folder being the app's.
int qoptions_save(const char *path, const int *v) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    int ok = 1;
    for (int i = 0; i < QOPT_COUNT; i++) if (fprintf(f, "%s=%d\n", qopt_defs[i].key, v[i]) < 0) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) remove(tmp);
    return ok;
}
