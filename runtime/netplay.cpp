// See netplay.h.
#include "netplay.h"
#include "input_log.h"
#include "platform.h"
#include "runtime.h"
#include "recomp.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kNoSocket = INVALID_SOCKET;
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kNoSocket = -1;
using socklen_type = socklen_t;
#endif

namespace {

constexpr uint32_t kMagic = 0x4E504C59;  // "NPLY"
constexpr uint32_t kVersion = 2;
constexpr int kMaxPlayers = 4;
constexpr uint32_t kRing = 1024;          // ticks of state kept; far more than the delay
constexpr int kResend = 32;               // ticks of history each input packet carries
constexpr uint32_t kChunk = 1024;         // card bytes per packet
constexpr uint32_t kCardWindow = 32;      // card packets in flight
constexpr uint32_t kHashEvery = 1200;     // ticks between RAM hashes (10 s)
constexpr int kTimeoutMs = 15000;         // no word from a peer for this long ends the session

enum Msg : uint8_t { HELLO = 1, WELCOME, REJECT, CARD, CARDACK, START, CINPUT, HFRAME };

enum class Role { None, Host, Client };
Role g_role = Role::None;
sock_t g_sock = kNoSocket;
int g_players = 0, g_delay = 0, g_chan = 0;  // g_chan: this peer's channel

struct Peer {
    sockaddr_in addr{};
    bool joined = false;
    uint32_t card_have = 0;          // card bytes it has acknowledged
    uint32_t inputs_through = 0;     // host: the ticks of its input received contiguously (+1)
    uint32_t frames_through = 0;     // host: the frames it has acknowledged (+1)
    float aspect = 0;                // host: the aspect it displays
    std::chrono::steady_clock::time_point seen;
};
Peer g_peers[kMaxPlayers];           // host: indexed by channel (0 is the host itself)
sockaddr_in g_host{};                // client: the host

// One tick of the session: every channel's pad state and the jump made at it.
struct Frame {
    PadState pad[kMaxPlayers];
    uint32_t jump = 0;
    uint8_t have = 0;                // host: channels whose state has arrived
    bool done = false;               // every state is here (host: and relayed)
    uint32_t tick = UINT32_MAX;      // which tick this ring slot holds
};
Frame g_frames[kRing];
uint32_t g_tick = 0;                 // the next tick to play
uint32_t g_done_through = 0;         // client: frames received contiguously from the host (+1)
PadState g_cur[kMaxPlayers];         // what each channel reads this tick
uint64_t g_pending_jump = 0;         // host: edges sent but not yet made
std::vector<uint8_t> g_card;
time_t g_epoch = 0;
int g_utc_offset = 0;
uint64_t g_build = 0;
float g_aspect = 0;                  // the session's aspect (game_aspect.h)
std::chrono::steady_clock::time_point g_last_send;
uint64_t g_hash_mine[kMaxPlayers];   // host: the hashes of the tick below, by channel
uint32_t g_hash_tick = UINT32_MAX;
uint8_t g_hash_have = 0;

Frame& frame(uint32_t t) {
    Frame& f = g_frames[t % kRing];
    if (f.tick != t) {
        f = Frame();
        f.tick = t;
        for (int c = 0; c < kMaxPlayers; c++) f.pad[c].connected = c < g_players;
    }
    return f;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t float_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
float bits_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

// The build's identity: the recompiled code is the same on every platform, so these
// tables are too. A peer with another build of the game would desync at once.
uint64_t build_id() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; } };
    mix(kVersion);
    for (const char* p = GCN_DISC_ID; *p; p++) mix((uint8_t)*p);
    mix(g_recomp_func_count);
    mix(g_recomp_code_base);
    mix(g_recomp_code_end);
    for (uint32_t k = 0; k < g_recomp_rel_count; k++) mix(g_recomp_rels[k].fingerprint);
    return h;
}

// Ranges netplay_hash_exclude has taken out of the hash, as offsets into RAM.
std::vector<std::pair<uint32_t, uint32_t>> g_hash_skip;

uint64_t ram_hash() {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0; a < RAM_SIZE; a += 8) {
        uint64_t v;
        memcpy(&v, HOST(0x80000000u + a), 8);
        for (const auto& r : g_hash_skip)
            if (a + 8 > r.first && a < r.second) v = 0;
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

// ---- wire format ----
struct Writer {
    uint8_t b[1500];
    size_t n = 0;
    void u8(uint8_t v) { b[n++] = v; }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void bytes(const void* p, size_t len) { memcpy(b + n, p, len); n += len; }
    void pad(const PadState& s) {
        u8(s.connected ? 1 : 0); u16(s.buttons);
        u8(s.stick_x); u8(s.stick_y); u8(s.cstick_x); u8(s.cstick_y); u8(s.trig_l); u8(s.trig_r);
    }
};
struct Reader {
    const uint8_t* b;
    size_t n, i = 0;
    bool ok = true;
    bool need(size_t k) { if (i + k > n) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return b[i++]; }
    uint16_t u16() { uint16_t lo = u8(); return (uint16_t)(lo | (u8() << 8)); }
    uint32_t u32() { uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
    uint64_t u64() { uint64_t lo = u32(); return lo | ((uint64_t)u32() << 32); }
    PadState pad() {
        PadState s;
        s.connected = u8() != 0; s.buttons = u16();
        s.stick_x = u8(); s.stick_y = u8(); s.cstick_x = u8(); s.cstick_y = u8(); s.trig_l = u8(); s.trig_r = u8();
        return s;
    }
};

void send_to(const sockaddr_in& to, const Writer& w) {
    sendto(g_sock, (const char*)w.b, (int)w.n, 0, (const sockaddr*)&to, sizeof(to));
}

Writer header(Msg m) {
    Writer w;
    w.u32(kMagic);
    w.u8(m);
    return w;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

bool open_socket(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == kNoSocket) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(g_sock, (const sockaddr*)&a, sizeof(a)) != 0) {
        fprintf(stderr, "[netplay] cannot listen on UDP port %d\n", port);
        return false;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
#else
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    return true;
}

// The next datagram, if one is waiting.
bool receive(Reader& r, sockaddr_in& from, uint8_t* buf, size_t cap) {
    socklen_type len = sizeof(from);
    int n = (int)recvfrom(g_sock, (char*)buf, (int)cap, 0, (sockaddr*)&from, &len);
    if (n <= 5) return false;
    r = Reader{buf, (size_t)n};
    return r.u32() == kMagic;
}

void hash_report(int chan, uint32_t tick, uint64_t h) {
    if (tick != g_hash_tick) return;
    g_hash_mine[chan] = h;
    g_hash_have |= 1u << chan;
    if (g_hash_have != (1u << g_players) - 1) return;
    for (int c = 1; c < g_players; c++)
        if (g_hash_mine[c] != g_hash_mine[0])
            fprintf(stderr, "[netplay] DESYNC at tick %u: player %d's game differs from the host's\n", tick, c + 1);
    g_hash_have = 0;
}

// ---- host ----
void host_handle(Reader& r, const sockaddr_in& from) {
    const uint8_t m = r.u8();
    int chan = -1;
    for (int c = 1; c < g_players; c++)
        if (g_peers[c].joined && same_addr(g_peers[c].addr, from)) chan = c;
    if (chan > 0) g_peers[chan].seen = std::chrono::steady_clock::now();
    if (m == HELLO) {
        const uint32_t ver = r.u32();
        const uint64_t build = r.u64();
        const float aspect = bits_float(r.u32());
        if (!r.ok) return;
        if (ver != kVersion || build != g_build) {
            Writer w = header(REJECT);
            send_to(from, w);
            fprintf(stderr, "[netplay] refused a player with a different build of the game\n");
            return;
        }
        if (chan < 0) {
            for (int c = 1; c < g_players && chan < 0; c++)
                if (!g_peers[c].joined) {
                    chan = c;
                    g_peers[c].joined = true;
                    g_peers[c].addr = from;
                    g_peers[c].seen = std::chrono::steady_clock::now();
                    g_peers[c].aspect = aspect;
                    char ip[64];
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    fprintf(stderr, "[netplay] player %d joined from %s:%d\n", c + 1, ip, ntohs(from.sin_port));
                }
            if (chan < 0) { Writer w = header(REJECT); send_to(from, w); return; }
        }
        Writer w = header(WELCOME);
        w.u8((uint8_t)chan); w.u8((uint8_t)g_players); w.u8((uint8_t)g_delay);
        w.u64((uint64_t)g_epoch); w.u32((uint32_t)g_utc_offset); w.u32((uint32_t)g_card.size());
        send_to(from, w);
    } else if (m == CARDACK && chan > 0) {
        const uint32_t have = r.u32();
        if (r.ok && have > g_peers[chan].card_have) g_peers[chan].card_have = have;
    } else if (m == CINPUT && chan > 0) {
        const uint32_t first = r.u32();
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n && r.ok; i++) {
            const PadState s = r.pad();
            const uint32_t t = first + i;
            if (!r.ok || t < g_tick || t >= g_tick + kRing / 2) continue;
            Frame& f = frame(t);
            if (f.done || (f.have & (1u << chan))) continue;
            f.pad[chan] = s;
            f.pad[chan].connected = true;
            f.have |= 1u << chan;
        }
        const uint32_t frames_through = r.u32();
        const uint32_t htick = r.u32();
        const uint64_t h = r.u64();
        if (!r.ok) return;
        if (frames_through > g_peers[chan].frames_through) g_peers[chan].frames_through = frames_through;
        while (frame(g_peers[chan].inputs_through).have & (1u << chan) ||
               frame(g_peers[chan].inputs_through).done)
            g_peers[chan].inputs_through++;
        if (htick != UINT32_MAX) hash_report(chan, htick, h);
    }
}

// The start, with the session's aspect. Sent until the client's first input arrives.
void send_start(int c) {
    Writer w = header(START);
    w.u32(float_bits(g_aspect));
    send_to(g_peers[c].addr, w);
}

// Each client's frames from what it has acknowledged up to the last one complete.
void host_send_frames(uint32_t through) {
    for (int c = 1; c < g_players; c++) {
        const Peer& p = g_peers[c];
        uint32_t from = p.frames_through;
        if (through > (uint32_t)kResend && from < through - kResend) from = through - kResend;
        if (from >= through) from = through > 0 ? through - 1 : 0;
        Writer w = header(HFRAME);
        w.u32(from);
        const uint8_t n = (uint8_t)(through - from);
        w.u8(n);
        for (uint32_t t = from; t < from + n; t++) {
            const Frame& f = frame(t);
            for (int k = 0; k < kMaxPlayers; k++) w.pad(f.pad[k]);
            w.u32(f.jump);
        }
        w.u32(p.inputs_through);
        send_to(p.addr, w);
    }
}

// ---- client ----
void client_handle(Reader& r) {
    const uint8_t m = r.u8();
    if (m == HFRAME) {
        const uint32_t first = r.u32();
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n && r.ok; i++) {
            Frame got;
            for (int k = 0; k < kMaxPlayers; k++) got.pad[k] = r.pad();
            got.jump = r.u32();
            const uint32_t t = first + i;
            if (!r.ok || t < g_tick || t >= g_tick + kRing / 2) continue;
            Frame& f = frame(t);
            if (f.done) continue;
            for (int k = 0; k < kMaxPlayers; k++) f.pad[k] = got.pad[k];
            f.jump = got.jump;
            f.done = true;
        }
        const uint32_t acked = r.u32();
        if (r.ok && acked > g_peers[0].inputs_through) g_peers[0].inputs_through = acked;
        while (frame(g_done_through).done) g_done_through++;
        g_peers[0].seen = std::chrono::steady_clock::now();
    }
}

PadState g_mine[kRing];              // client: this peer's own input, by tick
uint32_t g_mine_through = 0;         // client: own input produced for ticks below this
uint32_t g_hash_send_tick = UINT32_MAX;
uint64_t g_hash_send = 0;

void client_send_inputs() {
    uint32_t from = g_peers[0].inputs_through;
    if (g_mine_through > (uint32_t)kResend && from < g_mine_through - kResend) from = g_mine_through - kResend;
    if (from >= g_mine_through) from = g_mine_through > 0 ? g_mine_through - 1 : 0;
    Writer w = header(CINPUT);
    w.u32(from);
    const uint8_t n = (uint8_t)(g_mine_through - from);
    w.u8(n);
    for (uint32_t t = from; t < from + n; t++) w.pad(g_mine[t % kRing]);
    w.u32(g_done_through);
    w.u32(g_hash_send_tick);
    w.u64(g_hash_send);
    send_to(g_host, w);
}

void pump() {
    uint8_t buf[1600];
    Reader r{buf, 0};
    sockaddr_in from{};
    while (receive(r, from, buf, sizeof(buf))) {
        if (g_role == Role::Host) host_handle(r, from);
        else if (same_addr(from, g_host)) client_handle(r);
    }
}

void lost(const char* who) {
    fprintf(stderr, "[netplay] lost contact with %s; ending the session\n", who);
    input_log_end();
    fflush(stderr);
    plat_exit_now(4);
}

}  // namespace

bool netplay_active() { return g_role != Role::None; }

double netplay_aspect() { return g_aspect; }

int netplay_session_channel() { return g_role != Role::None ? g_chan : input_replay_session_channel(); }

uint64_t netplay_ram_hash() { return ram_hash(); }

void netplay_hash_exclude(uint32_t addr, uint32_t size) {
    const uint32_t off = addr - 0x80000000u;
    g_hash_skip.emplace_back(off, off + size);
}

bool netplay_host(int port, int players, int delay, double aspect, const std::string& card) {
    if (players < 2 || players > kMaxPlayers) {
        fprintf(stderr, "[netplay] a session is 2 to %d players\n", kMaxPlayers);
        return false;
    }
    if (!open_socket(port)) return false;
    g_role = Role::Host;  // pump() hands packets to the host's side from here on
    g_players = players;
    g_delay = delay;
    g_chan = 0;
    g_build = build_id();
    g_aspect = (float)aspect;
    g_epoch = time(nullptr);
    g_utc_offset = rtc_utc_offset();
    if (FILE* f = fopen(card.c_str(), "rb")) {
        uint8_t b[65536];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) g_card.insert(g_card.end(), b, b + n);
        fclose(f);
    }
    fprintf(stderr, "[netplay] hosting on UDP port %d for %d players; waiting for them to join\n", port, players);
    uint64_t last = 0;
    for (;;) {
        pump();
        bool ready = true;
        for (int c = 1; c < players; c++) ready &= g_peers[c].joined && g_peers[c].card_have >= g_card.size();
        if (ready) break;
        const uint64_t now = now_ms();
        if (now - last >= 20) {
            last = now;
            for (int c = 1; c < players; c++) {
                Peer& p = g_peers[c];
                if (!p.joined || p.card_have >= g_card.size()) continue;
                for (uint32_t k = 0, off = p.card_have; k < kCardWindow && off < g_card.size(); k++, off += kChunk) {
                    const uint32_t len = (uint32_t)std::min<size_t>(kChunk, g_card.size() - off);
                    Writer w = header(CARD);
                    w.u32(off); w.u16((uint16_t)len); w.bytes(g_card.data() + off, len);
                    send_to(p.addr, w);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // What the game treats as in view can depend on the aspect (game_aspect.h); the session
    // plays at the widest, so every player's sides are filled.
    for (int c = 1; c < players; c++) g_aspect = std::max(g_aspect, g_peers[c].aspect);
    for (int c = 1; c < players; c++) send_start(c);
    rtc_set_base(g_epoch);
    for (int c = 1; c < players; c++) g_peers[c].seen = std::chrono::steady_clock::now();
    fprintf(stderr, "[netplay] everyone is in: starting (input delay %d ticks, the game's aspect %.3f)\n", delay, g_aspect);
    return true;
}

bool netplay_join(const char* host, double aspect, const std::string& card_out) {
    std::string s(host);
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) { fprintf(stderr, "[netplay] --join wants host:port\n"); return false; }
    const std::string name = s.substr(0, colon);
    const int port = atoi(s.c_str() + colon + 1);
    if (!open_socket(0)) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[netplay] cannot resolve %s\n", name.c_str());
        return false;
    }
    g_host = *(const sockaddr_in*)res->ai_addr;
    g_host.sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    g_build = build_id();
    fprintf(stderr, "[netplay] joining %s\n", host);
    bool welcomed = false, started = false;
    uint32_t card_size = 0, have = 0;
    std::vector<uint8_t> card;
    uint64_t last = 0;
    const uint64_t t0 = now_ms();
    uint8_t buf[1600];
    while (!started) {
        Reader r{buf, 0};
        sockaddr_in from{};
        while (receive(r, from, buf, sizeof(buf))) {
            if (!same_addr(from, g_host)) continue;
            const uint8_t m = r.u8();
            if (m == REJECT) {
                fprintf(stderr, "[netplay] the host refused us: the session is full, or it runs another build of the game\n");
                return false;
            } else if (m == WELCOME && !welcomed) {
                g_chan = r.u8(); g_players = r.u8(); g_delay = r.u8();
                g_epoch = (time_t)r.u64(); g_utc_offset = (int)r.u32(); card_size = r.u32();
                if (!r.ok) continue;
                welcomed = true;
                card.assign(card_size, 0);
                fprintf(stderr, "[netplay] joined as player %d of %d; receiving the host's memory card\n", g_chan + 1, g_players);
            } else if (m == CARD && welcomed) {
                const uint32_t off = r.u32();
                const uint16_t len = r.u16();
                if (r.ok && off == have && off + len <= card_size && r.need(len)) {
                    memcpy(card.data() + off, r.b + r.i, len);
                    have += len;
                }
            } else if (m == START && welcomed && have >= card_size) {
                const float a = bits_float(r.u32());
                if (r.ok && a > 0.0f) {
                    g_aspect = a;
                    started = true;
                }
            }
        }
        const uint64_t now = now_ms();
        if (now - last >= (welcomed ? 10u : 250u)) {
            last = now;
            if (!welcomed) {
                Writer w = header(HELLO);
                w.u32(kVersion); w.u64(g_build); w.u32(float_bits((float)aspect));
                send_to(g_host, w);
            } else {
                Writer w = header(CARDACK);
                w.u32(have);
                send_to(g_host, w);
            }
        }
        if (!welcomed && now - t0 > 30000) {
            fprintf(stderr, "[netplay] no answer from %s\n", host);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A host without a card formats one at boot, from the session's start time
    // (memcard.cpp); so does a client given none, rather than read an empty file as a card.
    if (card.empty()) {
        remove(card_out.c_str());
    } else if (FILE* f = fopen(card_out.c_str(), "wb")) {
        fwrite(card.data(), 1, card.size(), f);
        fclose(f);
    }
    rtc_set_base(g_epoch);
    rtc_set_utc_offset(g_utc_offset);
    g_peers[0].seen = std::chrono::steady_clock::now();
    g_role = Role::Client;
    fprintf(stderr, "[netplay] starting (input delay %d ticks, the game's aspect %.3f)\n", g_delay, g_aspect);
    return true;
}

void netplay_tick(const PadState& live) {
    if (g_role == Role::None) return;
    const uint32_t T = g_tick;
    // This peer's controller, for the tick the delay puts it at.
    PadState local = live;
    local.connected = true;
    const uint32_t ahead = T + (uint32_t)g_delay;
    if (g_role == Role::Host) {
        Frame& f = frame(ahead);
        f.pad[0] = local;
        f.have |= 1;
        // The host's lag, less what it has already sent and not yet made, is the jump for
        // that tick: every peer makes it there.
        const uint64_t lag = clock_lag_edges();
        f.jump = lag > g_pending_jump ? (uint32_t)std::min<uint64_t>(lag - g_pending_jump, UINT32_MAX) : 0;
        g_pending_jump += f.jump;
    } else {
        g_mine[ahead % kRing] = local;
        g_mine_through = ahead + 1;
    }
    // The ticks inside the delay at the start have no input from anyone: nothing pressed.
    if (T < (uint32_t)g_delay) frame(T).done = true;
    const uint32_t all = (1u << g_players) - 1;
    const auto t0 = std::chrono::steady_clock::now();
    bool sent = false;
    for (;;) {
        pump();
        Frame& f = frame(T);
        if (g_role == Role::Host && !f.done && (f.have & all) == all) f.done = true;
        if (g_role == Role::Host && f.done) {
            host_send_frames(T + 1);
            break;
        }
        if (g_role == Role::Client) {
            const auto now = std::chrono::steady_clock::now();
            if (!sent || now - g_last_send > std::chrono::milliseconds(4)) {
                client_send_inputs();
                g_last_send = now;
                sent = true;
            }
            if (f.done) break;
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_last_send > std::chrono::milliseconds(4)) {
                host_send_frames(T);
                for (int c = 1; c < g_players; c++)
                    if (g_peers[c].inputs_through == 0) send_start(c);
                g_last_send = now;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - t0 > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (g_role == Role::Client) {
            if (now - g_peers[0].seen > std::chrono::milliseconds(kTimeoutMs)) lost("the host");
        } else {
            for (int c = 1; c < g_players; c++)
                if (!(f.have & (1u << c)) && now - g_peers[c].seen > std::chrono::milliseconds(kTimeoutMs)) {
                    char who[32];
                    snprintf(who, sizeof(who), "player %d", c + 1);
                    lost(who);
                }
        }
    }
    const Frame& f = frame(T);
    for (int c = 0; c < kMaxPlayers; c++) g_cur[c] = f.pad[c];
    if (g_role == Role::Host) g_pending_jump -= std::min<uint64_t>(g_pending_jump, f.jump);
    clock_apply_jump(f.jump);
    // Every so often each peer hashes guest RAM at the same tick; the host compares.
    if (T > 0 && T % kHashEvery == 0) {
        const uint64_t h = ram_hash();
        if (g_role == Role::Host) {
            g_hash_tick = T;
            g_hash_have = 0;
            hash_report(0, T, h);
        } else {
            g_hash_send_tick = T;
            g_hash_send = h;
        }
    }
    g_tick = T + 1;
}

bool netplay_pad(int chan, PadState& s) {
    if (g_role == Role::None || chan < 0 || chan >= kMaxPlayers) return false;
    s = g_cur[chan];
    return true;
}
