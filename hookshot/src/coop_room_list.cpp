// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_room_list.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <enet/enet.h>

#include "common/logging.h"
#include "relay_link.hpp"

namespace CoopRoomList {

std::string CellText(const Room& r, int my_protocol) {
    std::string t = r.room;
    if (r.protocol != my_protocol) return t + " (other version)";
    if (r.max > 0 && r.players >= r.max) return t + " (full)";
    if (r.max > 0) return t + " (" + std::to_string(r.players) + "/" + std::to_string(r.max) + ")";
    return t;
}

bool Joinable(const Room& r, int my_protocol) {
    return r.protocol == my_protocol && (r.max <= 0 || r.players < r.max);
}

namespace {

std::mutex g_mutex;
Snapshot g_now;
std::uint64_t g_asked_ms = 0, g_wanted_ms = 0;
std::string g_relay_asked;
std::atomic<bool> g_busy{false};

void Ask(std::string relay) {
    namespace relay_ns = coop::server::relay;
    Snapshot s;
    std::string host;
    std::uint16_t port = 0;
    if (!relay_ns::parse_relay_address(relay, host, port)) {
        s.state = "Relay address not right";
    } else if (enet_initialize() != 0) {
        s.state = "No network here";
    } else {
        ENetAddress at{};
        if (enet_address_set_host(&at, host.c_str()) != 0) {
            s.state = "The relay was not found";
        } else {
            at.port = port;
            if (const auto list = relay_ns::list_rooms_on_relay(at)) {
                for (const auto& e : list->rooms) {
                    s.rooms.push_back(Room{e.room, e.host, e.players, e.max_players, e.version});
                }
                if (s.rooms.empty()) s.state = "No rooms listed";
            } else {
                s.state = "The relay has no list";   // (no answer: a relay from before the list, or none there)
            }
        }
        enet_deinitialize();
    }
    LOG_INFO(Core, "coop: room list: {} room(s) from the relay{}", s.rooms.size(),
             s.state.empty() ? std::string() : " (" + s.state + ")");
    std::lock_guard lock(g_mutex);
    g_now = std::move(s);
    g_busy = false;
}

}  // namespace

void Look(bool wanted, std::uint64_t now_ms, const std::string& relay) {
    std::lock_guard lock(g_mutex);
    if (!wanted) {
        if (g_wanted_ms != 0 && now_ms - g_wanted_ms > kKeepMs) g_now = Snapshot{}, g_wanted_ms = 0, g_asked_ms = 0;
        return;
    }
    g_wanted_ms = now_ms;
    if (relay.empty()) {
        g_now = Snapshot{};
        g_now.state = "No relay set";
        g_asked_ms = 0;
        return;
    }
    if (relay != g_relay_asked) g_asked_ms = 0, g_now = Snapshot{};   // (another relay: its own list)
    if (g_busy || (g_asked_ms != 0 && now_ms - g_asked_ms < kEveryMs)) return;
    g_asked_ms = now_ms, g_relay_asked = relay, g_busy = true;
    std::thread(Ask, relay).detach();   // (a few seconds at most: list_rooms_on_relay's own tries)
}

void AskNow() {
    std::lock_guard lock(g_mutex);
    if (!g_busy) g_asked_ms = 0, g_now = Snapshot{};
}

Snapshot Now() {
    std::lock_guard lock(g_mutex);
    return g_now;
}

}  // namespace CoopRoomList
