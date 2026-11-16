// SPDX-License-Identifier: GPL-3.0-or-later
// Who has co-op UI input (docs/ui.md "Input"): a small stack of owners; the top one gets the events, and while any
// owner is on it the game's pad is masked (the adapter clears the buttons it hands the game). No allocation.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace coop::ui {

enum class Owner : std::uint8_t { none = 0, menu = 1, chat = 2, cheats = 3 };

class FocusStack {
public:
    // Puts `o` on top (moved there if it was lower). False for none.
    bool Push(Owner o) {
        if (o == Owner::none) {
            return false;
        }
        Remove(o);
        s_[n_++] = o;   // (each owner at most once: room for all of them)
        return true;
    }
    // Takes `o` off wherever it is; the one under it gets input again.
    void Pop(Owner o) { Remove(o); }
    Owner Top() const { return n_ == 0 ? Owner::none : s_[n_ - 1]; }
    bool Has(Owner o) const {
        for (std::size_t i = 0; i < n_; ++i) {
            if (s_[i] == o) return true;
        }
        return false;
    }
    bool MasksPad() const { return n_ > 0; }
    std::size_t Size() const { return n_; }

private:
    void Remove(Owner o) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (s_[i] != o) s_[w++] = s_[i];
        }
        n_ = w;
    }
    std::array<Owner, 3> s_{};
    std::size_t n_ = 0;
};

}  // namespace coop::ui
