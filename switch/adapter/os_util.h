// Small pure parts of the ExeFS adapter's OS slots (platform_os.cpp), here so the PC tests can check them
// (tests/switch/os_util_test.cpp). No library: they build freestanding into the module.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace coop_os_util {

// System ticks to microseconds without overflow for any tick count a console reaches (the frequency is read from the
// system, never assumed: 19.2 MHz on Switch 1, other values elsewhere). 0 when the frequency is not positive.
inline uint64_t TicksToUs(uint64_t ticks, int64_t freq) {
    if (freq <= 0) return 0;
    const uint64_t f = static_cast<uint64_t>(freq);
    return ticks / f * 1000000u + ticks % f * 1000000u / f;
}

// A path the platform's file slots take (the same rule as coop::os::IsModPath): relative, '/' separators, no empty
// part, no "..", no ':' or '\\'.
inline bool IsModPath(const char *rel) {
    if (!rel || !*rel || rel[0] == '/') return false;
    const char *part = rel;
    for (const char *c = rel;; ++c) {
        if (*c == '\\' || *c == ':') return false;
        if (*c == '/' || *c == '\0') {
            const size_t len = static_cast<size_t>(c - part);
            if (len == 0) return false;
            if (len == 2 && part[0] == '.' && part[1] == '.') return false;
            if (*c == '\0') return true;
            part = c + 1;
        }
    }
}

// out = root + rel + suffix, NUL-terminated. False (out = "") when rel is not a mod path or it does not fit.
inline bool JoinPath(char *out, size_t cap, const char *root, const char *rel, const char *suffix) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!root || !IsModPath(rel)) return false;
    size_t n = 0;
    const char *parts[3] = {root, rel, suffix ? suffix : ""};  // (an array: no <initializer_list> freestanding)
    for (const char *part : parts) {
        for (const char *c = part; *c; ++c) {
            if (n + 1 >= cap) {
                out[0] = '\0';
                return false;
            }
            out[n++] = *c;
        }
    }
    out[n] = '\0';
    return true;
}

// FNV-1a, 64 bits: the self-test's file hash.
inline uint64_t Fnv1a(const void *data, size_t n) {
    uint64_t h = 0xcbf29ce484222325u;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3u;
    return h;
}

}  // namespace coop_os_util
