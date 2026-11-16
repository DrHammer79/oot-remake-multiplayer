// SPDX-License-Identifier: GPL-3.0-or-later
// The ExeFS adapter's pure OS helpers (switch/adapter/os_util.h).
#include <doctest/doctest.h>

#include "../../switch/adapter/os_util.h"

#include <coop/os.hpp>

#include <cstring>
#include <string>

using coop_os_util::JoinPath;
using coop_os_util::TicksToUs;

TEST_CASE("exefs os: ticks to microseconds at any frequency, no overflow") {
    CHECK(TicksToUs(0, 19200000) == 0);
    CHECK(TicksToUs(19200000, 19200000) == 1000000);
    CHECK(TicksToUs(192, 19200000) == 10);
    CHECK(TicksToUs(31250000, 31250000) == 1000000);
    CHECK(TicksToUs(1, 1000000) == 1);
    // a year of ticks at 19.2 MHz: ticks * 1e6 alone would overflow 64 bits
    const std::uint64_t year = 19200000ull * 3600 * 24 * 365;
    CHECK(TicksToUs(year, 19200000) == 1000000ull * 3600 * 24 * 365);
    CHECK(TicksToUs(123, 0) == 0);
    CHECK(TicksToUs(123, -5) == 0);
}

TEST_CASE("exefs os: the mod path rule is coop::os's") {
    for (const char *p : {"a", "a/b.txt", "dir/sub/x.bin", "", "/a", "a//b", "a/", "../a", "a/../b", "a\\b", "c:/x",
                          "..", "...", "a/.."}) {
        CAPTURE(p);
        CHECK(coop_os_util::IsModPath(p) == coop::os::IsModPath(p));
    }
    CHECK_FALSE(coop_os_util::IsModPath(nullptr));
}

TEST_CASE("exefs os: paths join under the root and refuse what does not fit") {
    char out[32];
    CHECK(JoinPath(out, sizeof out, "coopsd:/config/", "a/b.txt", nullptr));
    CHECK(std::string(out) == "coopsd:/config/a/b.txt");
    CHECK(JoinPath(out, sizeof out, "r:/", "x", ".tmp"));
    CHECK(std::string(out) == "r:/x.tmp");
    CHECK_FALSE(JoinPath(out, sizeof out, "r:/", "../x", nullptr));
    CHECK(out[0] == '\0');
    CHECK_FALSE(JoinPath(out, sizeof out, "coopsd:/config/oot-coop/", "much/too/long.bin", nullptr));
    CHECK(out[0] == '\0');
    char exact[8];
    CHECK(JoinPath(exact, sizeof exact, "r:/", "abc", nullptr));  // 6 chars + NUL
    CHECK(JoinPath(exact, sizeof exact, "r:/", "abcd", nullptr));  // 7 + NUL: fits exactly
    CHECK_FALSE(JoinPath(exact, sizeof exact, "r:/", "abcde", nullptr));
}

TEST_CASE("exefs os: FNV-1a matches the published test values") {
    CHECK(coop_os_util::Fnv1a("", 0) == 0xcbf29ce484222325ull);
    CHECK(coop_os_util::Fnv1a("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(coop_os_util::Fnv1a("foobar", 6) == 0x85944171f73967e8ull);
}
