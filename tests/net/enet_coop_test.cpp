// SPDX-License-Identifier: GPL-3.0-or-later
// enet_coop (coop/net/enet_coop, v1 step 8c): stock ENet's protocol code on CoopPlatform's sock_* slots, here the
// PC std backend's. A real ENet server and client connect over 127.0.0.1, trade packets and disconnect, so the
// socket layer the ExeFS payload uses is proven end to end on the host. Its own program: enet_coop defines the
// same enet_* symbols as the PC's ENet.
#include <doctest/doctest.h>

#include <enet/enet.h>

#include "coop/os.hpp"
#include "enet_coop.h"

#include <cstring>
#include <string>

namespace {

struct Started {
    Started() {
        coop_enet_use_platform(coop::os::StdPlatform());
        ok = enet_initialize() == 0;
    }
    ~Started() {
        enet_deinitialize();
        coop_enet_use_platform(nullptr);
    }
    bool ok = false;
};

ENetAddress Loopback(enet_uint16 port) {
    ENetAddress a{};
    REQUIRE(enet_address_set_host_ip(&a, "127.0.0.1") == 0);
    a.port = port;
    return a;
}

// Services both hosts (1 ms waits) until `done` or about five seconds pass.
template <typename Done>
bool Pump(ENetHost *a, ENetHost *b, Done done) {
    for (int i = 0; i < 2500; ++i) {
        ENetEvent event;
        for (ENetHost *h : {a, b}) {
            while (h && enet_host_service(h, &event, 1) > 0) {
                if (done(h, event)) return true;
                if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("enet_coop: no platform, no start") {
    coop_enet_use_platform(nullptr);
    CHECK(enet_initialize() != 0);
    CHECK(enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM) == ENET_SOCKET_NULL);
    CoopPlatform missing = *coop::os::StdPlatform();
    missing.sock_recvfrom = nullptr;
    coop_enet_use_platform(&missing);
    CHECK(enet_initialize() != 0);
    coop_enet_use_platform(nullptr);
}

TEST_CASE("enet_coop: addresses") {
    Started started;
    REQUIRE(started.ok);
    ENetAddress a{};
    a.port = 27520;
    REQUIRE(enet_address_set_host_ip(&a, "10.0.200.7") == 0);
    const auto *b = reinterpret_cast<const unsigned char *>(&a.host);  // network byte order
    CHECK(b[0] == 10);
    CHECK(b[1] == 0);
    CHECK(b[2] == 200);
    CHECK(b[3] == 7);
    CHECK(a.port == 27520);
    char text[16];
    CHECK(enet_address_get_host_ip(&a, text, sizeof text) == 0);
    CHECK(std::string(text) == "10.0.200.7");
    CHECK(enet_address_get_host(&a, text, sizeof text) == 0);
    CHECK(std::string(text) == "10.0.200.7");
    CHECK(enet_address_get_host_ip(&a, text, 10) == -1);  // "10.0.200.7" + NUL needs 11
    CHECK(enet_address_get_host_ip(&a, text, 11) == 0);

    REQUIRE(enet_address_set_host_ip(&a, "255.255.255.255") == 0);
    CHECK(a.host == ENET_HOST_BROADCAST);
    CHECK(enet_address_get_host_ip(&a, text, sizeof text) == 0);
    CHECK(std::string(text) == "255.255.255.255");

    for (const char *bad : {"", "1.2.3", "256.1.1.1", "1.2.3.4 ", "localhost"})
        CHECK_MESSAGE(enet_address_set_host_ip(&a, bad) == -1, bad);

    // A name goes to the platform's resolver; a literal never does.
    ENetAddress named{};
    named.port = 7;
    REQUIRE(enet_address_set_host(&named, "localhost") == 0);
    CHECK(enet_address_get_host_ip(&named, text, sizeof text) == 0);
    CHECK(std::string(text) == "127.0.0.1");
    CHECK(named.port == 7);
    CHECK(enet_address_set_host(&named, "192.168.5.6") == 0);
    CHECK(enet_address_get_host_ip(&named, text, sizeof text) == 0);
    CHECK(std::string(text) == "192.168.5.6");
}

TEST_CASE("enet_coop: socket layer edges") {
    Started started;
    REQUIRE(started.ok);
    CHECK(enet_socket_create(ENET_SOCKET_TYPE_STREAM) == ENET_SOCKET_NULL);
    const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    REQUIRE(s != ENET_SOCKET_NULL);
    CHECK(enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1) == 0);
    CHECK(enet_socket_set_option(s, ENET_SOCKOPT_RCVBUF, 1 << 18) == 0);
    CHECK(enet_socket_listen(s, 1) == -1);
    CHECK(enet_socket_accept(s, nullptr) == ENET_SOCKET_NULL);
    CHECK(enet_socket_shutdown(s, ENET_SOCKET_SHUTDOWN_READ_WRITE) == -1);
    CHECK(enet_socketset_select(s, nullptr, nullptr, 0) == -1);
    int error = 7;
    CHECK(enet_socket_get_option(s, ENET_SOCKOPT_ERROR, &error) == 0);
    CHECK(error == 0);

    ENetAddress any{};
    REQUIRE(enet_socket_bind(s, &any) == 0);
    ENetAddress local{};
    REQUIRE(enet_socket_get_address(s, &local) == 0);
    CHECK(local.port != 0);

    // Nothing waiting: a receive is 0 (not an error), and wait says "try a receive" without blocking long.
    unsigned char buf[64];
    ENetBuffer in{};
    in.data = buf;
    in.dataLength = sizeof buf;
    ENetAddress from{};
    CHECK(enet_socket_receive(s, &from, &in, 1) == 0);
    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    const enet_uint32 before = enet_time_get();
    CHECK(enet_socket_wait(s, &condition, 0) == 0);
    CHECK(condition == ENET_SOCKET_WAIT_RECEIVE);
    condition = ENET_SOCKET_WAIT_SEND;
    CHECK(enet_socket_wait(s, &condition, 5) == 0);
    CHECK(condition == ENET_SOCKET_WAIT_NONE);
    CHECK(enet_time_get() - before < 1000);

    // Scatter/gather: two buffers out to ourselves, two buffers in.
    const ENetAddress self = Loopback(local.port);
    ENetBuffer out[2];
    char head[] = "abc", tail[] = "defg";
    out[0].data = head;
    out[0].dataLength = 3;
    out[1].data = tail;
    out[1].dataLength = 4;
    CHECK(enet_socket_send(s, &self, out, 2) == 7);
    unsigned char first[2], second[16];
    ENetBuffer parts[2];
    parts[0].data = first;
    parts[0].dataLength = sizeof first;
    parts[1].data = second;
    parts[1].dataLength = sizeof second;
    int got = 0;
    for (int i = 0; i < 1000 && got == 0; ++i) {
        got = enet_socket_receive(s, &from, parts, 2);
        if (got == 0) coop::os::Platform().sleep_us(1000);
    }
    REQUIRE(got == 7);
    CHECK(std::memcmp(first, "ab", 2) == 0);
    CHECK(std::memcmp(second, "cdefg", 5) == 0);
    CHECK(from.port == local.port);

    // A send the platform refuses is dropped (0), not an error that would end enet_host_service.
    enet_socket_destroy(s);
    CHECK(enet_socket_send(s, &self, out, 1) == 0);
    CHECK(enet_socket_receive(s, &from, &in, 1) == -1);

    // Time moves with the platform clock from the base enet_time_set gives.
    enet_time_set(1000);
    const enet_uint32 t = enet_time_get();
    CHECK(t >= 1000);
    CHECK(t < 2000);
}

TEST_CASE("enet_coop: a real ENet connect, packets both ways, disconnect") {
    Started started;
    REQUIRE(started.ok);
    ENetAddress bind_at = Loopback(0);
    ENetHost *server = enet_host_create(&bind_at, 4, 2, 0, 0);
    REQUIRE(server != nullptr);
    REQUIRE(server->address.port != 0);
    ENetHost *client = enet_host_create(nullptr, 1, 2, 0, 0);
    REQUIRE(client != nullptr);

    const ENetAddress to_server = Loopback(server->address.port);
    ENetPeer *peer = enet_host_connect(client, &to_server, 2, 42);
    REQUIRE(peer != nullptr);

    bool client_connected = false, server_connected = false;
    enet_uint32 data_seen = 0;
    CHECK(Pump(client, server, [&](ENetHost *h, const ENetEvent &e) {
        if (e.type == ENET_EVENT_TYPE_CONNECT) {
            (h == client ? client_connected : server_connected) = true;
            if (h == server) data_seen = e.data;
        }
        return client_connected && server_connected;
    }));
    CHECK(data_seen == 42);

    // Reliable client -> server, big enough to fragment (several datagrams through enet_socket_send).
    std::string big(5000, 'x');
    big[0] = 'h';
    big[4999] = 'z';
    ENetPacket *packet = enet_packet_create(big.data(), big.size(), ENET_PACKET_FLAG_RELIABLE);
    REQUIRE(enet_peer_send(peer, 0, packet) == 0);
    std::string server_got;
    ENetPeer *client_at_server = nullptr;
    CHECK(Pump(client, server, [&](ENetHost *h, const ENetEvent &e) {
        if (h != server || e.type != ENET_EVENT_TYPE_RECEIVE) return false;
        server_got.assign(reinterpret_cast<const char *>(e.packet->data), e.packet->dataLength);
        client_at_server = e.peer;
        enet_packet_destroy(e.packet);
        return true;
    }));
    CHECK(server_got == big);
    REQUIRE(client_at_server != nullptr);

    // And back.
    packet = enet_packet_create("pong", 4, ENET_PACKET_FLAG_RELIABLE);
    REQUIRE(enet_peer_send(client_at_server, 1, packet) == 0);
    std::string client_got;
    CHECK(Pump(client, server, [&](ENetHost *h, const ENetEvent &e) {
        if (h != client || e.type != ENET_EVENT_TYPE_RECEIVE) return false;
        client_got.assign(reinterpret_cast<const char *>(e.packet->data), e.packet->dataLength);
        CHECK(e.channelID == 1);
        enet_packet_destroy(e.packet);
        return true;
    }));
    CHECK(client_got == "pong");

    enet_peer_disconnect(peer, 7);
    bool server_saw_disconnect = false;
    CHECK(Pump(client, server, [&](ENetHost *h, const ENetEvent &e) {
        if (h == server && e.type == ENET_EVENT_TYPE_DISCONNECT) {
            server_saw_disconnect = e.data == 7;
            return true;
        }
        return false;
    }));
    CHECK(server_saw_disconnect);

    enet_host_destroy(client);
    enet_host_destroy(server);
}
