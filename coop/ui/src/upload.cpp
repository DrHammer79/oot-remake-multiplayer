// SPDX-License-Identifier: GPL-3.0-or-later
// coop/ui/upload.h: the draw list's hash and the per-texture pending rows of the picture.
#include "coop/ui/upload.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint64_t kPrime = 0x100000001b3ull;

std::uint64_t Mix(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * kPrime;
    }
    return h;
}

}  // namespace

extern "C" {

std::uint64_t coop_draw_list_hash(const CoopDrawList* l) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    if (l == nullptr) {
        return h;
    }
    const std::uint32_t n = l->n < COOP_DRAW_MAX_CMDS ? l->n : COOP_DRAW_MAX_CMDS;
    const std::uint32_t t = l->text_used < COOP_DRAW_MAX_TEXT ? l->text_used : COOP_DRAW_MAX_TEXT;
    h = Mix(h, &l->width, sizeof l->width);
    h = Mix(h, &l->height, sizeof l->height);
    h = Mix(h, &n, sizeof n);
    h = Mix(h, l->cmd, sizeof(CoopDrawCmd) * n);  // coop_draw_* zero each command first: no stray padding bytes
    return Mix(h, l->text, t);
}

int coop_upload_init(CoopUpload* u, int height, int buffers, int gap, std::uint8_t* rows) {
    if (u == nullptr || rows == nullptr || height <= 0 || height > COOP_RASTER_MAX_SIDE || buffers < 1 ||
        buffers > COOP_UPLOAD_MAX_BUFFERS || gap < 0) {
        return 0;
    }
    u->height = height;
    u->buffers = buffers;
    u->gap = gap;
    u->rows = rows;
    std::memset(rows, static_cast<int>((1u << buffers) - 1u), static_cast<std::size_t>(height));
    return 1;
}

void coop_upload_mark(CoopUpload* u, const CoopRasterBand* bands, int n) {
    if (u == nullptr || u->rows == nullptr || bands == nullptr) {
        return;
    }
    const auto all = static_cast<std::uint8_t>((1u << u->buffers) - 1u);
    for (int i = 0; i < n; ++i) {
        const int y1 = bands[i].y1 < u->height ? bands[i].y1 : u->height;
        for (int y = bands[i].y0; y < y1; ++y) {
            u->rows[y] = all;
        }
    }
}

int coop_upload_pending(const CoopUpload* u, int buffer) {
    if (u == nullptr || u->rows == nullptr || buffer < 0 || buffer >= u->buffers) {
        return 0;
    }
    const auto bit = static_cast<std::uint8_t>(1u << buffer);
    int n = 0;
    for (int y = 0; y < u->height; ++y) {
        n += (u->rows[y] & bit) != 0 ? 1 : 0;
    }
    return n;
}

int coop_upload_take(CoopUpload* u, int buffer, int max_rows, CoopRasterBand* out, int max_bands, int* complete) {
    if (complete != nullptr) {
        *complete = 0;
    }
    if (u == nullptr || u->rows == nullptr || buffer < 0 || buffer >= u->buffers || out == nullptr) {
        return 0;
    }
    const auto bit = static_cast<std::uint8_t>(1u << buffer);
    int n = 0;
    int budget = max_rows;
    int y = 0;
    while (y < u->height && budget > 0 && max_bands > 0) {
        if ((u->rows[y] & bit) == 0) {
            ++y;
            continue;
        }
        // A band starts here: take pending rows, and across gaps shorter than u->gap, while the budget lasts.
        const int y0 = y;
        int end = y0;  // one past the last pending row taken
        int scan = y0;
        while (scan < u->height && scan - y0 < budget) {
            if ((u->rows[scan] & bit) != 0) {
                end = scan + 1;
                ++scan;
                continue;
            }
            int next = scan;
            while (next < u->height && (u->rows[next] & bit) == 0 && next - scan < u->gap) {
                ++next;
            }
            if (next < u->height && (u->rows[next] & bit) != 0 && next - scan < u->gap && next - y0 < budget) {
                scan = next;  // a short gap: carry on in the same band
                continue;
            }
            break;
        }
        if (n == max_bands) {
            break;
        }
        for (int r = y0; r < end; ++r) {
            u->rows[r] = static_cast<std::uint8_t>(u->rows[r] & ~bit);
        }
        out[n].y0 = static_cast<std::uint16_t>(y0);
        out[n].y1 = static_cast<std::uint16_t>(end);
        ++n;
        budget -= end - y0;
        y = end;
        if (n == max_bands) {
            break;
        }
    }
    if (complete != nullptr) {
        *complete = coop_upload_pending(u, buffer) == 0 ? 1 : 0;
    }
    return n;
}

}  // extern "C"
