// Meta Quest (Android, OpenXR) host: the game runs unchanged on its own thread against a small SDL2 subset
// (host_sdl.c); the VR side (quest_xr.c) shows each presented frame on a screen in front of the player and turns
// the controllers into light guns. These are the calls between the two threads.
#pragma once
#include <stdint.h>

// ---- game thread -> VR thread (host_sdl.c)
// The newest presented frame (RGBA8, w x h). Returns 1 and copies it when it changed since the last call.
int qframe_take(uint32_t *dst, int max_pixels, int *w, int *h);
// The size of the frames the game presents (the coordinate space of quest_pointer); 0 before the first frame.
void qframe_size(int *w, int *h);

// ---- VR thread -> game thread (host_sdl.c)
void quest_pointer(int x, int y);               // the active gun's aim, in frame pixels
void quest_mouse_button(int button, int down);  // SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT
void quest_key(int sdl_scancode, int sdl_keycode, int down);
// player 2's gun beside this player's (the flat apps' local second player, never online: arcade.cpp): its aim (frame
// pixels), its buttons (SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT) and its Start; quest_player2(0): it left (its crosshair
// goes, its trigger lets go)
void quest_pointer2(int x, int y);
void quest_mouse_button2(int button, int down);
void quest_start2(void);
void quest_player2(int in);
void quest_quit(void);
void arcade_restart(void);                      // the game back to its start, between frames (arcade.cpp RESTART)
int host_gun_live_rect(int *x0, int *y0, int *x1, int *y1);   // where the game takes a shot (picture pixels)
int host_player_ammo(int p);                    // the game's bullets for player p, -1 out of play (gun_input.c)
int host_player_playing(int p);                 // 1 while the game takes player p's shots (not menus, cut scenes)
// player p's place in the game (the game's own byte): HOST_P_IN in it (playing, or in a scene), HOST_P_OUT out of it
// (the continue countdown, then that player's game over while the other plays), HOST_P_RANKING the game over's
// ranking, HOST_P_NONE not in it (the title and attract, or a player 2 who never joined); others between (1 and 3
// as a player comes in, 0 at the boot)
enum { HOST_P_OUT = 4, HOST_P_IN = 5, HOST_P_RANKING = 6, HOST_P_NONE = 9 };
int host_player_state(int p);
// Player p's events, read from the game's RAM each game frame (arcade.cpp update_ammo): counts that only rise, for
// the rumble of every shell. HOST_EV_SHOT a bullet spent, HOST_EV_RELOAD the magazine full again (in play),
// HOST_EV_HIT a life lost. (A save state loaded or a jump of more than one is none of them.)
enum { HOST_EV_SHOT, HOST_EV_RELOAD, HOST_EV_HIT, HOST_EV_COUNT };
unsigned host_player_events(int p, int ev);
// The events new since a shell last looked (seen: its own, zeroed at first; the first look takes the counts as they
// are, so a shell that starts late doesn't get the past): a bit (1 << HOST_EV_*) each. Several at once (the game ran
// two frames between looks) are one.
typedef struct { unsigned n[2][HOST_EV_COUNT]; int primed[2]; } HostEventsSeen;
static inline int host_events_new(HostEventsSeen *seen, int p) {
    int bits = 0;
    if (p < 0 || p > 1) return 0;
    for (int e = 0; e < HOST_EV_COUNT; e++) {
        const unsigned n = host_player_events(p, e);
        if (seen->primed[p] && n != seen->n[p][e]) bits |= 1 << e;
        seen->n[p][e] = n;
    }
    seen->primed[p] = 1;
    return bits;
}
// Where player p's gun aims in the game's newest frame (picture pixels, as quest_pointer's); 0: off the screen, or
// that player has no gun. Online it is the lockstep's own input, so the partner's is there whatever their app.
int host_player_aim(int p, int *x, int *y);
// While paused (system menu open, headset off) the game thread waits in its next present and audio is silent.
void quest_set_paused(int paused);
void quest_audio_service(void);                 // reopen the audio stream after the output device changed
void quest_audio_shutdown(void);                // the app is exiting: the stream stopped and closed first
void quest_show_message(const char *text);      // a text page instead of the game (missing files, errors)
void quest_clear_message(void);                  // the game's pictures again (Vision Pro: the ROM set came in)
extern volatile int quest_volume;              // the game's sound, 0..100 (the VR menu's Game volume)
// RGB565 value that SDL_RenderCopy turns into transparent pixels (-1: none): the 2D overlay over the VR scene
extern int quest_overlay_key;
// 5x7 text into the frame being drawn (between SDL_RenderCopy and SDL_RenderPresent); colour as 0xAABBGGRR
void quest_render_text(int x, int y, int scale, uint32_t rgba, const char *s);

// ---- VR thread (quest_xr.c)
const char *quest_files_dir(void);              // the app's external files dir: game/, save/, port.log, env.txt
