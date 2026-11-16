// SPDX-License-Identifier: GPL-3.0-or-later
// The one UTF-8 decoder of co-op's UI layer: the text measure (drawlist.cpp coop_text_width) and the software raster
// (raster.cpp) both walk text with it, so a drawn text's pen moves exactly as its measure counts, bad bytes included.
// Header-only, no library calls: it goes into the ExeFS payload too.
#ifndef COOP_UI_UTF8_HPP
#define COOP_UI_UTF8_HPP
#include <cstdint>

namespace coop::ui {

inline constexpr std::uint32_t kBadCodepoint = 0xFFFFFFFFu;

// One codepoint from [s, end) (s < end), s advanced past it. A bad byte gives kBadCodepoint and only that byte is
// consumed: a lead byte whose continuation bytes are missing or cut short leaves s right after the lead byte, so the
// bytes after it are read again on their own (each a codepoint or another bad byte). Not checked: overlong forms and
// surrogates decode as their value (a font has no glyph for them: they count as missing).
inline std::uint32_t DecodeUtf8(const unsigned char*& s, const unsigned char* end) {
    const unsigned char b = *s++;
    if (b < 0x80) {
        return b;
    }
    int more = 0;
    std::uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) {
        more = 1, cp = b & 0x1Fu;
    } else if ((b & 0xF0) == 0xE0) {
        more = 2, cp = b & 0x0Fu;
    } else if ((b & 0xF8) == 0xF0) {
        more = 3, cp = b & 0x07u;
    } else {
        return kBadCodepoint;
    }
    if (end - s < more) {
        return kBadCodepoint;
    }
    for (int i = 0; i < more; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            return kBadCodepoint;
        }
    }
    for (int i = 0; i < more; ++i) {
        cp = cp << 6 | (s[i] & 0x3Fu);
    }
    s += more;
    return cp;
}

}  // namespace coop::ui

#endif
