// Two devices playing together, with no server and no port forwarding: on the same Wi-Fi (or a phone's hotspot) they
// find each other by a broadcast every second, and talk over one UDP socket. A connection carries messages both ways
// in order and none lost (each numbered, acknowledged, sent again until it's acknowledged), and a few that may be
// lost (the live score). One other device at a time.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define P2P_PORT 47776          // the first of four tried (a second copy of the game on the same machine takes the next)
#define P2P_MAX_MSG 1200        // a message's most bytes

typedef struct {
    char name[32];              // its player
    char device[16];            // "PS Vita", "Android", "Windows", "Linux"
    int build, songs;
    uint32_t ip;                // network order
    uint16_t port;              // host order
    double seen;                // when it last said hello (seconds, plat_now_us's)
} P2PPeer;

enum { P2P_OFF, P2P_IDLE, P2P_CONNECTING, P2P_CONNECTED, P2P_LOST };

bool p2p_open(const char * name);          // the socket up, saying hello on the network; false: no network
void p2p_close(void);                      // the connection closed, the socket shut
bool p2p_is_open(void);
void p2p_poll(void);                       // every frame: what's come in, what's to send again
int p2p_peers(P2PPeer * out, int max);     // who's been heard on the network lately, newest first
bool p2p_connect(uint32_t ip, uint16_t port);
void p2p_hangup(void);                     // the connection closed (the socket stays open)
int p2p_state(void);
const char * p2p_other(void);              // the other one's name ("" when not connected)
const char * p2p_other_device(void);
bool p2p_hosting(void);                    // the other one connected to this one (this one leads)
// messages: type (the game's own numbers, 1-255) and up to P2P_MAX_MSG bytes. false: the queue is full (try later)
bool p2p_send(int type, const void * data, int len);
void p2p_send_fast(int type, const void * data, int len);  // may be lost (sent once)
int p2p_recv(int * type, void * buf, int max);             // the next message's length, -1 none
int p2p_backlog(void);                     // messages queued and not yet acknowledged
double p2p_clock(void);                    // seconds on the other one's clock now (its plat_now_us), as well as it's known
double p2p_ping_ms(void);
const char * p2p_my_address(char * buf, int size);   // "192.168.1.5" (what another device would type to reach this one)
bool p2p_parse_address(const char * text, uint32_t * ip, uint16_t * port);
// over the internet, still with no server of the game's own and no port forwarding: a public STUN server says how the
// internet sees this device, which is shown as a code ("ABCDE-FGHJK"). Each one types the other's code, and the two
// send to each other at once, which opens a way through both routers (most home routers; not some phone networks)
void p2p_code_begin(void);                          // asks (in the background) once the socket's open
int p2p_my_code(char * buf, int size);              // 1: buf has it, 0: still asking, -1: none (no internet)
bool p2p_parse_code(const char * text, uint32_t * ip, uint16_t * port);
bool p2p_connect_code(uint32_t ip, uint16_t port);  // like p2p_connect, trying for longer (they're typing too)
