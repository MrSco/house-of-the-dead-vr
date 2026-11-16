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
void quest_quit(void);
int host_player_ammo(int p);                    // the game's bullets for player p, -1 out of play (gun_input.c)
int host_player_playing(int p);                 // 1 while the game takes player p's shots (not menus, cut scenes)
// While paused (system menu open, headset off) the game thread waits in its next present and audio is silent.
void quest_set_paused(int paused);
void quest_audio_service(void);                 // reopen the audio stream after the output device changed
void quest_audio_shutdown(void);                // the app is exiting: the stream stopped and closed first
void quest_show_message(const char *text);      // a text page instead of the game (missing files, errors)
extern volatile int quest_volume;              // the game's sound, 0..100 (the VR menu's Game volume)
// RGB565 value that SDL_RenderCopy turns into transparent pixels (-1: none): the 2D overlay over the VR scene
extern int quest_overlay_key;
// 5x7 text into the frame being drawn (between SDL_RenderCopy and SDL_RenderPresent); colour as 0xAABBGGRR
void quest_render_text(int x, int y, int scale, uint32_t rgba, const char *s);

// ---- VR thread (quest_xr.c)
const char *quest_files_dir(void);              // the app's external files dir: game/, save/, port.log, env.txt
