// SPDX-License-Identifier: GPL-3.0-or-later
// ENet's platform layer (what unix.c / win32.c define) over CoopPlatform's sock_* and time slots. See enet_coop.h.
// No libc and no std: the same file builds into the freestanding ExeFS payload.
#include "enet_coop.h"

#include <enet/enet.h>

#ifdef COOP_PLATFORM_STATIC
#include "coop_platform_static.h"
#define COOP_ENET_CALL(slot, fail, ...) coop_plat_##slot(__VA_ARGS__)
#endif

namespace {

#ifndef COOP_PLATFORM_STATIC
const CoopPlatform *g_platform = nullptr;
#define COOP_ENET_CALL(slot, fail, ...) (g_platform && g_platform->slot ? g_platform->slot(__VA_ARGS__) : (fail))
#endif

// ENet keeps time in milliseconds from a base that enet_time_set moves.
uint64_t g_time_base_ms = 0;

uint64_t NowMs() { return COOP_ENET_CALL(now_us, uint64_t{0}) / 1000; }

int Handle(ENetSocket socket) { return socket == ENET_SOCKET_NULL ? -1 : static_cast<int>(socket); }

// ENetAddress.host is in network byte order, so its bytes in memory are the address's bytes in order.
CoopAddr ToCoop(const ENetAddress &a) {
    CoopAddr out{};
    out.family = 4;
    out.port = a.port;
    const auto *b = reinterpret_cast<const uint8_t *>(&a.host);
    for (int i = 0; i < 4; ++i) out.addr[i] = b[i];
    return out;
}

bool FromCoop(const CoopAddr &in, ENetAddress &out) {
    if (in.family != 4) return false;
    auto *b = reinterpret_cast<uint8_t *>(&out.host);
    for (int i = 0; i < 4; ++i) b[i] = in.addr[i];
    out.port = in.port;
    return true;
}

// Only a dotted quad, as inet_pton(AF_INET) takes it.
bool ParseIp(const char *s, uint8_t out[4]) {
    if (!s) return false;
    for (int part = 0; part < 4; ++part) {
        if (part > 0 && *s++ != '.') return false;
        int value = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (*s++ - '0');
        }
        if (digits == 0 || value > 255) return false;
        out[part] = static_cast<uint8_t>(value);
    }
    return *s == '\0';
}

size_t Total(const ENetBuffer *buffers, size_t count) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) n += buffers[i].dataLength;
    return n;
}

}  // namespace

extern "C" {

#ifndef COOP_PLATFORM_STATIC
void coop_enet_use_platform(const CoopPlatform *platform) { g_platform = platform; }
#endif

int enet_initialize(void) {
#ifdef COOP_PLATFORM_STATIC
    return 0;
#else
    const CoopPlatform *p = g_platform;
    const bool ok = p && p->sock_open && p->sock_bind && p->sock_local && p->sock_sendto && p->sock_recvfrom &&
                    p->sock_close && p->resolve && p->now_us && p->sleep_us;
    return ok ? 0 : -1;
#endif
}

void enet_deinitialize(void) {}

#ifdef COOP_PLATFORM_STATIC
// The payload builds ENet without callbacks.c: its allocator table is called through pointers (a lifter refuses
// those) and defaults to libc malloc/abort, which the payload does not have. Memory comes from the platform's
// arena; out of memory is NULL (ENet checks it) and one log line, never abort inside the game.
void *enet_malloc(size_t size) {
    void *memory = coop_plat_mem_alloc(size, 16);
    static bool told = false;
    if (!memory && !told) {
        told = true;
        coop_plat_log("enet: out of memory (the arena is full)");
    }
    return memory;
}

void enet_free(void *memory) { coop_plat_mem_free(memory); }

ENetVersion enet_linked_version(void) { return ENET_VERSION; }

// Own allocators cannot be honoured here: refused, so a caller never believes its callbacks are in use.
int enet_initialize_with_callbacks(ENetVersion version, const ENetCallbacks *inits) {
    if (version < ENET_VERSION_CREATE(1, 3, 0)) return -1;
    if (inits && (inits->malloc || inits->free || inits->no_memory)) return -1;
    return enet_initialize();
}
#endif

enet_uint32 enet_host_random_seed(void) {
    return static_cast<enet_uint32>(COOP_ENET_CALL(now_us, uint64_t{0}) * 2654435761u);
}

enet_uint32 enet_time_get(void) { return static_cast<enet_uint32>(NowMs() - g_time_base_ms); }

void enet_time_set(enet_uint32 new_time_base) { g_time_base_ms = NowMs() - new_time_base; }

int enet_address_set_host_ip(ENetAddress *address, const char *name) {
    CoopAddr a{};
    if (!address || !ParseIp(name, a.addr)) return -1;
    a.family = 4;
    a.port = address->port;
    return FromCoop(a, *address) ? 0 : -1;
}

int enet_address_set_host(ENetAddress *address, const char *name) {
    if (!address || !name) return -1;
    if (enet_address_set_host_ip(address, name) == 0) return 0;
    CoopAddr a{};
    if (COOP_ENET_CALL(resolve, -1, name, COOP_SOCK_UDP4, &a) != 0) return -1;
    a.port = address->port;
    return FromCoop(a, *address) ? 0 : -1;
}

int enet_address_get_host_ip(const ENetAddress *address, char *name, size_t name_length) {
    if (!address || !name) return -1;
    const CoopAddr a = ToCoop(*address);
    char text[16];
    size_t n = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) text[n++] = '.';
        const int v = a.addr[i];
        if (v >= 100) text[n++] = static_cast<char>('0' + v / 100);
        if (v >= 10) text[n++] = static_cast<char>('0' + v / 10 % 10);
        text[n++] = static_cast<char>('0' + v % 10);
    }
    if (n + 1 > name_length) return -1;
    for (size_t i = 0; i < n; ++i) name[i] = text[i];
    name[n] = '\0';
    return 0;
}

int enet_address_get_host(const ENetAddress *address, char *name, size_t name_length) {
    return enet_address_get_host_ip(address, name, name_length);
}

ENetSocket enet_socket_create(ENetSocketType type) {
    if (type != ENET_SOCKET_TYPE_DATAGRAM) return ENET_SOCKET_NULL;
    const int s = COOP_ENET_CALL(sock_open, -1, COOP_SOCK_UDP4);
    return s < 0 ? ENET_SOCKET_NULL : static_cast<ENetSocket>(s);
}

int enet_socket_bind(ENetSocket socket, const ENetAddress *address) {
    CoopAddr local{};
    local.family = 4;  // NULL: any address, any port
    if (address) local = ToCoop(*address);
    return COOP_ENET_CALL(sock_bind, -1, Handle(socket), &local) == 0 ? 0 : -1;
}

int enet_socket_get_address(ENetSocket socket, ENetAddress *address) {
    CoopAddr local{};
    if (!address || COOP_ENET_CALL(sock_local, -1, Handle(socket), &local) != 0) return -1;
    return FromCoop(local, *address) ? 0 : -1;
}

int enet_socket_listen(ENetSocket, int) { return -1; }

ENetSocket enet_socket_accept(ENetSocket, ENetAddress *) { return ENET_SOCKET_NULL; }

int enet_socket_connect(ENetSocket, const ENetAddress *) { return -1; }

int enet_socket_shutdown(ENetSocket, ENetSocketShutdown) { return -1; }

// Platform sockets are non-blocking already; buffer sizes, broadcast and the rest are left to the platform.
int enet_socket_set_option(ENetSocket socket, ENetSocketOption, int) { return Handle(socket) < 0 ? -1 : 0; }

int enet_socket_get_option(ENetSocket socket, ENetSocketOption option, int *value) {
    if (Handle(socket) < 0 || !value || option != ENET_SOCKOPT_ERROR) return -1;
    *value = 0;
    return 0;
}

void enet_socket_destroy(ENetSocket socket) {
    if (Handle(socket) >= 0) COOP_ENET_CALL(sock_close, -1, Handle(socket));
}

int enet_socket_send(ENetSocket socket, const ENetAddress *address, const ENetBuffer *buffers, size_t count) {
    if (!address || (count && !buffers)) return -1;
    const CoopAddr to = ToCoop(*address);
    const size_t total = Total(buffers, count);
    int sent;
    if (count == 1) {
        sent = COOP_ENET_CALL(sock_sendto, -1, Handle(socket), buffers[0].data, total, &to);
    } else {
        uint8_t stage[ENET_PROTOCOL_MAXIMUM_MTU];
        if (total > sizeof stage) return -1;
        size_t at = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto *b = static_cast<const uint8_t *>(buffers[i].data);
            for (size_t k = 0; k < buffers[i].dataLength; ++k) stage[at++] = b[k];
        }
        sent = COOP_ENET_CALL(sock_sendto, -1, Handle(socket), stage, total, &to);
    }
    return sent < 0 ? 0 : sent;  // refused: dropped (enet_coop.h)
}

int enet_socket_receive(ENetSocket socket, ENetAddress *address, ENetBuffer *buffers, size_t count) {
    if (count == 0 || !buffers) return -1;
    CoopAddr from{};
    int got;
    if (count == 1) {
        got = COOP_ENET_CALL(sock_recvfrom, -1, Handle(socket), buffers[0].data, buffers[0].dataLength, &from);
    } else {
        uint8_t stage[ENET_PROTOCOL_MAXIMUM_MTU];
        const size_t cap = Total(buffers, count);
        got = COOP_ENET_CALL(sock_recvfrom, -1, Handle(socket), stage, cap < sizeof stage ? cap : sizeof stage,
                             &from);
        size_t at = 0;
        for (size_t i = 0; i < count && got > 0 && at < static_cast<size_t>(got); ++i) {
            auto *b = static_cast<uint8_t *>(buffers[i].data);
            for (size_t k = 0; k < buffers[i].dataLength && at < static_cast<size_t>(got); ++k) b[k] = stage[at++];
        }
    }
    if (got == COOP_SOCK_WOULD_BLOCK) return 0;
    if (got < 0) return -1;
    if (address && !FromCoop(from, *address)) {
        address->host = ENET_HOST_ANY;
        address->port = 0;
    }
    return got;
}

// The slots cannot poll: sleep one slice (1 ms; timeouts are whole ms, so never past one) and say "maybe readable", so
// enet_host_service tries a receive and loops until its own timeout (enet_coop.h).
int enet_socket_wait(ENetSocket socket, enet_uint32 *condition, enet_uint32 timeout) {
    if (!condition || Handle(socket) < 0) return -1;
    const enet_uint32 asked = *condition;
    *condition = ENET_SOCKET_WAIT_NONE;
    if (timeout > 0) COOP_ENET_CALL(sleep_us, (void)0, uint64_t{1000});  // whole ms: one is never past it
    if (asked & ENET_SOCKET_WAIT_RECEIVE) *condition = ENET_SOCKET_WAIT_RECEIVE;
    return 0;
}

int enet_socketset_select(ENetSocket, ENetSocketSet *, ENetSocketSet *, enet_uint32) { return -1; }

}  // extern "C"
