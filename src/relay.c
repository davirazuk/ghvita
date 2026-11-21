// relay.h: meeting far away through public MQTT brokers (MQTT 3.1.1 over plain TCP, QoS 0), messages sealed with
// ChaCha20 under a key from the room code. Each side listens on its own topic (h for the host, g for the guest).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "relay.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAENOTCONN || \
                       WSAGetLastError() == WSAEINPROGRESS || WSAGetLastError() == WSAEALREADY)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == ENOTCONN || \
                       errno == EALREADY)
#endif

// ------------------------------------------------------------------------------------------------ SHA-256, ChaCha20

static uint32_t ror(uint32_t x, int n)
{
    return x >> n | x << (32 - n);
}

static void sha256(const uint8_t * msg, size_t len, uint8_t out[32])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t total = ((len + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + i;
            blk[i] = p < len ? msg[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (int i = 0; i < 8; i++)
                blk[56 + i] = (uint8_t)((uint64_t)len * 8 >> (56 - 8 * i));
        uint32_t w[64], a[8];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[i * 4] << 24 | blk[i * 4 + 1] << 16 | blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3;
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        memcpy(a, h, sizeof(a));
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = a[7] + (ror(a[4], 6) ^ ror(a[4], 11) ^ ror(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k[i] + w[i];
            uint32_t t2 = (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
            memmove(a + 1, a, 7 * sizeof(uint32_t));
            a[4] += t1;
            a[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++)
            h[i] += a[i];
    }
    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

#define QR(a, b, c, d) (a += b, d = ror(d ^ a, 16), c += d, b = ror(b ^ c, 20), a += b, d = ror(d ^ a, 24), \
                        c += d, b = ror(b ^ c, 25))

// ChaCha20 (RFC 8439): data ^= the key stream for this nonce
static void chacha20(const uint8_t key[32], const uint8_t nonce[12], uint8_t * data, int len)
{
    uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++)
        st[4 + i] = key[i * 4] | key[i * 4 + 1] << 8 | key[i * 4 + 2] << 16 | (uint32_t)key[i * 4 + 3] << 24;
    for (int i = 0; i < 3; i++)
        st[13 + i] = nonce[i * 4] | nonce[i * 4 + 1] << 8 | nonce[i * 4 + 2] << 16 | (uint32_t)nonce[i * 4 + 3] << 24;
    for (uint32_t block = 1; len > 0; block++) {
        uint32_t x[16];
        st[12] = block;
        memcpy(x, st, sizeof(x));
        for (int r = 0; r < 10; r++) {
            QR(x[0], x[4], x[8], x[12]), QR(x[1], x[5], x[9], x[13]), QR(x[2], x[6], x[10], x[14]);
            QR(x[3], x[7], x[11], x[15]), QR(x[0], x[5], x[10], x[15]), QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8], x[13]), QR(x[3], x[4], x[9], x[14]);
        }
        for (int i = 0; i < 64 && len > 0; i++, len--)
            *data++ ^= (uint8_t)((x[i / 4] + st[i / 4]) >> (8 * (i % 4)));
    }
}

// ------------------------------------------------------------------------------------------------ the brokers

#define BROKERS 3
#define BUF 8192
#define QN 256
static const char * const broker_names[BROKERS] = {"broker.hivemq.com", "broker.emqx.io", "test.mosquitto.org"};

typedef struct {
    sock_t s;
    int state;                  // 0 not yet, 1 connecting, 2 CONNECT sent, 3 listening, -1 given up
    uint8_t rx[BUF], tx[BUF];
    int rn, tn;
    double began, ping;
    int tries;
} Broker;

static struct {
    bool on, host;
    char mine[64], theirs[64];  // topics: where this one listens, where the other does
    uint8_t key[32];
    Broker b[BROKERS];
    int heard_on;               // the broker the other one was first heard through (-1 none): sent only there then
    uint8_t q[QN][RELAY_MAX];
    int qlen[QN], qh, qn;
    uint32_t seen[256];         // the last messages' nonces (one delivered by two brokers counts once)
    int seen_at;
} R;

static volatile int dns_state;  // 0 not looked up, 1 looking, 2 done
static uint32_t broker_ip[BROKERS];
static uint16_t broker_port[BROKERS];

static double now_s(void)
{
    return plat_now_us() / 1e6;
}

static int lookup(void * arg)
{
    (void)arg;
    char test[96] = "";
#ifndef __vita__
    if (getenv("GHV_MQTT"))                 // tests: a broker of their own ("127.0.0.1:1883")
        snprintf(test, sizeof(test), "%s", getenv("GHV_MQTT"));
#endif
    for (int i = 0; i < BROKERS; i++) {
        char host[96];
        snprintf(host, sizeof(host), "%s", test[0] ? test : broker_names[i]);
        char * colon = strchr(host, ':');
        broker_port[i] = (uint16_t)(colon ? atoi(colon + 1) : 1883);
        if (colon)
            *colon = 0;
        uint32_t ip = inet_addr(host);
        if (ip == INADDR_NONE) {
            // (getaddrinfo, not gethostbyname: the STUN lookup may be running in its own thread at the same moment,
            // and gethostbyname's answer is one shared buffer)
            struct addrinfo hints, * res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            ip = 0;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(res);
            }
        }
        broker_ip[i] = ip == INADDR_NONE ? 0 : ip;
        if (test[0] && i == 0) {            // (one test broker)
            for (int k = 1; k < BROKERS; k++)
                broker_ip[k] = 0;
            break;
        }
    }
    dns_state = 2;
    return 0;
}

static void put_len(Broker * b, int n)      // MQTT's remaining length
{
    do {
        uint8_t c = n % 128;
        n /= 128;
        b->tx[b->tn++] = (uint8_t)(c | (n ? 128 : 0));
    } while (n);
}

static void put_str(Broker * b, const char * s, int n)
{
    b->tx[b->tn++] = (uint8_t)(n >> 8);
    b->tx[b->tn++] = (uint8_t)n;
    memcpy(b->tx + b->tn, s, (size_t)n);
    b->tn += n;
}

static void flush(Broker * b)
{
    while (b->tn > 0) {
#ifdef MSG_NOSIGNAL
        int n = (int)send(b->s, (const char *)b->tx, b->tn, MSG_NOSIGNAL);    // (a dropped line isn't a crash)
#else
        int n = (int)send(b->s, (const char *)b->tx, b->tn, 0);
#endif
        if (n <= 0) {
            if (n < 0 && WOULD_BLOCK())
                return;                     // (later: the connection's still opening, or the line's full)
            b->state = -1;
            return;
        }
        memmove(b->tx, b->tx + n, (size_t)(b->tn - n));
        b->tn -= n;
    }
}

static void mqtt_connect(Broker * b)
{
    char id[24];
    snprintf(id, sizeof(id), "ghv%08x%04x", (unsigned)rand() ^ (unsigned)plat_now_us(), (unsigned)rand() & 0xffff);
    int idn = (int)strlen(id);
    b->tx[b->tn++] = 0x10;
    put_len(b, 10 + 2 + idn);
    put_str(b, "MQTT", 4);
    b->tx[b->tn++] = 4;                     // 3.1.1
    b->tx[b->tn++] = 2;                     // a clean session
    b->tx[b->tn++] = 0;
    b->tx[b->tn++] = 60;                    // keep alive, seconds
    put_str(b, id, idn);
}

static void mqtt_subscribe(Broker * b)
{
    int tn = (int)strlen(R.mine);
    b->tx[b->tn++] = 0x82;
    put_len(b, 2 + 2 + tn + 1);
    b->tx[b->tn++] = 0;
    b->tx[b->tn++] = 1;                     // the packet's number
    put_str(b, R.mine, tn);
    b->tx[b->tn++] = 0;                     // QoS 0
}

static void mqtt_publish(Broker * b, const uint8_t * data, int len)
{
    int tn = (int)strlen(R.theirs);
    if (b->tn + 5 + 2 + tn + len > BUF)
        return;                             // (backed up: dropped, the game's own layer sends it again)
    b->tx[b->tn++] = 0x30;
    put_len(b, 2 + tn + len);
    put_str(b, R.theirs, tn);
    memcpy(b->tx + b->tn, data, (size_t)len);
    b->tn += len;
}

// a message from the other one: opened, checked, kept unless it's one already had through another broker
static void got(int bi, const uint8_t * p, int n)
{
    if (n < 12 + 4 || n > 12 + RELAY_MAX + 4)
        return;
    uint8_t plain[RELAY_MAX + 4];
    int m = n - 12;
    memcpy(plain, p + 12, (size_t)m);
    chacha20(R.key, p, plain, m);
    if (memcmp(plain, "GVR1", 4))
        return;                             // another key (another room's, or noise)
    uint32_t tag = (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    for (int i = 0; i < 256; i++)
        if (R.seen[i] == tag && tag)
            return;
    R.seen[R.seen_at++ & 255] = tag;
    if (R.heard_on < 0)
        R.heard_on = bi;
    if (R.qn < QN) {
        int at = (R.qh + R.qn++) % QN;
        memcpy(R.q[at], plain + 4, (size_t)(m - 4));
        R.qlen[at] = m - 4;
    }
}

static void read_broker(int bi)
{
    Broker * b = &R.b[bi];
    for (;;) {
        int n = (int)recv(b->s, (char *)b->rx + b->rn, BUF - b->rn, 0);
        if (n == 0) {
            b->state = -1;                  // closed on us
            return;
        }
        if (n < 0) {
            if (!WOULD_BLOCK())
                b->state = -1;
            break;
        }
        b->rn += n;
        if (b->rn == BUF)
            break;
    }
    // whole packets: type, remaining length, body
    int at = 0;
    while (b->rn - at >= 2) {
        int len = 0, mul = 1, k = 1;
        while (k < 5 && at + k < b->rn) {
            len += (b->rx[at + k] & 127) * mul;
            mul *= 128;
            if (!(b->rx[at + k++] & 128))
                break;
        }
        if (at + k + len > b->rn)
            break;                          // (the rest comes)
        const uint8_t * body = b->rx + at + k;
        int type = b->rx[at] >> 4;
        if (type == 2 && b->state == 2 && len >= 2 && body[1] == 0) {
            mqtt_subscribe(b);              // CONNACK: accepted
            b->state = 3;
            plat_log("relay: %s listening", broker_names[bi]);
        } else if (type == 2 && len >= 2 && body[1] != 0)
            b->state = -1;
        else if (type == 3 && len >= 2) {
            int tl = body[0] << 8 | body[1], skip = 2 + tl + (((b->rx[at] >> 1) & 3) ? 2 : 0);
            if (skip <= len)
                got(bi, body + skip, len - skip);
        }
        at += k + len;
    }
    memmove(b->rx, b->rx + at, (size_t)(b->rn - at));
    b->rn -= at;
    if (b->rn == BUF)
        b->rn = 0;                          // (a packet bigger than the buffer: no message of ours)
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

// ------------------------------------------------------------------------------------------------ the rooms

static const char CODE_ABC[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

void relay_new_room(char * code, int size)
{
#ifndef __vita__
    if (getenv("GHV_ROOM")) {               // tests: a room code known ahead
        snprintf(code, (size_t)size, "%s", getenv("GHV_ROOM"));
        return;
    }
#endif
    uint32_t r = (uint32_t)rand() * 2654435761u ^ (uint32_t)plat_now_us() ^ (uint32_t)rand() << 16;
    char c[8];
    for (int i = 0; i < 6; i++) {
        c[i] = CODE_ABC[r & 31];
        r = r >> 5 ^ (uint32_t)rand() << 27;
    }
    c[6] = 0;
    snprintf(code, (size_t)size, "%s", c);
}

bool relay_room_ok(const char * text, char * code, int size)
{
    char c[8];
    int n = 0;
    for (const char * t = text; *t; t++) {
        char ch = *t >= 'a' && *t <= 'z' ? (char)(*t - 32) : *t;
        if (ch == ' ' || ch == '-')
            continue;
        if (ch == '0')
            ch = 'O';                       // (look-alikes typed for each other)
        if (ch == '1')
            ch = 'I';
        if (ch == 'O')
            ch = 'Q';
        if (ch == 'I')
            ch = 'J';
        if (!strchr(CODE_ABC, ch) || n >= 6)
            return false;
        c[n++] = ch;
    }
    if (n != 6)
        return false;
    c[6] = 0;
    snprintf(code, (size_t)size, "%s", c);
    return true;
}

bool relay_start(const char * room, bool host)
{
    relay_stop();
    memset(&R, 0, sizeof(R));
    R.on = true;
    R.host = host;
    R.heard_on = -1;
    for (int i = 0; i < BROKERS; i++)
        R.b[i].s = BAD_SOCK;
    char seed[64];
    uint8_t h[32];
    snprintf(seed, sizeof(seed), "ghvita relay key %s", room);
    sha256((const uint8_t *)seed, strlen(seed), R.key);
    snprintf(seed, sizeof(seed), "ghvita relay topic %s", room);
    sha256((const uint8_t *)seed, strlen(seed), h);
    char hex[24];
    for (int i = 0; i < 10; i++)
        snprintf(hex + i * 2, 3, "%02x", h[i]);
    snprintf(R.mine, sizeof(R.mine), "ghvita/2/%s/%c", hex, host ? 'h' : 'g');
    snprintf(R.theirs, sizeof(R.theirs), "ghvita/2/%s/%c", hex, host ? 'g' : 'h');
    if (dns_state == 0) {
        dns_state = 1;
        plat_thread("relay dns", lookup, NULL, PLAT_BACKGROUND);
    }
    return true;
}

void relay_stop(void)
{
    for (int i = 0; i < BROKERS; i++)
        if (R.b[i].s != BAD_SOCK && R.b[i].state != 0) {
            close_sock(R.b[i].s);
            R.b[i].s = BAD_SOCK;
        }
    R.on = false;
}

bool relay_on(void)
{
    return R.on;
}

void relay_poll(void)
{
    if (!R.on || dns_state != 2)
        return;
    double t = now_s();
    for (int i = 0; i < BROKERS; i++) {
        Broker * b = &R.b[i];
        if (b->state == -1 && broker_ip[i] && b->tries < 3 && t - b->began > 5) {
            if (b->s != BAD_SOCK)
                close_sock(b->s);           // dropped: once more, a little later
            b->s = BAD_SOCK;
            b->state = 0;
            b->rn = b->tn = 0;
        }
        if (b->state == 0) {
            if (!broker_ip[i]) {
                b->state = -1;
                b->tries = 3;
                continue;
            }
            b->tries++;
            b->began = t;
            b->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (b->s == BAD_SOCK) {
                b->state = -1;
                continue;
            }
            nonblocking(b->s);
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = broker_ip[i];
            a.sin_port = htons(broker_port[i]);
            if (connect(b->s, (struct sockaddr *)&a, sizeof(a)) != 0 && !WOULD_BLOCK()) {
                b->state = -1;
                continue;
            }
            mqtt_connect(b);
            b->state = 1;
        }
        if (b->state == 1) {
            flush(b);                       // (sent once the connection's open)
            if (b->state == 1 && b->tn == 0) {
                b->state = 2;
                b->ping = t;
            }
            if (b->state == 1 && t - b->began > 8)
                b->state = -1;              // nobody's answering there
        }
        if (b->state >= 2) {
            if (t - b->ping > 25) {
                b->ping = t;
                b->tx[b->tn++] = 0xC0;      // PINGREQ (keeps the broker, and the routers on the way, from hanging up)
                b->tx[b->tn++] = 0;
            }
            flush(b);
            if (b->state >= 2)
                read_broker(i);
            if (b->state == 2 && t - b->began > 10)
                b->state = -1;
        }
    }
}

int relay_ready(void)
{
    int n = 0;
    for (int i = 0; R.on && i < BROKERS; i++)
        n += R.b[i].state == 3;
    return n;
}

bool relay_failed(void)
{
    if (!R.on || dns_state != 2)
        return false;
    for (int i = 0; i < BROKERS; i++)
        if (R.b[i].state != -1 || (broker_ip[i] && R.b[i].tries < 3))
            return false;
    return true;
}

bool relay_send(const uint8_t * data, int len)
{
    if (!R.on || len > RELAY_MAX)
        return false;
    static uint32_t counter;
    uint8_t m[12 + 4 + RELAY_MAX];
    uint32_t tag = (uint32_t)rand() * 2654435761u ^ (uint32_t)plat_now_us() * 40503u;
    tag = tag ? tag : 1;
    counter++;
    for (int i = 0; i < 4; i++) {
        m[i] = (uint8_t)(tag >> (8 * i));
        m[4 + i] = (uint8_t)(counter >> (8 * i));
        m[8 + i] = (uint8_t)((uint32_t)rand() >> (8 * i));
    }
    memcpy(m + 12, "GVR1", 4);
    memcpy(m + 16, data, (size_t)len);
    chacha20(R.key, m, m + 12, 4 + len);
    bool sent = false;
    for (int i = 0; i < BROKERS; i++)
        if (R.b[i].state == 3 && (R.heard_on < 0 || R.heard_on == i || R.b[R.heard_on].state != 3)) {
            mqtt_publish(&R.b[i], m, 16 + len);
            flush(&R.b[i]);
            sent = true;
        }
    return sent;
}

int relay_recv(uint8_t * buf, int max)
{
    if (!R.qn)
        return -1;
    int n = R.qlen[R.qh] < max ? R.qlen[R.qh] : max;
    memcpy(buf, R.q[R.qh], (size_t)n);
    R.qh = (R.qh + 1) % QN;
    R.qn--;
    return n;
}
