// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS network self-test (switch/adapter/net_selftest.cpp, v1 step 8c) on the host: its coop_plat_* calls and
// ENet's (through enet_coop's table) go to the std backend through recorders. Every socket it makes must be bound to
// 127.0.0.1 before it sends: on an emulator these are real host sockets, and one on 0.0.0.0 can raise a firewall
// prompt. Nothing answers on 27520/27521 here, so the steps time out; only the addresses are checked.
#include <doctest/doctest.h>

#include "../../switch/adapter/net_selftest.cpp"

#include "coop/os.hpp"
#include "enet_coop.h"

#include <set>
#include <string>
#include <vector>

namespace selftest_test {

const CoopPlatform *Std() { return coop::os::StdPlatform(); }

std::vector<CoopAddr> g_binds;
std::set<int> g_bound;
int g_unbound_sends = 0;
std::vector<std::string> g_log;

int Open(uint32_t kind) { return Std()->sock_open(kind); }
int Bind(int s, const CoopAddr *local) {
    g_binds.push_back(*local);
    const int r = Std()->sock_bind(s, local);
    if (r == 0) g_bound.insert(s);
    return r;
}
int SendTo(int s, const void *data, size_t n, const CoopAddr *to) {
    if (!g_bound.count(s)) ++g_unbound_sends;
    return Std()->sock_sendto(s, data, n, to);
}
int Close(int s) {
    g_bound.erase(s);
    return Std()->sock_close(s);
}

bool Loopback(const CoopAddr &a) {
    return a.family == 4 && a.addr[0] == 127 && a.addr[1] == 0 && a.addr[2] == 0 && a.addr[3] == 1;
}

}  // namespace selftest_test

extern "C" {
int coop_plat_sock_open(uint32_t sock_kind) { return selftest_test::Open(sock_kind); }
int coop_plat_sock_bind(int s, const CoopAddr *local) { return selftest_test::Bind(s, local); }
int coop_plat_sock_local(int s, CoopAddr *out) { return selftest_test::Std()->sock_local(s, out); }
int coop_plat_sock_sendto(int s, const void *data, size_t n, const CoopAddr *to) {
    return selftest_test::SendTo(s, data, n, to);
}
int coop_plat_sock_recvfrom(int s, void *buf, size_t cap, CoopAddr *from) {
    return selftest_test::Std()->sock_recvfrom(s, buf, cap, from);
}
int coop_plat_sock_close(int s) { return selftest_test::Close(s); }
void coop_plat_log(const char *line) { selftest_test::g_log.emplace_back(line); }
}

TEST_CASE("net self-test: every socket is bound to 127.0.0.1 before it sends") {
    // ENet's socket calls go through the same recorders.
    CoopPlatform table = *selftest_test::Std();
    table.sock_open = coop_plat_sock_open;
    table.sock_bind = coop_plat_sock_bind;
    table.sock_sendto = coop_plat_sock_sendto;
    table.sock_close = coop_plat_sock_close;
    coop_enet_use_platform(&table);

    // Up to the echo (frame 1800), past its 300-frame timeout into the ENet connect, and a few ENet frames.
    for (uint64_t frame = 1; frame <= 1800 + 300 + 20; ++frame) coop_net_selftest_frame(frame);

    REQUIRE(selftest_test::g_binds.size() == 2);  // the UDP echo socket, then ENet's host socket
    for (const CoopAddr &a : selftest_test::g_binds) {
        CHECK(selftest_test::Loopback(a));
        CHECK(a.port == 0);  // any free port
    }
    CHECK(selftest_test::g_unbound_sends == 0);

    bool bound_line = false, connecting = false;
    for (const std::string &line : selftest_test::g_log) {
        bound_line = bound_line || line.rfind("net: udp bound to port ", 0) == 0;
        connecting = connecting || line == "net: enet connecting to 127.0.0.1:27521";
    }
    CHECK(bound_line);
    CHECK(connecting);

    // End the test's ENet host (it is still waiting for a connect nobody answers).
    for (uint64_t frame = 2121; frame <= 2121 + 900; ++frame) coop_net_selftest_frame(frame);
    coop_enet_use_platform(nullptr);
}
