// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS adapter's network slots (switch/adapter/platform_net.cpp, v1 step 8c) on the host, over a fake of
// the game's nn::socket: an in-memory UDP loopback that checks every call is non-blocking (MSG_DONTWAIT) and
// uses the Switch's sockaddr layout (length byte, family 2, network byte order).
#include <doctest/doctest.h>

#include "../../switch/adapter/platform_net.cpp"

#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

struct Datagram {
    std::vector<uint8_t> bytes;
    coop_switch_net::SockAddrIn from;
};

struct FakeSocket {
    bool open = false;
    uint16_t port = 0;  // bound port, 0 = unbound
    std::deque<Datagram> queue;
};

struct Fake {
    std::map<int, FakeSocket> fds;
    int next_fd = 3;  // 0-2 are the game's own (never opened through the slots)
    uint16_t next_port = 50000;
    int blocking_calls = 0;  // send/recv without MSG_DONTWAIT
    int bad_layout = 0;      // a sockaddr not in the Switch's layout
    int resolver_calls = 0;
    int frees = 0;
    int closes = 0;
    int max_fds = 64;
} g_fake;

void Reset() { g_fake = Fake{}; for (int &fd : g_owned) fd = -1; }

bool GoodLayout(const ::sockaddr *sa, unsigned int len) {
    const auto *in = reinterpret_cast<const coop_switch_net::SockAddrIn *>(sa);
    return len == 16 && in->len == 16 && in->family == 2;
}

uint16_t PortOf(const coop_switch_net::SockAddrIn &sa) { return static_cast<uint16_t>(sa.port[0] << 8 | sa.port[1]); }

coop_switch_net::SockAddrIn Loopback(uint16_t port) {
    coop_switch_net::SockAddrIn sa{};
    sa.len = 16;
    sa.family = 2;
    sa.port[0] = static_cast<uint8_t>(port >> 8);
    sa.port[1] = static_cast<uint8_t>(port);
    sa.addr[0] = 127;
    sa.addr[3] = 1;
    return sa;
}

// Two results for a name: an IPv6 one first (family 28), then 10.1.2.3, as a resolver may order them.
::sockaddr g_v6_addr{};
coop_switch_net::SockAddrIn g_v4_addr{};
::addrinfo g_v4_info{};
::addrinfo g_v6_info{};

}  // namespace

namespace nn::socket {
bool IsInitialized() { return true; }
int Socket(int domain, int type, int protocol) {
    if (domain != 2 || type != 2 || protocol != 17) return -1;
    if (static_cast<int>(g_fake.fds.size()) >= g_fake.max_fds) return -1;
    const int fd = g_fake.next_fd++;
    g_fake.fds[fd].open = true;
    return fd;
}
int Bind(int fd, const ::sockaddr *addr, unsigned int len) {
    if (!GoodLayout(addr, len)) ++g_fake.bad_layout;
    auto &s = g_fake.fds[fd];
    if (!s.open || s.port) return -1;
    const uint16_t port = PortOf(*reinterpret_cast<const coop_switch_net::SockAddrIn *>(addr));
    s.port = port ? port : g_fake.next_port++;
    return 0;
}
int GetSockName(int fd, ::sockaddr *addr, unsigned int *len) {
    auto &s = g_fake.fds[fd];
    if (!s.open || *len < 16) return -1;
    auto sa = Loopback(s.port);
    sa.addr[0] = sa.addr[3] = 0;  // bound to any
    std::memcpy(addr, &sa, 16);
    *len = 16;
    return 0;
}
long SendTo(int fd, const void *data, unsigned long size, int flags, const ::sockaddr *to, unsigned int to_len) {
    if (!(flags & 0x80)) ++g_fake.blocking_calls;
    if (!GoodLayout(to, to_len)) ++g_fake.bad_layout;
    auto &s = g_fake.fds[fd];
    if (!s.open) return -1;
    if (!s.port) s.port = g_fake.next_port++;  // an unbound socket gets a port on its first send
    const auto &dest = *reinterpret_cast<const coop_switch_net::SockAddrIn *>(to);
    for (auto &[other_fd, other] : g_fake.fds) {
        if (other.open && other.port == PortOf(dest) && dest.addr[0] == 127) {
            const auto *b = static_cast<const uint8_t *>(data);
            other.queue.push_back({std::vector<uint8_t>(b, b + size), Loopback(s.port)});
        }
    }
    return static_cast<long>(size);
}
long RecvFrom(int fd, void *buf, unsigned long size, int flags, ::sockaddr *from, unsigned int *from_len) {
    if (!(flags & 0x80)) ++g_fake.blocking_calls;
    auto &s = g_fake.fds[fd];
    if (!s.open || s.queue.empty()) return -1;  // EAGAIN
    Datagram d = s.queue.front();
    s.queue.pop_front();
    const size_t n = d.bytes.size() < size ? d.bytes.size() : size;
    std::memcpy(buf, d.bytes.data(), n);
    if (from && from_len && *from_len >= 16) {
        std::memcpy(from, &d.from, 16);
        *from_len = 16;
    }
    return static_cast<long>(n);
}
int Close(int fd) {
    ++g_fake.closes;
    auto it = g_fake.fds.find(fd);
    if (it == g_fake.fds.end() || !it->second.open) return -1;
    g_fake.fds.erase(it);
    return 0;
}
int GetAddrInfo(const char *node, const char *, const ::addrinfo *hints, ::addrinfo **res) {
    ++g_fake.resolver_calls;
    if (!hints || hints->ai_family != 2 || hints->ai_socktype != 2) return -1;
    if (std::string(node) != "host.example") return -1;
    g_v6_addr = ::sockaddr{};
    g_v6_addr.sa_len = 16;
    g_v6_addr.sa_family = 28;
    g_v6_info = ::addrinfo{};
    g_v6_info.ai_family = 28;
    g_v6_info.ai_addr = &g_v6_addr;
    g_v6_info.ai_addrlen = 16;
    g_v4_addr = Loopback(4242);
    g_v4_addr.addr[0] = 10;
    g_v4_addr.addr[1] = 1;
    g_v4_addr.addr[2] = 2;
    g_v4_addr.addr[3] = 3;
    g_v4_info = ::addrinfo{};
    g_v4_info.ai_family = 2;
    g_v4_info.ai_addr = reinterpret_cast<::sockaddr *>(&g_v4_addr);
    g_v4_info.ai_addrlen = 16;
    g_v6_info.ai_next = &g_v4_info;
    *res = &g_v6_info;
    return 0;
}
void FreeAddrInfo(::addrinfo *res) {
    if (res == &g_v6_info) ++g_fake.frees;
}
}  // namespace nn::socket

namespace {

CoopAddr Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    CoopAddr out{};
    out.family = 4;
    out.port = port;
    out.addr[0] = a;
    out.addr[1] = b;
    out.addr[2] = c;
    out.addr[3] = d;
    return out;
}

}  // namespace

TEST_CASE("switch net: sockaddr layout and IPv4 literals") {
    coop_switch_net::SockAddrIn sa{};
    CHECK(coop_switch_net::ToSwitch(Addr(192, 168, 1, 20, 27520), sa));
    CHECK(sa.len == 16);
    CHECK(sa.family == 2);
    CHECK(sa.port[0] == 0x6b);  // 27520 = 0x6b80, network byte order
    CHECK(sa.port[1] == 0x80);
    CHECK(sa.addr[0] == 192);
    CHECK(sa.addr[3] == 20);
    CoopAddr back{};
    CHECK(coop_switch_net::FromSwitch(&sa, 16, back));
    CHECK(back.family == 4);
    CHECK(back.port == 27520);
    CHECK(back.addr[0] == 192);
    CHECK(back.addr[3] == 20);

    CoopAddr v6{};
    v6.family = 6;
    CHECK_FALSE(coop_switch_net::ToSwitch(v6, sa));
    sa.family = 28;
    CHECK_FALSE(coop_switch_net::FromSwitch(&sa, 16, back));
    CHECK(back.family == 0);
    CHECK_FALSE(coop_switch_net::FromSwitch(&sa, 4, back));

    uint8_t ip[4];
    CHECK(coop_switch_net::ParseIpv4("127.0.0.1", ip));
    CHECK(ip[0] == 127);
    CHECK(ip[3] == 1);
    CHECK(coop_switch_net::ParseIpv4("255.255.255.255", ip));
    for (const char *bad : {"", "1.2.3", "1.2.3.4.", "256.0.0.1", "1.2.3.4x", "1..2.3", "0127.0.0.1", "127.1",
                            "host.example", " 1.2.3.4"})
        CHECK_MESSAGE(!coop_switch_net::ParseIpv4(bad, ip), bad);
}

TEST_CASE("switch net: open, bind, local address and refusals") {
    Reset();
    CHECK(coop_plat_sock_open(COOP_SOCK_UDP6) == -1);
    CHECK(coop_plat_sock_open(99) == -1);
    const int s = coop_plat_sock_open(COOP_SOCK_UDP4);
    REQUIRE(s >= 0);
    CoopAddr any{};
    any.family = 4;
    CHECK(coop_plat_sock_bind(s, &any) == 0);
    CoopAddr local{};
    CHECK(coop_plat_sock_local(s, &local) == 0);
    CHECK(local.family == 4);
    CHECK(local.port == 50000);
    CHECK(coop_plat_sock_bind(s, nullptr) == -1);
    CoopAddr v6{};
    v6.family = 6;
    CHECK(coop_plat_sock_bind(s, &v6) == -1);

    // The game's own descriptors (and anything we did not open) are never touched.
    char buf[8];
    CHECK(coop_plat_sock_bind(1, &any) == -1);
    CHECK(coop_plat_sock_local(1, &local) == -1);
    CHECK(coop_plat_sock_sendto(1, "x", 1, &any) == -1);
    CHECK(coop_plat_sock_recvfrom(1, buf, sizeof buf, nullptr) == -1);
    CHECK(coop_plat_sock_close(1) == -1);
    CHECK(coop_plat_sock_close(-1) == -1);
    CHECK(g_fake.closes == 0);

    CHECK(coop_plat_sock_close(s) == 0);
    CHECK(coop_plat_sock_close(s) == -1);  // only once
    CHECK(g_fake.closes == 1);
    CHECK(g_fake.bad_layout == 0);
}

TEST_CASE("switch net: datagrams round trip without ever blocking") {
    Reset();
    const int a = coop_plat_sock_open(COOP_SOCK_UDP4);
    const int b = coop_plat_sock_open(COOP_SOCK_UDP4);
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    const CoopAddr at_b = Addr(0, 0, 0, 0, 27520);
    REQUIRE(coop_plat_sock_bind(b, &at_b) == 0);

    char buf[64];
    CoopAddr from{};
    CHECK(coop_plat_sock_recvfrom(b, buf, sizeof buf, &from) == COOP_SOCK_WOULD_BLOCK);

    const CoopAddr to_b = Addr(127, 0, 0, 1, 27520);
    CHECK(coop_plat_sock_sendto(a, "hello", 5, &to_b) == 5);
    const int got = coop_plat_sock_recvfrom(b, buf, sizeof buf, &from);
    REQUIRE(got == 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);
    CHECK(from.family == 4);
    CHECK(from.addr[0] == 127);
    CHECK(from.port == 50000);  // a's port, given on its first send

    // Echo back to where it came from.
    CHECK(coop_plat_sock_sendto(b, buf, 5, &from) == 5);
    CHECK(coop_plat_sock_recvfrom(a, buf, sizeof buf, nullptr) == 5);
    CHECK(coop_plat_sock_recvfrom(a, buf, sizeof buf, nullptr) == COOP_SOCK_WOULD_BLOCK);

    CHECK(coop_plat_sock_sendto(a, nullptr, 3, &to_b) == -1);
    CHECK(coop_plat_sock_sendto(a, buf, 70000, &to_b) == -1);
    CHECK(coop_plat_sock_sendto(a, buf, 1, nullptr) == -1);
    CHECK(coop_plat_sock_recvfrom(a, nullptr, 4, nullptr) == -1);
    CHECK(g_fake.blocking_calls == 0);
    CHECK(g_fake.bad_layout == 0);
    CHECK(coop_plat_sock_close(a) == 0);
    CHECK(coop_plat_sock_close(b) == 0);
}

TEST_CASE("switch net: at most eight sockets, the ninth is closed again") {
    Reset();
    int s[8];
    for (int &fd : s) {
        fd = coop_plat_sock_open(COOP_SOCK_UDP4);
        REQUIRE(fd >= 0);
    }
    CHECK(coop_plat_sock_open(COOP_SOCK_UDP4) == -1);
    CHECK(g_fake.closes == 1);
    CHECK(g_fake.fds.size() == 8);
    CHECK(coop_plat_sock_close(s[3]) == 0);
    CHECK(coop_plat_sock_open(COOP_SOCK_UDP4) >= 0);  // the freed slot is reused

    // The game ran out of descriptors: refused, nothing remembered.
    Reset();
    g_fake.max_fds = 0;
    CHECK(coop_plat_sock_open(COOP_SOCK_UDP4) == -1);
    for (int fd : g_owned) CHECK(fd == -1);
}

TEST_CASE("switch net: resolve parses literals itself and asks the resolver for names") {
    Reset();
    CoopAddr out{};
    out.port = 9;
    CHECK(coop_plat_resolve("192.168.0.7", COOP_SOCK_UDP4, &out) == 0);
    CHECK(out.family == 4);
    CHECK(out.port == 0);
    CHECK(out.addr[0] == 192);
    CHECK(out.addr[3] == 7);
    CHECK(g_fake.resolver_calls == 0);

    CHECK(coop_plat_resolve("host.example", COOP_SOCK_UDP4, &out) == 0);
    CHECK(out.family == 4);
    CHECK(out.port == 0);  // the resolver's 4242 is dropped
    CHECK(out.addr[0] == 10);
    CHECK(out.addr[3] == 3);
    CHECK(g_fake.resolver_calls == 1);
    CHECK(g_fake.frees == 1);

    CHECK(coop_plat_resolve("nowhere.example", COOP_SOCK_UDP4, &out) == -1);
    CHECK(out.family == 0);
    CHECK(coop_plat_resolve("", COOP_SOCK_UDP4, &out) == -1);
    CHECK(coop_plat_resolve("127.0.0.1", COOP_SOCK_UDP6, &out) == -1);
    CHECK(coop_plat_resolve(nullptr, COOP_SOCK_UDP4, &out) == -1);
    CHECK(coop_plat_resolve("127.0.0.1", COOP_SOCK_UDP4, nullptr) == -1);
    CHECK(g_fake.resolver_calls == 2);
}
