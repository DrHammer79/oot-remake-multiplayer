// SPDX-License-Identifier: GPL-3.0-or-later
// The Switch's socket address layout and the conversions to and from CoopAddr (ExeFS adapter, v1 step 8c).
// Freestanding: no libc, so the payload and the host tests share it. The Switch's BSD layer keeps the BSD
// layout: a length byte before the family byte (sin_len, sin_family), unlike Linux and Windows.
#ifndef COOP_SWITCH_SOCK_ADDR_H
#define COOP_SWITCH_SOCK_ADDR_H

#include <stddef.h>
#include <stdint.h>

#include "coop_platform.h"

namespace coop_switch_net {

constexpr int kAfInet = 2;
constexpr int kSockDgram = 2;
constexpr int kIpprotoUdp = 17;
// A receive or send given this flag never waits, whatever the socket's mode: no Fcntl(O_NONBLOCK) needed
// (Fritchii: bsd.cpp RecvFromImpl/SendToImpl, common/socket_types.h FLAG_MSG_DONTWAIT).
constexpr int kMsgDontWait = 0x80;

struct SockAddrIn {
    uint8_t len;         // sizeof(SockAddrIn)
    uint8_t family;      // kAfInet
    uint8_t port[2];     // network byte order
    uint8_t addr[4];     // network byte order
    uint8_t zero[8];
};
static_assert(sizeof(SockAddrIn) == 16, "sockaddr_in is 16 bytes on the Switch");

// CoopAddr (family 4) to the Switch's sockaddr_in. IPv6 is refused: the game's socket config has none.
inline bool ToSwitch(const CoopAddr &in, SockAddrIn &out) {
    if (in.family != 4) return false;
    out = SockAddrIn{};
    out.len = sizeof(SockAddrIn);
    out.family = kAfInet;
    out.port[0] = static_cast<uint8_t>(in.port >> 8);
    out.port[1] = static_cast<uint8_t>(in.port);
    for (int i = 0; i < 4; ++i) out.addr[i] = in.addr[i];
    return true;
}

// A sockaddr the Switch filled (len bytes at sa) to CoopAddr; false (out cleared) for anything but IPv4.
inline bool FromSwitch(const void *sa, uint32_t len, CoopAddr &out) {
    out = CoopAddr{};
    if (!sa || len < 8) return false;
    const auto *b = static_cast<const uint8_t *>(sa);
    if (b[1] != kAfInet) return false;
    out.family = 4;
    out.port = static_cast<uint16_t>((b[2] << 8) | b[3]);
    for (int i = 0; i < 4; ++i) out.addr[i] = b[4 + i];
    return true;
}

// A dotted IPv4 literal ("127.0.0.1"): exactly four parts of 1-3 digits, each at most 255. No other forms
// (inet_aton's "127.1" or hex) so a host name is never misread as an address.
inline bool ParseIpv4(const char *s, uint8_t out[4]) {
    if (!s) return false;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (*s != '.') return false;
            ++s;
        }
        int value = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (*s - '0');
            ++s;
        }
        if (digits == 0 || value > 255) return false;
        out[part] = static_cast<uint8_t>(value);
    }
    return *s == '\0';
}

}  // namespace coop_switch_net

#endif
