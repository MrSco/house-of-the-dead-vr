// Online co-op for the native builds (the Quest; the desktop build for tests): two players in one game, as on the
// arcade cabinet. See online.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- start-up (before the game starts) ----
// Reads online.cfg (name, server, voice) and online-session.cfg, which a restart into an online game leaves behind
// (or PORT_ONLINE from the environment, for tests). Returns 1 when this run is online: net_mode is then set (host or
// guest) and the game must start from fresh save files (every player's game starts the same).
int online_boot(const char *files_dir);
void online_start(void);                 // after net_init: the network thread, when online
bool online_session(void);               // this run plays online (host or guest)
void online_quit(void);                  // the app is closing: a guest's game stops waiting

// The platform's restart (quest_xr.c: the app closes and opens again); the desktop build has none (tests start each
// process online through PORT_ONLINE)
extern void (*online_restart)(void);

// ---- the VR menu's ONLINE tab ----
enum { OV_OFFLINE, OV_FINDING, OV_CONNECTING, OV_WAITING, OV_LINKED, OV_CATCHING_UP, OV_PARTNER_GONE, OV_FAILED };
typedef struct online_view {
    int state;                           // OV_*
    bool host, lan;                      // this run hosts (P1) / joined (P2); on the local network
    char name[16], server[64];
    char code[5];                        // the room's code (hosting, or the one joined)
    char partner[24];
    int ping_ms;                         // -1 unknown
    int catch_up_pct;                    // OV_CATCHING_UP: how much of the host's game this guest has run
    bool desync;                         // the two games went apart (a guest)
    char message[96];                    // why the last action failed, or what is happening now
    char address[3][24]; int address_count;   // this headset's addresses (LAN hosting)
    // voice
    int voice_mode;                      // 0 off, 1 open mic, 2 push to talk
    bool muted, mic_denied, talking, partner_talking;
    int level, partner_level, volume;    // levels 0..8, volume percent
    // the browser
    bool browse_answered, browse_busy, discovering;
    char server_name[64]; int server_rooms, server_players;
    char join_ip[24];
} online_view;
enum { OG_JOIN, OG_PLAYING, OG_FULL, OG_OLD };
typedef struct online_game { char name[32], detail[40]; int players, max_players, status; bool lan; } online_game;

void online_get_view(online_view *v);
int online_games(online_game *out, int max);   // the browser: server rooms, then games on the local network
void online_browse(bool open);           // keep the browser's lists fresh while it shows
void online_refresh(void);
void online_join_game(int index);        // an index into online_games
void online_host_room(void);             // a room on the lobby server (the app restarts into it)
void online_host_lan(void);              // the built-in host on this network
void online_join_code(const char *code);
void online_quick(void);                 // the first open room on the server
void online_set_join_ip(const char *address);
void online_join_ip(void);
void online_leave(void);                 // back to playing alone (the app restarts)
void online_set_name(const char *name);
void online_set_voice_mode(int mode);
void online_set_volume(int percent);
void online_toggle_mute(void);
void online_talk_key(bool held);         // every VR frame: push to talk (a thumbstick click held)
bool online_push_to_talk(void);          // voice chat sends only while the talk key is held
int online_lag_ms(void);                 // a guest: how much later its own shots come back from the host (0: not a guest)

// A short message over the game (a partner joined / left, waiting for the host); false = nothing to show
bool online_toast(char *text, size_t n, unsigned *serial);
