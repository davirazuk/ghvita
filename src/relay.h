// Two devices far apart finding each other with no server of the game's own and no port forwarding: both connect out to
// free public MQTT brokers (the kind smart-home gadgets talk through) and meet on a topic named from a room code. What
// they say there is encrypted with a key made from the code (the code itself never goes out). p2p.c uses it to swap
// addresses and open a direct connection, and, when the networks won't allow one, to carry the game itself.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RELAY_MAX 1400          // a message's most bytes

bool relay_start(const char * room, bool host);  // the brokers looked up (in the background) and connected to
void relay_stop(void);
bool relay_on(void);
void relay_poll(void);                           // every frame
int relay_ready(void);                           // brokers connected and listening (0: none yet)
bool relay_send(const uint8_t * data, int len);  // to the other one, through every broker (or the one they're heard on)
int relay_recv(uint8_t * buf, int max);          // the next message from the other one, -1 none
bool relay_failed(void);                         // no broker could be reached
void relay_new_room(char * code, int size);      // a fresh room code ("K7Q2MX")
bool relay_room_ok(const char * text, char * code, int size);   // a typed code, tidied (false: not one)
