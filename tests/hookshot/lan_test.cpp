// SPDX-License-Identifier: GPL-3.0-or-later
// Games on this network (hookshot/src/coop_lan.h): the announcement's datagram and the list of games heard.
#include <doctest/doctest.h>

#include "coop_lan.h"

using namespace CoopLan;

TEST_CASE("lan: an announcement reads back as the game it names, and never says the join password") {
    Game g;
    g.name = "Saria";
    g.port = 27510;
    g.players = 2;
    g.max = 4;
    g.room = "forest-gang";
    g.protocol = 14;
    const std::string d = Encode(g);
    CHECK(d.rfind("SQUADALA LAN 1\n", 0) == 0);
    CHECK(d.find("password") == std::string::npos);
    const auto back = Decode(d);
    REQUIRE(back);
    CHECK(back->name == "Saria");
    CHECK(back->port == 27510);
    CHECK(back->players == 2);
    CHECK(back->max == 4);
    CHECK(back->room == "forest-gang");
    CHECK(back->protocol == 14);
}

TEST_CASE("lan: anything else is not a game") {
    CHECK_FALSE(Decode(""));
    CHECK_FALSE(Decode("hello"));
    CHECK_FALSE(Decode("SQUADALA LAN 10\nport=1\nprotocol=14\n"));   // (another format)
    CHECK_FALSE(Decode("SQUADALA LAN 1\nname=x\nprotocol=14\n"));    // (no port)
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=0\nprotocol=14\n"));
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=70000\nprotocol=14\n"));
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=27510\n"));              // (no protocol)
    CHECK_FALSE(Decode(std::string(600, 'x')));
    const auto odd = Decode("SQUADALA LAN 1\nport=27510\nprotocol=14\nlater=thing\nplayers=abc\n");
    REQUIRE(odd);   // (an unknown key and a bad count: left alone)
    CHECK(odd->players == 0);
    CHECK(odd->name == "A game");
}

TEST_CASE("lan: a name loses its control characters and is cut at 32 bytes, on a UTF-8 letter") {
    Game g;
    g.port = 1;
    g.name = "Sa\nria\x01";
    CHECK(Decode(Encode(g))->name == "Saria");
    g.name = std::string(31, 'a') + "\xc3\xa9" + "zz";   // (an e with an accent across the 32nd byte)
    const std::string name = Decode(Encode(g))->name;
    CHECK(name.size() == 31);
    CHECK(name == std::string(31, 'a'));
}

TEST_CASE("lan: the list keeps one row per address and port, drops the quiet ones, and keeps a steady order") {
    List l;
    Game a;
    a.name = "Saria", a.port = 27510, a.protocol = 14, a.max = 4, a.players = 1;
    Game b = a;
    b.name = "Mido";
    l.Add("192.168.1.20", a, 1000);
    l.Add("192.168.1.30", b, 1500);
    a.players = 2;
    l.Add("192.168.1.20", a, 2000);   // (the same game again: one row, its newest word)
    auto now = l.Now(2500);
    REQUIRE(now.size() == 2);
    CHECK(now[0].game.name == "Mido");   // (by name)
    CHECK(now[1].game.players == 2);
    CHECK(l.Now(1500 + kGoneMs).size() == 1);   // (Mido quiet that long: gone)
    CHECK(l.Now(2000 + kGoneMs).empty());
}

TEST_CASE("lan: a game's row says its players, full, or another version, and only those it can join join") {
    Heard h;
    h.game.name = "Saria", h.game.port = 27510, h.game.protocol = 14, h.game.players = 2, h.game.max = 4;
    CHECK(RowText(h, 14) == "Saria's game (2/4)");
    CHECK(Joinable(h, 14));
    h.game.players = 4;
    CHECK(RowText(h, 14) == "Saria's game (full)");
    CHECK_FALSE(Joinable(h, 14));
    h.game.players = 1;
    CHECK(RowText(h, 13) == "Saria's game (other version)");
    CHECK_FALSE(Joinable(h, 13));
}

TEST_CASE("lan: a game on this PC heard at 127.0.0.1 and at its network address is one row") {
    List l;
    Game g;
    g.name = "Saria", g.port = 27510, g.protocol = 14;
    l.Add("127.0.0.1", g, 1000);
    CHECK(l.Now(1100).size() == 1);   // (only the loopback: kept)
    l.Add("192.168.1.20", g, 1050);
    const auto now = l.Now(1100);
    REQUIRE(now.size() == 1);
    CHECK(now[0].address == "192.168.1.20");
}
