// The VR settings (the menu's tabs, quest_menu.c), kept in vr-options.cfg in the app's files folder as key=value
// lines. Every value is an int; each has a range, a step and a default (QOptDef).
#pragma once

enum {
    QOPT_AIM, QOPT_PITCH, QOPT_GUN_Y, QOPT_GUN_Z, QOPT_RUMBLE,          // GUN
    QOPT_CROSSHAIR, QOPT_GUN_MODEL, QOPT_GUN_SIZE,
    QOPT_HAND, QOPT_GESTURE, QOPT_GRIP, QOPT_CAB_RELOAD,                // HANDS
    QOPT_VIEW, QOPT_WORLD, QOPT_HUD, QOPT_ROOM,                         // VIEW
    QOPT_CAB_SIZE, QOPT_CAB_DIST, QOPT_CAB_HEIGHT, QOPT_CAB_SIDE, QOPT_CAB_TURN,   // VIEW (the cabinet's place)
    QOPT_HORIZON, QOPT_VIGNETTE,                                        // COMFORT
    QOPT_RESOLUTION, QOPT_HZ,                                           // GRAPHICS
    QOPT_VOLUME, QOPT_CRASH_REPORTS, QOPT_MUSIC, QOPT_SFX, QOPT_VOICE,  // SYSTEM
    QOPT_PICTURE, QOPT_FULLSCREEN, QOPT_GYRO, QOPT_GYRO_SPEED,          // GAME (the flat apps'; gyro: touch screens)
    QOPT_BRIGHTNESS,                                                    // GRAPHICS / GAME (every app)
    QOPT_REMOTE_SPEED,                                                  // GAME (Apple TV: the Siri Remote's sight)
    QOPT_TRIGGER,                                                       // GAME (the flat apps: a pad's triggers)
    QOPT_COUNT
};
enum { QAIM_LASER, QAIM_TRACER, QAIM_DOT, QAIM_OFF };   // TRACER: a streak on each shot (as Time Crisis 2 VR)
enum { QGUN_CABINET, QGUN_PISTOL };   // the gun model: the arcade cabinet's, or the pistol
enum { QHAND_RIGHT, QHAND_LEFT, QHAND_BOTH };      // the gun in your right hand, your left, or one in each
enum { QGESTURE_OFF, QGESTURE_MAGAZINE, QGESTURE_DOWN };   // reloading by moving: a magazine into the grip, or aiming at the floor
enum { QCABRELOAD_GESTURE, QCABRELOAD_OFFSCREEN };   // the cabinet view: Reload gesture as set, or shooting off the screen
enum { QVIEW_IMMERSIVE, QVIEW_CABINET };
// what is round the cabinet view: darkness, your own room (the headset's passthrough), or the mansion's courtyard
enum { QROOM_DARK, QROOM_PASSTHROUGH, QROOM_COURTYARD };
enum { QHORIZON_GAME, QHORIZON_NOROLL, QHORIZON_LEVEL };
enum { QVIGNETTE_OFF, QVIGNETTE_LOW, QVIGNETTE_HIGH };
enum { QPICTURE_HD, QPICTURE_ORIGINAL };   // flat: the 3D at the screen's resolution, or at the arcade's own
// the resolution presets: eye buffers this much of the headset's recommended size
#define QRES_COUNT 4
static const float qres_scale[QRES_COUNT] = { 0.8f, 1.0f, 1.25f, 1.5f };
static const int qhz_rates[3] = { 72, 90, 120 };

typedef struct { const char *key; int min, max, def; } QOptDef;
extern const QOptDef qopt_defs[QOPT_COUNT];

void qoptions_defaults(int *v);
void qoptions_load(const char *path, int *v);          // defaults, then what the file has (bad lines are skipped)
int qoptions_save(const char *path, const int *v);    // 0 when it could not be written
