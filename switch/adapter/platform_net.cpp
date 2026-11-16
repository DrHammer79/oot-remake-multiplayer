// SPDX-License-Identifier: GPL-3.0-or-later
// CoopPlatform's network slots on the game's own socket library (ExeFS adapter, v1 step 8c): non-blocking UDP
// over nn::socket, so ENet runs on it (coop/net/enet_coop). Static binding (step 8 decision 1): the payload's
// coop::os calls these coop_plat_<slot> functions directly, no table.
// - nn::socket is already initialized by the game (prototype run): never Initialize or Finalize it here.
// - Non-blocking per call: every send and receive passes MSG_DONTWAIT, so no Fcntl and no errno import. A
//   receive that fails is reported as "nothing waiting", like the PC backend does for ICMP port-unreachable; a
//   bad handle is refused before the call. Send failures are -1 (ENet drops the datagram and retries later).
// - IPv4 only: the game's socket config has no IPv6 (COOP_SOCK_UDP6 is refused).
// - Handles are the sdk's descriptors (>= 0); we keep a small set of the ones we opened, so a stray handle
//   from the core can never close or read one of the game's own sockets.
#include "coop_platform_static.h"
#include "nn_socket.h"
#include "sock_addr.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // the host tests on Windows (MSVC has no __atomic builtins)
#endif

namespace {

#if defined(_MSC_VER) && !defined(__clang__)
static_assert(sizeof(long) == sizeof(int), "Interlocked works on long");
int AtomicLoad(int *p) {
    return static_cast<int>(_InterlockedCompareExchange(reinterpret_cast<volatile long *>(p), 0, 0));
}
bool AtomicSwap(int *p, int expected, int desired) {
    return _InterlockedCompareExchange(reinterpret_cast<volatile long *>(p), desired, expected) == expected;
}
#else
int AtomicLoad(int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
bool AtomicSwap(int *p, int expected, int desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

using namespace coop_switch_net;

// The game's socket config caps how many descriptors exist at once; the core needs one (ENet) plus spares.
constexpr int kMaxOwned = 8;
int g_owned[kMaxOwned] = {-1, -1, -1, -1, -1, -1, -1, -1};

// Slots change with compare-and-swap, so open/close on one thread and send/receive on another never tear.
bool Owned(int s) {
    if (s < 0) return false;
    for (int &fd : g_owned)
        if (AtomicLoad(&fd) == s) return true;
    return false;
}

bool Remember(int s) {
    for (int &fd : g_owned) {
        if (AtomicSwap(&fd, -1, s)) return true;
    }
    return false;
}

// True when s was ours (and now is not): only one closer wins.
bool Forget(int s) {
    for (int &fd : g_owned) {
        if (AtomicSwap(&fd, s, -1)) return true;
    }
    return false;
}

}  // namespace

extern "C" {

int coop_plat_sock_open(uint32_t sock_kind) {
    if (sock_kind != COOP_SOCK_UDP4) return -1;
    const int s = nn::socket::Socket(kAfInet, kSockDgram, kIpprotoUdp);
    if (s < 0) return -1;
    if (!Remember(s)) {
        nn::socket::Close(s);
        return -1;
    }
    return s;
}

int coop_plat_sock_bind(int s, const CoopAddr *local) {
    SockAddrIn sa;
    if (!Owned(s) || !local || !ToSwitch(*local, sa)) return -1;
    return nn::socket::Bind(s, reinterpret_cast<const ::sockaddr *>(&sa), sizeof sa) == 0 ? 0 : -1;
}

int coop_plat_sock_local(int s, CoopAddr *out) {
    if (!Owned(s) || !out) return -1;
    SockAddrIn sa{};
    unsigned int len = sizeof sa;
    if (nn::socket::GetSockName(s, reinterpret_cast<::sockaddr *>(&sa), &len) != 0) return -1;
    return FromSwitch(&sa, len, *out) ? 0 : -1;
}

int coop_plat_sock_sendto(int s, const void *data, size_t n, const CoopAddr *to) {
    SockAddrIn sa;
    if (!Owned(s) || !to || (n && !data) || n > 65507 || !ToSwitch(*to, sa)) return -1;
    const long sent = nn::socket::SendTo(s, data, n, kMsgDontWait, reinterpret_cast<const ::sockaddr *>(&sa),
                                         sizeof sa);
    return sent < 0 ? -1 : static_cast<int>(sent);
}

int coop_plat_sock_recvfrom(int s, void *buf, size_t cap, CoopAddr *from) {
    if (!Owned(s) || (cap && !buf)) return -1;
    SockAddrIn sa{};
    unsigned int len = sizeof sa;
    const long got = nn::socket::RecvFrom(s, buf, cap > 65535 ? 65535 : cap, kMsgDontWait,
                                          reinterpret_cast<::sockaddr *>(&sa), &len);
    if (got < 0) return COOP_SOCK_WOULD_BLOCK;
    if (from) FromSwitch(&sa, len, *from);
    return static_cast<int>(got);
}

int coop_plat_sock_close(int s) {
    if (s < 0 || !Forget(s)) return -1;
    return nn::socket::Close(s) == 0 ? 0 : -1;
}

// A literal is parsed here (no request to the resolver); a name asks the game's resolver (sfdnsres) for its
// first IPv4 address. Port left 0.
int coop_plat_resolve(const char *host, uint32_t sock_kind, CoopAddr *out) {
    if (!host || !out || sock_kind != COOP_SOCK_UDP4) return -1;
    *out = CoopAddr{};
    if (ParseIpv4(host, out->addr)) {
        out->family = 4;
        return 0;
    }
    if (*host == '\0') return -1;
    ::addrinfo hints{};
    hints.ai_family = kAfInet;
    hints.ai_socktype = kSockDgram;
    ::addrinfo *res = nullptr;
    if (nn::socket::GetAddrInfo(host, nullptr, &hints, &res) != 0 || !res) return -1;
    int result = -1;
    for (const ::addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == kAfInet && ai->ai_addr && FromSwitch(ai->ai_addr, ai->ai_addrlen, *out)) {
            out->port = 0;
            result = 0;
            break;
        }
    }
    nn::socket::FreeAddrInfo(res);
    return result;
}

}  // extern "C"
