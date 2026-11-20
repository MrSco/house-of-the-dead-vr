#pragma once
/* The lobby browser: asks an NMN2 server (upstream/docs/NETPLAY.md) for this game's rooms without joining any, the way the
 * server answers a stranger (DISCOVER -> ANNOUNCE, ROOMS padded to 400 bytes -> ROOMLIST). One small UDP socket, polled. */
#include <stdbool.h>
#include <stdint.h>

enum { NET_ROOMS_MAX=32 };
#define NET_GAME_ID "ha"     /* the arcade game (src/quest): its own rooms, never the PC game's */
typedef struct net_room { int id,version,state,players,max_players; char name[32],code[8]; } net_room;

/* Starts a query (resolves the address once per server; a refresh reuses it). False = the address does not resolve. */
bool net_rooms_query(const char *server,uint16_t game_version);
void net_rooms_poll(void);                       /* every frame while a query or the browser is open */
bool net_rooms_busy(void);                       /* waiting for the first answer */
bool net_rooms_answered(void);                   /* the server answered this query */
int  net_rooms_count(void);
const net_room *net_rooms_get(int i);
const char *net_rooms_server_name(void);
int  net_rooms_server_players(void);
void net_rooms_close(void);

/* Room codes: four characters that cannot be misread (no 0/O, 1/I, 2/Z, 5/S, 8/B). A hosted room is named "NAME PLATFORM CODE" (0.4.x: "NAME CODE"): the code is the last word. */
void net_room_code_make(char out[5]);
bool net_room_code_valid(const char *code);
extern const char net_room_code_chars[];
