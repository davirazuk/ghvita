// Two devices together over one UDP socket, no server (p2p.h). Every packet: "GV", version, kind, the connection's id,
// a sequence number, and what's been received of the other way (the next one expected, and a bit for each of the 32
// after it). A message that matters is kept until the other side has it and sent again after a while without word;
// they're handed on in order.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p2p.h"
#include "relay.h"
#include "plat.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#ifndef __vita__
#include <ifaddrs.h>
#include <net/if.h>
#endif
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

#ifdef __vita__
#include <psp2/net/netctl.h>
#endif

#ifndef GHV_BUILD
#define GHV_BUILD 0
#endif
#if defined(__vita__)
#define DEVICE "PS Vita"
#elif defined(__ANDROID__)
#define DEVICE "Android"
#elif defined(_WIN32)
#define DEVICE "Windows"
#else
#define DEVICE "Linux"
#endif

enum { K_HELLO = 1, K_CONNECT, K_ACCEPT, K_MSG, K_ACK, K_FAST, K_BYE, K_PING, K_PONG, K_BUSY, K_PUNCH };
#define RELAY_IP 0xFFFFFFFEu   // "from" a packet that came through the relay (no real address is this)
#define HEAD 20
#define WIN 256                 // messages out and not yet acknowledged, at most
#define QUEUE 1024              // messages waiting to go out, and come in and not yet read

typedef struct {
    uint32_t seq;
    uint8_t type, tries;
    uint16_t len;
    bool used, acked;
    double sent;
    uint8_t data[P2P_MAX_MSG];
} Slot;

typedef struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[P2P_MAX_MSG];
} Msg;

static struct {
    sock_t s;
    bool open;
    uint16_t port;
    uint32_t me_id;             // this run's (so its own hellos are ignored)
    char name[32];
    P2PPeer peers[16];
    uint32_t peer_id[16];       // each one's run (one heard at two addresses, a LAN's and loopback, is still one)
    double hello_t;
    // the connection
    int state;
    uint32_t ip, conn;
    uint16_t peer_port;
    bool hosting;
    char other[32], other_device[16];
    double heard, said, tried, began;
    // sending
    uint32_t next_seq, base;
    Slot out[WIN];
    Msg * wait;                 // QUEUE, a ring
    int wait_head, wait_n;
    double srtt, rto;
    // receiving
    uint32_t expect;
    Slot in[WIN];
    Msg * inbox;                // QUEUE, a ring
    int in_head, in_n;
    bool owe_ack;
    // the other one's clock
    double offset, best_rtt, ping_t;
    double patience;            // seconds a connect keeps trying (longer for a code: the other one's typing too)
    // this device as the internet sees it (a STUN server's answer): the code another device types to reach it
    int stun;                   // 0 not asked, 1 asking, 2 known, 3 no answer
    double stun_t, stun_began;
    uint8_t stun_tx[12];
    uint32_t pub_ip;            // network order
    uint16_t pub_port;          // host order
    // a room (far away), met in through the relay: the other one's addresses, tried directly before the relay's used
    int room;                   // 0 none, 1 hosting one, 2 joining one
    char room_code[8];
    double room_t, join_t, punch_t, punch_until, direct_until;
    uint32_t cand_ip[8];
    uint16_t cand_port[8];
    int ncand;
    bool relay, welcomed, busy_room;
} P;

static double now_s(void)
{
    return plat_now_us() / 1e6;
}

static double t_since(double t)
{
    return now_s() - t;
}

static void put32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t * p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void nonblocking(sock_t s)
{
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#elif defined(__vita__)
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

#ifndef __vita__
// tests: acting as a home router does, letting in only what comes from where this one has sent (GHV_P2P_NAT)
static struct {
    bool on, read;
    uint32_t ip[16];
    uint16_t port[16];
    int n;
} nat;

static void nat_read(void)
{
    if (!nat.read) {
        nat.read = true;
        nat.on = getenv("GHV_P2P_NAT") != NULL;
    }
}

static bool nat_lets_in(uint32_t ip, uint16_t port)
{
    nat_read();
    if (!nat.on)
        return true;
    for (int i = 0; i < nat.n; i++)
        if (nat.ip[i] == ip && nat.port[i] == port)
            return true;
    return false;
}

static void nat_sent(uint32_t ip, uint16_t port)
{
    nat_read();
    if (!nat.on || ip == INADDR_BROADCAST || (ntohl(ip) & 255) == 255)
        return;
    for (int i = 0; i < nat.n; i++)
        if (nat.ip[i] == ip && nat.port[i] == port)
            return;
    if (nat.n < 16) {
        nat.ip[nat.n] = ip;
        nat.port[nat.n++] = port;
    }
}
#endif

static void send_to(uint32_t ip, uint16_t port, const uint8_t * pkt, int len)
{
    if (ip == RELAY_IP) {
        uint8_t m[RELAY_MAX];
        if (len + 1 <= RELAY_MAX) {
            m[0] = 'P';
            memcpy(m + 1, pkt, (size_t)len);
            relay_send(m, len + 1);
        }
        return;
    }
#ifndef __vita__
    nat_sent(ip, port);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    sendto(P.s, (const char *)pkt, len, 0, (struct sockaddr *)&a, sizeof(a));
}

// what's been received of the other way: the next one expected, and which of the 32 after it are in
static void head(uint8_t * pkt, int kind, uint32_t seq)
{
    pkt[0] = 'G';
    pkt[1] = 'V';
    pkt[2] = 1;
    pkt[3] = (uint8_t)kind;
    put32(pkt + 4, P.conn);
    put32(pkt + 8, seq);
    put32(pkt + 12, P.expect);
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t q = P.expect + 1 + (uint32_t)i;
        const Slot * sl = &P.in[q % WIN];
        if (sl->used && sl->seq == q)
            bits |= 1u << i;
    }
    put32(pkt + 16, bits);
}

static void send_kind(int kind, const void * data, int len)
{
    uint8_t pkt[HEAD + 3 + P2P_MAX_MSG];
    head(pkt, kind, 0);
    if (len > 0)
        memcpy(pkt + HEAD, data, (size_t)len);
    send_to(P.ip, P.peer_port, pkt, HEAD + (len > 0 ? len : 0));
    P.said = now_s();
    if (kind == K_ACK || kind == K_MSG)
        P.owe_ack = false;
}

static void send_slot(Slot * sl)
{
    uint8_t pkt[HEAD + 3 + P2P_MAX_MSG];
    head(pkt, K_MSG, sl->seq);
    pkt[HEAD] = sl->type;
    pkt[HEAD + 1] = (uint8_t)sl->len;
    pkt[HEAD + 2] = (uint8_t)(sl->len >> 8);
    memcpy(pkt + HEAD + 3, sl->data, sl->len);
    send_to(P.ip, P.peer_port, pkt, HEAD + 3 + sl->len);
    sl->sent = now_s();
    sl->tries++;
    P.said = sl->sent;
    P.owe_ack = false;
}

static void who_am_i(uint8_t * p)
{
    // name[32], device[16], build, songs (filled by the caller), port
    memset(p, 0, 58);
    snprintf((char *)p, 32, "%s", P.name);
    snprintf((char *)p + 32, 16, "%s", DEVICE);
    put32(p + 48, (uint32_t)GHV_BUILD);
    p[56] = (uint8_t)P.port;
    p[57] = (uint8_t)(P.port >> 8);
}

static int my_songs;
void p2p_set_songs(int n)
{
    my_songs = n;
}

// this device's networks: each one's address and broadcast address (network order), the Wi-Fi's first. The Vita has
// one (its Wi-Fi); a phone or PC may have several (Wi-Fi, a hotspot it's giving, cable, a VPN)
static int my_networks(uint32_t * ip, uint32_t * bcast, int max)
{
    int n = 0;
#if defined(__vita__)
    SceNetCtlInfo a, m;
    if (max > 0 && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &a) >= 0 &&
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &m) >= 0) {
        uint32_t i = inet_addr(a.ip_address), k = inet_addr(m.netmask);
        if (i && i != INADDR_NONE) {
            ip[0] = i;
            bcast[0] = i | ~k;
            n = 1;
        }
    }
#elif defined(_WIN32)
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK)
        return 0;
    INTERFACE_INFO list[16];
    DWORD got = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, list, sizeof(list), &got, NULL, NULL) == 0) {
        for (int i = 0; i < (int)(got / sizeof(INTERFACE_INFO)) && n < max; i++) {
            if (!(list[i].iiFlags & IFF_UP) || (list[i].iiFlags & IFF_LOOPBACK))
                continue;
            uint32_t a = list[i].iiAddress.AddressIn.sin_addr.s_addr, k = list[i].iiNetmask.AddressIn.sin_addr.s_addr;
            if (!a)
                continue;
            ip[n] = a;
            bcast[n++] = a | ~k;
        }
    }
    closesocket(s);
#else
    struct ifaddrs * all;
    if (getifaddrs(&all) != 0)
        return 0;
    for (int pass = 0; pass < 2; pass++)            // the Wi-Fi first ("wlan0", "wlp2s0"), then the rest
        for (struct ifaddrs * f = all; f && n < max; f = f->ifa_next) {
            if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || !(f->ifa_flags & IFF_UP) ||
                (f->ifa_flags & IFF_LOOPBACK))
                continue;
            bool wifi = f->ifa_name && (f->ifa_name[0] == 'w' || !strncmp(f->ifa_name, "ap", 2) ||
                                        !strncmp(f->ifa_name, "swlan", 5));
            if (wifi != (pass == 0))
                continue;
            uint32_t a = ((struct sockaddr_in *)f->ifa_addr)->sin_addr.s_addr;
            uint32_t k = f->ifa_netmask ? ((struct sockaddr_in *)f->ifa_netmask)->sin_addr.s_addr : htonl(0xffffff00);
            ip[n] = a;
            bcast[n++] = a | ~k;
        }
    freeifaddrs(all);
#endif
    return n;
}

static void say_hello(void)
{
    uint8_t pkt[HEAD + 64];
    pkt[0] = 'G';
    pkt[1] = 'V';
    pkt[2] = 1;
    pkt[3] = K_HELLO;
    put32(pkt + 4, P.me_id);
    memset(pkt + 8, 0, 12);
    who_am_i(pkt + HEAD);
    put32(pkt + HEAD + 52, (uint32_t)my_songs);
    // the whole network's broadcast and each of this device's networks' own (Windows with two adapters sends the
    // first only out of one; a phone giving a hotspot has the hotspot's network as well as its own)
    uint32_t ip[8], bc[8];
    int nets = my_networks(ip, bc, 8);
    for (int k = 0; k < 4; k++) {
        send_to(INADDR_BROADCAST, (uint16_t)(P2P_PORT + k), pkt, HEAD + 58);
        for (int i = 0; i < nets; i++)
            send_to(bc[i], (uint16_t)(P2P_PORT + k), pkt, HEAD + 58);
#ifndef __vita__
        if (getenv("GHV_P2P_LOOPBACK"))     // tests: two copies on one machine
            send_to(htonl(0x7f000001), (uint16_t)(P2P_PORT + k), pkt, HEAD + 58);
#endif
    }
}

bool p2p_open(const char * name)
{
    snprintf(P.name, sizeof(P.name), "%s", name && name[0] ? name : "Player");
    if (P.open)
        return true;
    char err[120];
    if (!plat_net_up(err, sizeof(err)))
        return false;
#ifdef _WIN32
    static bool wsa;
    if (!wsa) {
        WSADATA w;
        wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }
#endif
    P.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (P.s == BAD_SOCK)
        return false;
    int on = 1;
    setsockopt(P.s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
    bool bound = false;
    for (int k = 0; k < 4 && !bound; k++) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons((uint16_t)(P2P_PORT + k));
        if (bind(P.s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            bound = true;
            P.port = (uint16_t)(P2P_PORT + k);
        }
    }
    if (!bound) {
        close_sock(P.s);
        return false;
    }
    nonblocking(P.s);
    if (!P.wait)
        P.wait = calloc(QUEUE, sizeof(Msg));
    if (!P.inbox)
        P.inbox = calloc(QUEUE, sizeof(Msg));
    if (!P.wait || !P.inbox) {
        close_sock(P.s);
        return false;
    }
    P.open = true;
    P.state = P2P_IDLE;
    P.me_id = (uint32_t)(plat_now_us() * 977) ^ (uint32_t)rand() << 8 ^ P.port;
    P.hello_t = -10;
    memset(P.peers, 0, sizeof(P.peers));
    memset(P.peer_id, 0, sizeof(P.peer_id));
    plat_log("p2p: open on port %d", P.port);
    return true;
}

bool p2p_is_open(void)
{
    return P.open;
}

static void reset_link(void)
{
    memset(P.out, 0, sizeof(P.out));
    memset(P.in, 0, sizeof(P.in));
    P.next_seq = P.base = P.expect = 1;
    P.wait_head = P.wait_n = 0;
    P.in_head = P.in_n = 0;
    P.srtt = 0.05;
    P.rto = 0.25;
    P.offset = 0;
    P.best_rtt = 1e9;
    P.owe_ack = false;
}

void p2p_hangup(void)
{
    if (P.state == P2P_CONNECTED || P.state == P2P_CONNECTING) {
        for (int i = 0; i < 3; i++)
            send_kind(K_BYE, NULL, 0);
    }
    P.state = P.open ? P2P_IDLE : P2P_OFF;
    P.other[0] = P.other_device[0] = 0;
    P.hosting = false;
    reset_link();
    if (P.room) {
        relay_stop();                   // (the room's over with the connection)
        P.room = 0;
    }
    P.relay = false;
}

void p2p_close(void)
{
    if (!P.open)
        return;
    p2p_hangup();
    close_sock(P.s);
    P.open = false;
    P.state = P2P_OFF;
    P.stun = 0;                 // a new socket goes out through the router at a new port: a new code
}

bool p2p_connect(uint32_t ip, uint16_t port)
{
    if (!P.open)
        return false;
    if (P.state == P2P_CONNECTED)
        p2p_hangup();
    reset_link();
    P.ip = ip;
    P.peer_port = port;
    P.conn = (uint32_t)(plat_now_us() * 131) ^ P.me_id ^ 0x5a5a1234u;
    P.state = P2P_CONNECTING;
    P.tried = -1;
    P.began = now_s();
    P.hosting = false;
    P.patience = 8;
    return true;
}

bool p2p_connect_code(uint32_t ip, uint16_t port)
{
    if (!p2p_connect(ip, port))
        return false;
    P.patience = 120;
    return true;
}

int p2p_state(void)
{
    return P.state;
}

const char * p2p_other(void)
{
    return P.state == P2P_CONNECTED ? P.other : "";
}

const char * p2p_other_device(void)
{
    return P.state == P2P_CONNECTED ? P.other_device : "";
}

bool p2p_hosting(void)
{
    return P.hosting;
}

int p2p_peers(P2PPeer * out, int max)
{
    int n = 0;
    double t = now_s();
    for (int i = 0; i < 16 && n < max; i++)
        if (P.peers[i].seen > 0 && t - P.peers[i].seen < 5)
            out[n++] = P.peers[i];
    return n;
}

bool p2p_send(int type, const void * data, int len)
{
    if (P.state != P2P_CONNECTED || len < 0 || len > P2P_MAX_MSG || P.wait_n >= QUEUE)
        return false;
    Msg * m = &P.wait[(P.wait_head + P.wait_n) % QUEUE];
    m->type = (uint8_t)type;
    m->len = (uint16_t)len;
    if (len)
        memcpy(m->data, data, (size_t)len);
    P.wait_n++;
    return true;
}

void p2p_send_fast(int type, const void * data, int len)
{
    if (P.state != P2P_CONNECTED || len < 0 || len > P2P_MAX_MSG)
        return;
    uint8_t b[3 + P2P_MAX_MSG];
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)len;
    b[2] = (uint8_t)(len >> 8);
    memcpy(b + 3, data, (size_t)len);
    send_kind(K_FAST, b, 3 + len);
}

static void to_inbox(int type, const uint8_t * data, int len)
{
    Msg * m = &P.inbox[(P.in_head + P.in_n) % QUEUE];
    m->type = (uint8_t)type;
    m->len = (uint16_t)len;
    memcpy(m->data, data, (size_t)len);
    P.in_n++;
}

int p2p_recv(int * type, void * buf, int max)
{
    if (!P.in_n)
        return -1;
    Msg * m = &P.inbox[P.in_head];
    int n = m->len < max ? m->len : max;
    *type = m->type;
    memcpy(buf, m->data, (size_t)n);
    P.in_head = (P.in_head + 1) % QUEUE;
    P.in_n--;
    return n;
}

int p2p_backlog(void)
{
    return P.wait_n + (int)(P.next_seq - P.base);
}

double p2p_clock(void)
{
    return now_s() + P.offset;
}

double p2p_ping_ms(void)
{
    return P.best_rtt < 1e8 ? P.best_rtt * 1000 : P.srtt * 1000;
}

// the other side has these: the ones before `ack`, and the 32 after as its bits say
static void take_ack(uint32_t ack, uint32_t bits)
{
    double t = now_s();
    for (uint32_t q = P.base; q != P.next_seq; q++) {
        Slot * sl = &P.out[q % WIN];
        if (!sl->used || sl->acked)
            continue;
        bool got = (int32_t)(ack - q) > 0;
        if (!got && (int32_t)(q - ack) >= 1 && q - ack <= 32)
            got = bits >> (q - ack - 1) & 1;
        if (got) {
            sl->acked = true;
            if (sl->tries == 1) {
                double rtt = t - sl->sent;
                P.srtt = P.srtt * 0.875 + rtt * 0.125;
                P.rto = P.srtt * 3 + 0.03;
                P.rto = P.rto < 0.08 ? 0.08 : P.rto > 1.5 ? 1.5 : P.rto;
            }
        }
    }
    while (P.base != P.next_seq && P.out[P.base % WIN].acked) {
        P.out[P.base % WIN].used = false;
        P.base++;
    }
}

static void got_msg(uint32_t seq, const uint8_t * body, int blen)
{
    P.owe_ack = true;
    if (blen < 3)
        return;
    int type = body[0], len = body[1] | body[2] << 8;
    if (len > blen - 3 || len > P2P_MAX_MSG)
        return;
    if ((int32_t)(seq - P.expect) < 0 || seq - P.expect >= WIN)
        return;                         // had it already, or too far ahead
    Slot * sl = &P.in[seq % WIN];
    if (!(sl->used && sl->seq == seq)) {
        sl->used = true;
        sl->seq = seq;
        sl->type = (uint8_t)type;
        sl->len = (uint16_t)len;
        memcpy(sl->data, body + 3, (size_t)len);
    }
    // in order, as far as they go (and as far as the inbox has room: what doesn't fit is acknowledged later)
    while (P.in_n < QUEUE) {
        Slot * h = &P.in[P.expect % WIN];
        if (!(h->used && h->seq == P.expect))
            break;
        to_inbox(h->type, h->data, h->len);
        h->used = false;
        P.expect++;
    }
}

static void got_hello(uint32_t from_ip, const uint8_t * p, int len, uint32_t id)
{
    if (len < 58 || id == P.me_id)
        return;
    double t = now_s();
    uint16_t port = (uint16_t)(p[56] | p[57] << 8);
    int at = -1, old = 0;
    for (int i = 0; i < 16; i++) {
        if (P.peer_id[i] == id && P.peers[i].seen > 0) {
            at = i;
            break;
        }
        if (P.peers[i].seen < P.peers[old].seen)
            old = i;
    }
    if (at < 0)
        at = old;
    P2PPeer * w = &P.peers[at];
    snprintf(w->name, sizeof(w->name), "%.31s", (const char *)p);
    snprintf(w->device, sizeof(w->device), "%.15s", (const char *)p + 32);
    w->build = (int)get32(p + 48);
    w->songs = (int)get32(p + 52);
    // a peer heard on another adapter too keeps the address it was first heard at (it's not flipping in the list)
    if (P.peer_id[at] != id || t - w->seen > 3) {
        w->ip = from_ip;
        w->port = port;
    }
    P.peer_id[at] = id;
    w->seen = t;
}

// ---- the internet: STUN (RFC 5389). A public server says what address and port this socket's packets come from once
// they're out through the router; that's the code. Two devices that each send to the other's at once open a way through
// both routers (nothing listens for it: each router lets the answer in because its own device sent there first)

#define STUN_SERVERS 4
static uint32_t stun_ip[STUN_SERVERS];          // network order
static uint16_t stun_port[STUN_SERVERS];
static volatile int stun_found = -1;            // -1 looking them up, then how many

static int stun_lookup(void * arg)
{
    (void)arg;
    static const char * const servers[] = {"stun.l.google.com:19302", "stun.cloudflare.com:3478",
                                           "stun1.l.google.com:19302", "stun.nextcloud.com:443"};
    char test[64] = "";
#ifndef __vita__
    if (getenv("GHV_STUN"))             // tests: a stand-in server
        snprintf(test, sizeof(test), "%s", getenv("GHV_STUN"));
#endif
    int n = 0;
    for (int i = 0; i < STUN_SERVERS && n < STUN_SERVERS; i++) {
        char host[64];
        snprintf(host, sizeof(host), "%s", test[0] ? test : servers[i]);
        char * colon = strchr(host, ':');
        int port = colon ? atoi(colon + 1) : 3478;
        if (colon)
            *colon = 0;
        uint32_t ip = inet_addr(host);
        if (ip == INADDR_NONE) {
            // (getaddrinfo: the relay's lookup may run in its own thread at the same moment; gethostbyname shares one
            // buffer between them)
            struct addrinfo hints, * res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
                continue;
            ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
        stun_ip[n] = ip;
        stun_port[n++] = (uint16_t)port;
        if (test[0])
            break;
    }
    stun_found = n;
    return 0;
}

static void stun_ask(double t)
{
    if (P.stun != 1)
        return;
    if (t - P.stun_began > 12) {
        P.stun = 3;
        plat_log("p2p: no answer from the internet (STUN)");
        return;
    }
    if (stun_found < 0 || t - P.stun_t < 0.7)
        return;
    P.stun_t = t;
    uint8_t q[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    memcpy(q + 8, P.stun_tx, 12);
    for (int i = 0; i < stun_found; i++)
        send_to(stun_ip[i], stun_port[i], q, 20);
}

static void stun_answer(const uint8_t * p, int n)
{
    if (n < 20 || p[4] != 0x21 || p[5] != 0x12 || p[6] != 0xA4 || p[7] != 0x42 || memcmp(p + 8, P.stun_tx, 12))
        return;
    int len = p[2] << 8 | p[3], at = 20;
    if (len + 20 > n)
        len = n - 20;
    while (at + 4 <= 20 + len) {
        int type = p[at] << 8 | p[at + 1], alen = p[at + 2] << 8 | p[at + 3];
        const uint8_t * v = p + at + 4;
        if (at + 4 + alen > n)
            break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t port = (uint16_t)(v[2] << 8 | v[3]);
            uint8_t a[4] = {v[4], v[5], v[6], v[7]};
            if (type == 0x0020) {               // XOR-MAPPED-ADDRESS: mixed with the magic number
                port ^= 0x2112;
                a[0] ^= 0x21, a[1] ^= 0x12, a[2] ^= 0xA4, a[3] ^= 0x42;
            }
            memcpy(&P.pub_ip, a, 4);
            P.pub_port = port;
            if (P.stun != 2)
                plat_log("p2p: on the internet as %u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], port);
            P.stun = 2;
            if (type == 0x0020)
                return;
        }
        at += 4 + ((alen + 3) & ~3);
    }
}

void p2p_code_begin(void)
{
    if (!P.open || P.stun == 1 || P.stun == 2)
        return;
    static bool looked;
    if (!looked || stun_found == 0) {   // (none found last time: no internet then, maybe there is now)
        looked = true;
        stun_found = -1;
        plat_thread("stun", stun_lookup, NULL, PLAT_BACKGROUND);
    }
    for (int i = 0; i < 12; i++)
        P.stun_tx[i] = (uint8_t)(rand() ^ (int)(plat_now_us() >> (i % 4 * 3)));
    P.stun = 1;
    P.stun_began = now_s();
    P.stun_t = -10;
}

// a code: the address and port, and two bits that check them, as ten letters and digits (none of 0 O 1 I)
static const char CODE_ABC[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

static int code_check(uint64_t v)
{
    int sum = 0;
    for (int i = 0; i < 6; i++)
        sum += (int)(v >> (i * 8) & 255) * (i + 1);
    return sum & 3;
}

int p2p_my_code(char * buf, int size)
{
    snprintf(buf, (size_t)size, "%s", "");
    if (P.stun != 2)
        return P.stun == 1 ? 0 : -1;
    uint64_t v = (uint64_t)ntohl(P.pub_ip) << 16 | P.pub_port;
    v |= (uint64_t)code_check(v) << 48;
    char c[12];
    int k = 0;
    for (int i = 9; i >= 0; i--) {
        c[k++] = CODE_ABC[v >> (i * 5) & 31];
        if (i == 5)
            c[k++] = '-';
    }
    c[k] = 0;
    snprintf(buf, (size_t)size, "%s", c);
    return 1;
}

bool p2p_parse_code(const char * text, uint32_t * ip, uint16_t * port)
{
    uint64_t v = 0;
    int got = 0;
    for (const char * t = text; *t; t++) {
        char ch = *t >= 'a' && *t <= 'z' ? (char)(*t - 32) : *t;
        if (ch == '-' || ch == ' ')
            continue;
        const char * at = strchr(CODE_ABC, ch);
        if (!at || !ch || got >= 10)
            return false;
        v = v << 5 | (uint64_t)(at - CODE_ABC);
        got++;
    }
    if (got != 10 || (int)(v >> 48) != code_check(v & 0xffffffffffffull))
        return false;
    *ip = htonl((uint32_t)(v >> 16));
    *port = (uint16_t)v;
    return *ip != 0 && *port != 0;
}

// one packet in, from the network (fip, fport) or through the relay (RELAY_IP)
static void take_packet(const uint8_t * pkt, int n, uint32_t fip, uint16_t fport, double t)
{
    if (pkt[0] != 'G' || pkt[1] != 'V' || pkt[2] != 1)
        return;
    int kind = pkt[3];
    uint32_t id = get32(pkt + 4), seq = get32(pkt + 8), ack = get32(pkt + 12), bits = get32(pkt + 16);
    const uint8_t * body = pkt + HEAD;
    int blen = n - HEAD;
    if (kind == K_HELLO) {
        got_hello(fip, body, blen, id);
        return;
    }
    if (kind == K_PUNCH)
        return;                         // (only there to open the way through the routers)
    if (kind == K_CONNECT) {
        if (blen < 58)
            return;
        if (P.state == P2P_CONNECTED && id == P.conn && fip == RELAY_IP && !P.relay) {
            // the same connection through the relay now (the guest gave up on the direct way): that way. (Not another
            // of its own addresses: the guest tries them all at once, and stays with the one that answered first)
            P.ip = fip;
            P.peer_port = fport;
            P.relay = true;
        }
        if (P.state == P2P_CONNECTED && id == P.conn && fip != P.ip)
            return;                     // (a try of the same connection by another of its addresses: already in)
        if (P.state == P2P_CONNECTED && !(id == P.conn && fip == P.ip)) {
            uint8_t r[HEAD];
            memset(r, 0, sizeof(r));
            r[0] = 'G', r[1] = 'V', r[2] = 1, r[3] = K_BUSY;
            put32(r + 4, id);
            send_to(fip, fport, r, HEAD);
            return;
        }
        // both connecting to each other at once (two codes typed): the one whose try has the lower number goes on
        // trying, and the other takes it (and hosts)
        if (P.state == P2P_CONNECTING && fip == P.ip && P.conn < id)
            return;
        if (P.state != P2P_CONNECTED) {
            // someone wants to play: this one hosts
            reset_link();
            P.ip = fip;
            P.peer_port = fport;
            P.conn = id;
            P.state = P2P_CONNECTED;
            P.hosting = true;
            P.relay = fip == RELAY_IP;
            snprintf(P.other, sizeof(P.other), "%.31s", (const char *)body);
            snprintf(P.other_device, sizeof(P.other_device), "%.15s", (const char *)body + 32);
            P.heard = t;
            plat_log("p2p: %s (%s) connected%s", P.other, P.other_device, P.relay ? " through the relay" : "");
        }
        uint8_t me[64];
        who_am_i(me);
        send_kind(K_ACCEPT, me, 58);
        return;
    }
    if (kind == K_ACCEPT && P.state == P2P_CONNECTING && id == P.conn && fip != P.ip && fip != RELAY_IP)
        for (int i = 0; i < P.ncand; i++)
            if (P.cand_ip[i] == fip) {
                P.ip = fip;             // (one of the room's addresses answered: that one)
                P.peer_port = fport;
                P.relay = false;
            }
    if (id != P.conn || fip != P.ip)
        return;                   // not this connection's
    P.heard = t;
    if (kind == K_ACCEPT && P.state == P2P_CONNECTING) {
        if (blen >= 58) {
            snprintf(P.other, sizeof(P.other), "%.31s", (const char *)body);
            snprintf(P.other_device, sizeof(P.other_device), "%.15s", (const char *)body + 32);
        }
        P.state = P2P_CONNECTED;
        P.peer_port = fport;
        plat_log("p2p: connected to %s (%s)%s", P.other, P.other_device, P.relay ? " through the relay" : "");
        return;
    }
    if (kind == K_BUSY && P.state == P2P_CONNECTING) {
        P.state = P2P_LOST;
        return;
    }
    if (P.state != P2P_CONNECTED)
        return;
    take_ack(ack, bits);
    if (kind == K_MSG)
        got_msg(seq, body, blen);
    else if (kind == K_FAST && blen >= 3) {
        int len = body[1] | body[2] << 8;
        if (len <= blen - 3 && P.in_n < QUEUE)
            to_inbox(body[0], body + 3, len);
    } else if (kind == K_BYE) {
        plat_log("p2p: %s left", P.other);
        P.state = P2P_LOST;
    } else if (kind == K_PING && blen >= 8) {
        uint8_t r[16];
        memcpy(r, body, 8);
        double mine = t;
        memcpy(r + 8, &mine, 8);
        send_kind(K_PONG, r, 16);
    } else if (kind == K_PONG && blen >= 16) {
        double t0, theirs;
        memcpy(&t0, body, 8);
        memcpy(&theirs, body + 8, 8);
        double rtt = t - t0;
        if (rtt >= 0 && rtt < 2 && rtt <= P.best_rtt * 1.5 + 0.002) {
            // the clock from the quickest answers (the one-way time taken as half the round trip)
            if (rtt < P.best_rtt)
                P.best_rtt = rtt;
            P.offset = theirs - (t0 + t) / 2;
        }
    }
}


// ---- rooms: meeting far away through the relay (relay.h), then talking directly if the networks allow it

// this device's addresses as the other one would try them: the internet's view of it (STUN), then its own networks'
static int my_addresses(uint8_t * out)
{
    int n = 0;
    uint32_t ip[4], bc[4];
    if (P.stun == 2) {
        memcpy(out, &P.pub_ip, 4);
        out[4] = (uint8_t)(P.pub_port >> 8), out[5] = (uint8_t)P.pub_port;
        n++;
    }
    int k = my_networks(ip, bc, 4);
    for (int i = 0; i < k && n < 6; i++, n++) {
        memcpy(out + n * 6, &ip[i], 4);
        out[n * 6 + 4] = (uint8_t)(P.port >> 8), out[n * 6 + 5] = (uint8_t)P.port;
    }
#ifndef __vita__
    if (getenv("GHV_P2P_LOOPBACK") && n < 6) {      // tests: two copies on one machine
        uint32_t lo = htonl(0x7f000001);
        memcpy(out + n * 6, &lo, 4);
        out[n * 6 + 4] = (uint8_t)(P.port >> 8), out[n * 6 + 5] = (uint8_t)P.port;
        n++;
    }
#endif
    return n;
}

// 'J' (the guest: here I am) and 'W' (the host: welcome): who, and where to reach them
static void say_room(char type)
{
    uint8_t m[1 + 58 + 1 + 6 * 6];
    m[0] = (uint8_t)type;
    who_am_i(m + 1);
    int n = my_addresses(m + 60);
    m[59] = (uint8_t)n;
    relay_send(m, 60 + n * 6);
}

static void heard_room(const uint8_t * m, int n)
{
    // the other one's addresses
    P.ncand = 0;
    int k = n >= 60 ? m[59] : 0;
    for (int i = 0; i < k && i < 8 && 60 + i * 6 + 6 <= n; i++) {
        memcpy(&P.cand_ip[P.ncand], m + 60 + i * 6, 4);
        P.cand_port[P.ncand++] = (uint16_t)(m[60 + i * 6 + 4] << 8 | m[60 + i * 6 + 5]);
    }
    snprintf(P.other, sizeof(P.other), "%.31s", (const char *)m + 1);
    snprintf(P.other_device, sizeof(P.other_device), "%.15s", (const char *)m + 33);
}

static void room_poll(double t)
{
    if (!P.room)
        return;
    relay_poll();
    uint8_t m[RELAY_MAX];
    int n;
    while ((n = relay_recv(m, sizeof(m))) > 0) {
        if (m[0] == 'P' && n > 1)
            take_packet(m + 1, n - 1, RELAY_IP, 0, t);
        else if (m[0] == 'J' && P.room == 1 && n >= 60) {
            if (P.state == P2P_CONNECTED) {
                uint8_t b = 'B';
                relay_send(&b, 1);          // (this room has its two)
                continue;
            }
            heard_room(m, n);
            plat_log("p2p: %s (%s) is in the room, %d addresses", P.other, P.other_device, P.ncand);
            say_room('W');
            P.punch_until = t + 10;
        } else if (m[0] == 'W' && P.room == 2 && n >= 60 && !P.welcomed && P.state != P2P_CONNECTED) {
            heard_room(m, n);
            P.welcomed = true;
            plat_log("p2p: in %s's room, %d addresses", P.other, P.ncand);
            // try them directly; the relay if none answers
            reset_link();
            P.conn = (uint32_t)(plat_now_us() * 131) ^ P.me_id ^ 0x5a5a1234u;
            P.state = P2P_CONNECTING;
            P.hosting = false;
            P.tried = -1;
            P.began = t;
            P.patience = 45;
            P.relay = P.ncand == 0;
            P.ip = P.relay ? RELAY_IP : P.cand_ip[0];
            P.peer_port = P.relay ? 0 : P.cand_port[0];
            P.direct_until = t + 6;
        } else if (m[0] == 'B' && P.room == 2 && P.state != P2P_CONNECTED)
            P.busy_room = true;
    }
    // the guest: here I am, again now and then until welcomed (once the internet's view of this one's known, or soon)
    if (P.room == 2 && !P.welcomed && relay_ready() && (P.stun != 1 || t - P.room_t > 4) && t - P.join_t > 2) {
        P.join_t = t;
        say_room('J');
    }
    // the host: knocking on the guest's addresses, so its router lets the guest's knocks in
    if (P.room == 1 && P.state != P2P_CONNECTED && t < P.punch_until && t - P.punch_t > 0.25) {
        P.punch_t = t;
        uint8_t k[HEAD];
        head(k, K_PUNCH, 0);
        for (int i = 0; i < P.ncand; i++)
            send_to(P.cand_ip[i], P.cand_port[i], k, HEAD);
    }
}

bool p2p_room_host(char * code, int size)
{
    if (!P.open)
        return false;
    p2p_hangup();
    relay_new_room(P.room_code, sizeof(P.room_code));
    snprintf(code, (size_t)size, "%s", P.room_code);
    P.room = 1;
    P.room_t = now_s();
    P.ncand = 0;
    P.punch_until = 0;
    P.relay = P.welcomed = P.busy_room = false;
    p2p_code_begin();                   // (the internet's view of this one: the guest tries it first)
    relay_start(P.room_code, true);
    plat_log("p2p: hosting a room");
    return true;
}

bool p2p_room_join(const char * typed)
{
    char code[8];
    if (!P.open || !relay_room_ok(typed, code, sizeof(code)))
        return false;
    p2p_hangup();
    snprintf(P.room_code, sizeof(P.room_code), "%s", code);
    P.room = 2;
    P.room_t = now_s();
    P.join_t = -10;
    P.ncand = 0;
    P.relay = P.welcomed = P.busy_room = false;
    p2p_code_begin();
    relay_start(P.room_code, false);
    plat_log("p2p: joining a room");
    return true;
}

int p2p_room(char * code, int size)
{
    if (code)
        snprintf(code, (size_t)size, "%s", P.room ? P.room_code : "");
    return P.room;
}

const char * p2p_room_status(void)
{
    if (!P.room)
        return "";
    if (P.state == P2P_CONNECTED)
        return P.relay ? "Connected (through the relay)" : "Connected (directly)";
    if (relay_failed())
        return "Couldn't get online: is this device on the internet?";
    if (!relay_ready())
        return "Getting online...";
    if (P.room == 1)
        return P.ncand ? "They're here: connecting..." : "Waiting for the other player to join...";
    if (P.busy_room)
        return "That room already has two players";
    if (!P.welcomed)
        return t_since(P.room_t) > 25 ? "No one's hosting that room (check the code)" : "Looking for the room...";
    return P.relay ? "Connecting through the relay..." : "Reaching them directly...";
}

bool p2p_relayed(void)
{
    return P.state == P2P_CONNECTED && P.relay;
}

void p2p_poll(void)
{
    if (!P.open)
        return;
    double t = now_s();
    room_poll(t);
    uint8_t pkt[HEAD + 3 + P2P_MAX_MSG + 16];
    for (int budget = 0; budget < 600; budget++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(P.s, (char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
        if (n < HEAD)
            break;
#ifndef __vita__
        if (pkt[3] != K_HELLO && !nat_lets_in(from.sin_addr.s_addr, ntohs(from.sin_port)))
            continue;
        static int no_udp = -1;         // tests: networks that never let two devices talk directly (the relay's needed)
        if (no_udp < 0)
            no_udp = getenv("GHV_P2P_NOUDP") != NULL;
        if (no_udp && pkt[0] == 'G')
            continue;
#endif
        if (pkt[0] == 0x01 && pkt[1] == 0x01) {
            stun_answer(pkt, n);
            continue;
        }
        take_packet(pkt, n, from.sin_addr.s_addr, ntohs(from.sin_port), t);
    }
    if (P.state == P2P_CONNECTING) {
        if (t - P.tried > 0.25) {
            P.tried = t;
            uint8_t me[64];
            who_am_i(me);
            if (P.room == 2 && !P.relay)
                for (int i = 0; i < P.ncand; i++) {
                    uint8_t c[HEAD + 64];
                    head(c, K_CONNECT, 0);
                    memcpy(c + HEAD, me, 58);
                    send_to(P.cand_ip[i], P.cand_port[i], c, HEAD + 58);
                }
            else
                send_kind(K_CONNECT, me, 58);
        }
        if (P.room == 2 && !P.relay && t > P.direct_until) {
            // the networks won't let the two talk directly: the game through the relay instead
            P.relay = true;
            P.ip = RELAY_IP;
            P.peer_port = 0;
            P.tried = -1;
            plat_log("p2p: no direct way to them: through the relay");
        }
        if (t - P.began > P.patience)
            P.state = P2P_LOST;
    }
    if (P.state == P2P_CONNECTED) {
        // what was sent and not acknowledged in time, again (a few at a time: a burst would just be lost again)
        int resent = 0;
        for (uint32_t q = P.base; q != P.next_seq && resent < 48; q++) {
            Slot * sl = &P.out[q % WIN];
            if (sl->used && !sl->acked && t - sl->sent > P.rto * (sl->tries > 3 ? 2 : 1)) {
                send_slot(sl);
                resent++;
            }
        }
        // then what's waiting, as far as the window lets it
        while (P.wait_n && P.next_seq - P.base < (P.relay ? 24u : (uint32_t)WIN)) {     // (the relay: less in flight)
            Msg * m = &P.wait[P.wait_head];
            Slot * sl = &P.out[P.next_seq % WIN];
            sl->used = true;
            sl->acked = false;
            sl->seq = P.next_seq++;
            sl->type = m->type;
            sl->len = m->len;
            sl->tries = 0;
            memcpy(sl->data, m->data, m->len);
            P.wait_head = (P.wait_head + 1) % QUEUE;
            P.wait_n--;
            send_slot(sl);
        }
        if (P.owe_ack || t - P.said > 0.3)
            send_kind(K_ACK, NULL, 0);
        if (t - P.ping_t > (P.best_rtt > 1e8 ? 0.2 : 1.0)) {
            P.ping_t = t;
            send_kind(K_PING, &t, 8);
        }
        if (t - P.heard > 10) {
            plat_log("p2p: lost %s", P.other);
            P.state = P2P_LOST;
        }
    }
    if ((P.state == P2P_IDLE || P.state == P2P_CONNECTING || P.state == P2P_LOST) && t - P.hello_t > 1) {
        P.hello_t = t;
        say_hello();
    }
    stun_ask(t);
}

const char * p2p_my_address(char * buf, int size)
{
    // the address the way out goes from: a UDP socket "connected" to a public address sends nothing, and says which
    // of this machine's addresses it would use
    snprintf(buf, (size_t)size, "?");
#ifdef __vita__
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0 && info.ip_address[0]) {
        snprintf(buf, (size_t)size, "%s", info.ip_address);
        return buf;
    }
#endif
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK)
        return buf;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    a.sin_port = htons(53);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        struct sockaddr_in me;
        socklen_t ml = sizeof(me);
        if (getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_addr.s_addr) {
            uint32_t ip = ntohl(me.sin_addr.s_addr);
            snprintf(buf, (size_t)size, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
        }
    }
    close_sock(s);
    if (buf[0] == '?') {
        // no way out to the internet (a hotspot with no data, a Wi-Fi with no internet): the network's own address
        uint32_t ip[1], bc[1];
        if (my_networks(ip, bc, 1)) {
            uint32_t h = ntohl(ip[0]);
            snprintf(buf, (size_t)size, "%u.%u.%u.%u", h >> 24, h >> 16 & 255, h >> 8 & 255, h & 255);
        }
    }
    return buf;
}

bool p2p_parse_address(const char * text, uint32_t * ip, uint16_t * port)
{
    unsigned a, b, c, d, p = P2P_PORT;
    int n = sscanf(text, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || p > 65535)
        return false;
    *ip = htonl(a << 24 | b << 16 | c << 8 | d);
    *port = (uint16_t)p;
    return true;
}
