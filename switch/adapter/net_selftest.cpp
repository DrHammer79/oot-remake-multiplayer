// SPDX-License-Identifier: GPL-3.0-or-later
// The network self-test of the ExeFS payload (v1 step 8c run; built only with build.py --net). From the game's
// frame hook (from frame 1800), a few steps a frame and never waiting:
//   1. UDP through the CoopPlatform slots (platform_net.cpp): open, bind any port, send one datagram to the run's
//      echo on 127.0.0.1:27520, read the echo back.
//   2. ENet on the same slots (coop/net/enet_coop): connect to coop_server on 127.0.0.1:27521, then disconnect.
// Every result is one `net:` log line (coop_plat_log); the run's check reads them (reports/v1-exefs-net-a.md).
#include <enet/enet.h>

#include "coop_platform_static.h"

extern "C" {
void coop_net_selftest_frame(uint64_t frame);  // switch/proto/main.cpp, every frame (--net builds)
}

namespace {

constexpr uint64_t kStartFrame = 1800;  // ~30 s in: after the prototype's net step and the OS self-test (1200)
constexpr uint64_t kEchoFrames = 300;
constexpr uint64_t kConnectFrames = 900;
constexpr uint64_t kDisconnectFrames = 300;
constexpr uint16_t kEchoPort = 27520;    // LANE1's port: the run's echo
constexpr uint16_t kServerPort = 27521;  // the run's coop_server
constexpr char kHello[] = "coop-net echo from subsdk1";

// One log line from text and numbers.
struct Line {
    char buf[160];
    size_t n = 0;
    Line &s(const char *t) {
        while (*t && n + 1 < sizeof buf) buf[n++] = *t++;
        return *this;
    }
    Line &u(uint64_t v) {
        char d[20];
        int k = 0;
        do {
            d[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k && n + 1 < sizeof buf) buf[n++] = d[--k];
        return *this;
    }
    Line &addr(const CoopAddr &a) {
        for (int i = 0; i < 4; ++i) {
            if (i) s(".");
            u(a.addr[i]);
        }
        return s(":").u(a.port);
    }
    ~Line() {
        buf[n] = '\0';
        coop_plat_log(buf);
    }
};

CoopAddr Loopback(uint16_t port) {
    CoopAddr a{};
    a.family = 4;
    a.port = port;
    a.addr[0] = 127;
    a.addr[3] = 1;
    return a;
}

enum class Step { kWait, kEcho, kConnect, kDisconnect, kDone };

Step g_step = Step::kWait;
uint64_t g_deadline = 0;
int g_sock = -1;
ENetHost *g_host = nullptr;
ENetPeer *g_peer = nullptr;

void StartEcho(uint64_t frame) {
    g_sock = coop_plat_sock_open(COOP_SOCK_UDP4);
    // Loopback only: on an emulator this is a real host socket, and one on 0.0.0.0 can raise a firewall prompt.
    const CoopAddr here = Loopback(0);
    CoopAddr local{};
    if (g_sock < 0 || coop_plat_sock_bind(g_sock, &here) != 0 || coop_plat_sock_local(g_sock, &local) != 0) {
        Line().s(g_sock < 0 ? "net: FAIL udp open" : "net: FAIL udp bind");
        if (g_sock >= 0) coop_plat_sock_close(g_sock);
        g_sock = -1;
        return;
    }
    Line().s("net: udp bound to port ").u(local.port).s(" through the platform slots");
    const CoopAddr echo = Loopback(kEchoPort);
    const int sent = coop_plat_sock_sendto(g_sock, kHello, sizeof kHello - 1, &echo);
    Line().s("net: udp sent ").u(sent < 0 ? 0 : static_cast<uint64_t>(sent)).s(" bytes to ").addr(echo);
    g_deadline = frame + kEchoFrames;
    g_step = Step::kEcho;
}

bool SameAsHello(const char *buf, int n) {
    if (n != static_cast<int>(sizeof kHello - 1)) return false;
    for (int i = 0; i < n; ++i)
        if (buf[i] != kHello[i]) return false;
    return true;
}

void StartConnect(uint64_t frame) {
    if (enet_initialize() != 0) {
        Line().s("net: FAIL enet_initialize");
        g_step = Step::kDone;
        return;
    }
    // Bound to 127.0.0.1 too: unbound, the first send would bind the host socket to 0.0.0.0 (see StartEcho).
    ENetAddress here{};
    ENetAddress server{};
    server.port = kServerPort;
    if (enet_address_set_host_ip(&here, "127.0.0.1") == 0)
        g_host = enet_host_create(&here, 1, 6, 0, 0);  // port 0 = any; 6 = the protocol's channel count
    if (!g_host || enet_address_set_host_ip(&server, "127.0.0.1") != 0 ||
        !(g_peer = enet_host_connect(g_host, &server, 6, 0))) {
        Line().s("net: FAIL enet host/connect");
        g_step = Step::kDone;
        return;
    }
    Line().s("net: enet connecting to ").addr(Loopback(kServerPort));
    g_deadline = frame + kConnectFrames;
    g_step = Step::kConnect;
}

void Finish() {
    if (g_host) enet_host_destroy(g_host);
    g_host = nullptr;
    g_peer = nullptr;
    enet_deinitialize();
    Line().s("net: self-test done");
    g_step = Step::kDone;
}

}  // namespace

extern "C" void coop_net_selftest_frame(uint64_t frame) {
    switch (g_step) {
        case Step::kWait:
            if (frame < kStartFrame) return;
            StartEcho(frame);
            if (g_step == Step::kWait) StartConnect(frame);  // UDP failed: still try ENet
            return;
        case Step::kEcho: {
            char buf[64];
            CoopAddr from{};
            const int got = coop_plat_sock_recvfrom(g_sock, buf, sizeof buf, &from);
            if (got >= 0) {
                Line().s("net: udp echo ").u(static_cast<uint64_t>(got)).s(" bytes back from ").addr(from)
                    .s(SameAsHello(buf, got) ? ", same text" : ", DIFFERENT text");
            } else if (got != COOP_SOCK_WOULD_BLOCK) {
                Line().s("net: FAIL udp receive error");
            } else if (frame < g_deadline) {
                return;
            } else {
                Line().s("net: FAIL no udp echo in ").u(kEchoFrames).s(" frames");
            }
            coop_plat_sock_close(g_sock);
            g_sock = -1;
            StartConnect(frame);
            return;
        }
        case Step::kConnect:
        case Step::kDisconnect: {
            ENetEvent event;
            // Timeout 0: ENet never waits on the game's thread.
            for (int i = 0; i < 8 && g_host && enet_host_service(g_host, &event, 0) > 0; ++i) {
                if (event.type == ENET_EVENT_TYPE_CONNECT && g_step == Step::kConnect) {
                    Line().s("net: enet connected to the server (mtu ").u(event.peer->mtu).s(")");
                    enet_peer_disconnect(g_peer, 0);
                    g_deadline = frame + kDisconnectFrames;
                    g_step = Step::kDisconnect;
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    Line().s(g_step == Step::kDisconnect ? "net: enet disconnected"
                                                         : "net: FAIL enet refused (disconnect before connect)");
                    Finish();
                    return;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                }
            }
            if (frame >= g_deadline) {
                Line().s(g_step == Step::kConnect ? "net: FAIL enet connect timed out" : "net: FAIL no disconnect");
                if (g_peer) enet_peer_reset(g_peer);
                Finish();
            }
            return;
        }
        case Step::kDone:
            return;
    }
}
