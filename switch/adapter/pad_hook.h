// SPDX-License-Identifier: GPL-3.0-or-later
// The pad in the game's process (v1 step 8 (d), docs/exefs.md "Input"): what the module does with every
// nn::hid::GetNpadStates result the game asks for (switch/adapter/input.cpp redirects main's five imports here).
//
// For each result it
//   - keeps players 1-4's latest pad (buttons, sticks) as the game read it, and queues every button edge;
//   - feeds player 1's pad to co-op's UI reader (coop/ui/pad.hpp): CoopUiEvents into a queue, L+R then Minus opens
//     co-op's menu, Minus closes it;
//   - while the menu is open, hands the game player 1's states with no buttons and centred sticks, and afterwards
//     no button held while it was open until that button is let go (coop::ui::PadGate).
//
// Player 1 is npad No.1 or Handheld. Both can be connected at once (a controller and the console's own Joy-Con), so
// one of them is player 1's SOURCE for a whole frame: only the source is recorded and fed to the reader, the other is
// masked like the source (nothing while the menu has the pad). The source changes only when a new frame starts
// (Frame(), from the frame tick): to the other id when the source was not connected in the last frame and the other
// was, or when the other had input and the source had none.
//
// Threads: OnStates runs on the thread that reads the pad (the PRODUCER, one thread); Frame, the Take* calls, open(),
// reads(), the counters and RequestOpen run on the frame tick (the CONSUMER). They share only the SPSC queues and a
// few words, all through acquire/release atomics; pad() is for the producer's thread (and tests).
// Pure, no libc and no C++ library (atom:: below: GCC/Clang __atomic builtins): the module builds it freestanding,
// tests/switch/pad_hook_test.cpp on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <atomic>  // (MSVC, host tests only: it has no __atomic builtins)
#endif

#include "coop/ui/events.h"
#include "coop/ui/pad.hpp"

namespace pad_hook {

// Acquire/release atomics on plain words: GCC/Clang builtins (the module is freestanding, no <atomic>), and
// std::atomic_ref for MSVC's host build. The order is a template argument: the builtins need a constant one.
namespace atom {
enum Order { kRelaxed, kAcquire, kRelease, kAcqRel };
#if defined(_MSC_VER) && !defined(__clang__)
constexpr std::memory_order Std(Order o) {
    return o == kAcquire ? std::memory_order_acquire
           : o == kRelease ? std::memory_order_release
           : o == kAcqRel  ? std::memory_order_acq_rel
                           : std::memory_order_relaxed;
}
template <Order O, typename T>
T Load(const T& v) { return std::atomic_ref<T>(const_cast<T&>(v)).load(Std(O)); }
template <Order O, typename T, typename U>
void Store(T& v, U x) { std::atomic_ref<T>(v).store(static_cast<T>(x), Std(O)); }
template <Order O, typename T, typename U>
T Exchange(T& v, U x) { return std::atomic_ref<T>(v).exchange(static_cast<T>(x), Std(O)); }
#else
constexpr int Gnu(Order o) {
    return o == kAcquire ? __ATOMIC_ACQUIRE : o == kRelease ? __ATOMIC_RELEASE : o == kAcqRel ? __ATOMIC_ACQ_REL : __ATOMIC_RELAXED;
}
template <Order O, typename T>
T Load(const T& v) { return __atomic_load_n(&v, Gnu(O)); }
template <Order O, typename T, typename U>
void Store(T& v, U x) { __atomic_store_n(&v, static_cast<T>(x), Gnu(O)); }
template <Order O, typename T, typename U>
T Exchange(T& v, U x) { return __atomic_exchange_n(&v, static_cast<T>(x), Gnu(O)); }
#endif
}  // namespace atom

// The layout every GetNpadStates overload fills (FullKey, Handheld, JoyDual, JoyLeft, JoyRight: NpadCommonState).
struct NpadState {
    int64_t sampling;
    uint64_t buttons;  // nn::hid::NpadButton bits (coop::ui::npad)
    int32_t stick[4];  // left x, y, right x, y (+-32767, y up)
    uint32_t attributes;
    uint32_t reserved;
};
static_assert(sizeof(NpadState) == 0x28, "NpadCommonState is 0x28 bytes");

constexpr uint32_t kAttrConnected = 1;
constexpr int kPlayers = 4;
constexpr uint32_t kNpadNo1 = 0;
constexpr uint32_t kNpadHandheld = 0x20;
constexpr int32_t kActiveStick = 8000;  // a stick past this counts as input when choosing player 1's source

// Npad id -> player 0..3 (No.1-No.4; Handheld is player 1), or -1 for one co-op does not read.
constexpr int PlayerOf(uint32_t npad_id) {
    if (npad_id < static_cast<uint32_t>(kPlayers)) return static_cast<int>(npad_id);
    return npad_id == kNpadHandheld ? 0 : -1;
}

struct Pad {
    uint64_t buttons = 0;
    int32_t stick[4] = {0, 0, 0, 0};
};

// One button of one player went down or up (before the menu's gate: what the player pressed), with the left stick
// at that read.
struct Edge {
    uint8_t player;  // 0..3
    uint8_t bit;     // NpadButton bit
    uint8_t down;    // 1 down, 0 up
    uint8_t reserved;
    int32_t stick_x;
    int32_t stick_y;
};

// Single producer, single consumer ring: Push on one thread, Take on one other. It keeps the oldest items and
// counts the ones it had no room for. tail_ is written only by the producer, head_ only by the consumer; the item
// writes are published by the release store of tail_, and the slots handed back by the release store of head_.
template <typename T, size_t N>
class Queue {
public:
    void Push(const T& v) {
        const uint64_t t = atom::Load<atom::kRelaxed>(tail_);
        const uint64_t h = atom::Load<atom::kAcquire>(head_);
        if (t - h >= N) {
            atom::Store<atom::kRelaxed>(dropped_, atom::Load<atom::kRelaxed>(dropped_) + 1);
            return;
        }
        items_[t % N] = v;
        atom::Store<atom::kRelease>(tail_, t + 1);
    }
    size_t Take(T* out, size_t max) {
        uint64_t h = atom::Load<atom::kRelaxed>(head_);
        const uint64_t t = atom::Load<atom::kAcquire>(tail_);
        size_t k = 0;
        for (; k < max && h != t; ++k, ++h) out[k] = items_[h % N];
        atom::Store<atom::kRelease>(head_, h);
        return k;
    }
    uint64_t dropped() const { return atom::Load<atom::kRelaxed>(dropped_); }

private:
    T items_[N] = {};  // (zeros: the module's Hook is constinit, so this lands in .bss with no code)
    uint64_t head_ = 0;
    uint64_t tail_ = 0;
    uint64_t dropped_ = 0;
};

class Hook {
public:
    // auto_close_ms: the menu closes by itself after this long (0: never). The prototype has no menu picture yet, so
    // it uses 5 s: whoever opens it by accident gets the pad back on their own.
    constexpr explicit Hook(uint64_t auto_close_ms = 0) : auto_close_ms_(auto_close_ms) {}

    // PRODUCER. One GetNpadStates result: `count` states of npad `npad_id`, the latest first, as the game will get them.
    void OnStates(NpadState* states, int count, uint32_t npad_id, uint64_t now_ms) {
        const int p = PlayerOf(npad_id);
        if (p < 0 || count <= 0 || states == nullptr) return;
        const bool connected = (states[0].attributes & kAttrConnected) != 0;
        if (p == 0) {
            NextFrame();
            const int slot = npad_id == kNpadHandheld ? 1 : 0;
            if (connected) {
                seen_[slot] = true;
                active_[slot] = active_[slot] || Active(states[0]);
            }
            if (npad_id != source_) {  // not player 1's source this frame: the game gets it masked like the source
                MaskOther(states, count);
                return;
            }
        }
        if (!connected) return;  // not this style: its zeros are not the pad
        Record(p, states[0]);
        if (p != 0) return;
        TakeRequest(now_ms);
        const bool had_pad = Load(open_);
        if (had_pad && auto_close_ms_ != 0 && now_ms - opened_ms_ >= auto_close_ms_) {
            Close();
            Store(auto_closes_, Load(auto_closes_) + 1);
        }
        const bool was_open = Load(open_);
        const coop::ui::PadEvents ev =
            reader_.Feed(states[0].buttons, states[0].stick[0], states[0].stick[1], now_ms, was_open);
        for (size_t i = 0; i < ev.n; ++i) {
            if (ev.e[i].kind == COOP_UI_OPEN) {
                SetOpen(true, now_ms);
            } else if (ev.e[i].kind == COOP_UI_CLOSE) {
                SetOpen(false, now_ms);
            }
            events_.Push(ev.e[i]);
        }
        // The menu has the pad while open, and on the read it opened or closed on (so neither Minus reaches the game).
        const bool menu_has_pad = had_pad || Load(open_);
        states[0].buttons = gate_.Pass(states[0].buttons, menu_has_pad);
        for (int i = 0; i < count; ++i) {
            if (i > 0) {  // older states: the same mask, without moving the gate
                states[i].buttons = menu_has_pad ? 0 : states[i].buttons & ~gate_.latched();
            }
            if (menu_has_pad) {
                for (int32_t& s : states[i].stick) s = 0;
            }
        }
    }

    // CONSUMER: a new frame starts (the frame tick, with its frame number). Player 1's source may change from here.
    void Frame(uint64_t frame) { atom::Store<atom::kRelease>(frame_, frame); }

    // CONSUMER: co-op's menu is open (the game gets none of player 1's pad), or closed.
    bool open() const { return atom::Load<atom::kAcquire>(open_) != 0; }
    // CONSUMER: open or close the menu (the core, after f1); done at player 1's next read, with no UI event.
    void RequestOpen(bool open) { atom::Store<atom::kRelease>(request_, open ? kWantOpen : kWantClose); }

    size_t TakeEdges(Edge* out, size_t max) { return edges_.Take(out, max); }
    size_t TakeEvents(CoopUiEvent* out, size_t max) { return events_.Take(out, max); }
    uint64_t dropped_edges() const { return edges_.dropped(); }
    uint64_t dropped_events() const { return events_.dropped(); }
    // Connected reads recorded for a player (0: never seen).
    uint64_t reads(int player) const { return atom::Load<atom::kRelaxed>(reads_[player]); }
    // How many times the menu closed by itself (auto_close_ms).
    uint64_t auto_closes() const { return atom::Load<atom::kRelaxed>(auto_closes_); }
    // Player 1's source npad id (No.1 or Handheld).
    uint32_t source() const { return atom::Load<atom::kRelaxed>(source_pub_); }

    // PRODUCER's thread only (and tests): the latest pad of a player.
    const Pad& pad(int player) const { return pads_[player]; }

private:
    static constexpr uint32_t kWantOpen = 1;
    static constexpr uint32_t kWantClose = 2;

    // Producer-side atomics: written here, read by the consumer.
    static uint64_t Load(const uint64_t& v) { return atom::Load<atom::kRelaxed>(v); }
    static void Store(uint64_t& v, uint64_t x) { atom::Store<atom::kRelaxed>(v, x); }
    static bool Load(const uint32_t& v) { return atom::Load<atom::kRelaxed>(v) != 0; }

    static bool Active(const NpadState& s) {
        if (s.buttons != 0) return true;
        for (int32_t v : s.stick) {
            if (v > kActiveStick || v < -kActiveStick) return true;
        }
        return false;
    }

    // At the first read of player 1 in a new frame: maybe switch the source, then start counting again.
    void NextFrame() {
        const uint64_t f = atom::Load<atom::kAcquire>(frame_);
        if (f == frame_seen_) return;
        frame_seen_ = f;
        const int s = source_ == kNpadHandheld ? 1 : 0;
        const int o = 1 - s;
        if (seen_[o] && (!seen_[s] || (active_[o] && !active_[s]))) {
            source_ = o == 1 ? kNpadHandheld : kNpadNo1;
            atom::Store<atom::kRelaxed>(source_pub_, source_);
        }
        seen_[0] = seen_[1] = false;
        active_[0] = active_[1] = false;
    }

    void MaskOther(NpadState* states, int count) const {
        const bool menu_has_pad = Load(open_);
        for (int i = 0; i < count; ++i) {
            states[i].buttons = menu_has_pad ? 0 : states[i].buttons & ~gate_.latched();
            if (menu_has_pad) {
                for (int32_t& s : states[i].stick) s = 0;
            }
        }
    }

    void Record(int p, const NpadState& s) {
        Pad& pad = pads_[p];
        const uint64_t changed = pad.buttons ^ s.buttons;
        for (uint8_t bit = 0; bit < 64; ++bit) {
            if ((changed >> bit) & 1) {
                Edge e{};
                e.player = static_cast<uint8_t>(p);
                e.bit = bit;
                e.down = static_cast<uint8_t>((s.buttons >> bit) & 1);
                e.stick_x = s.stick[0];
                e.stick_y = s.stick[1];
                edges_.Push(e);
            }
        }
        pad.buttons = s.buttons;
        for (int i = 0; i < 4; ++i) pad.stick[i] = s.stick[i];
        Store(reads_[p], Load(reads_[p]) + 1);
    }

    void TakeRequest(uint64_t now_ms) {
        const uint32_t r = atom::Exchange<atom::kAcquire>(request_, 0u);
        if (r == kWantOpen) SetOpen(true, now_ms);
        if (r == kWantClose) SetOpen(false, now_ms);
    }

    void SetOpen(bool open, uint64_t now_ms) {
        if (open && !Load(open_)) opened_ms_ = now_ms;
        atom::Store<atom::kRelease>(open_, open ? 1u : 0u);
    }

    void Close() {
        SetOpen(false, 0);
        CoopUiEvent e{};
        e.kind = COOP_UI_CLOSE;
        events_.Push(e);
    }

    // Producer only.
    uint64_t auto_close_ms_;
    uint64_t opened_ms_ = 0;
    uint64_t frame_seen_ = 0;
    uint32_t source_ = kNpadNo1;
    bool seen_[2] = {false, false};
    bool active_[2] = {false, false};
    Pad pads_[kPlayers];
    coop::ui::PadReader reader_;
    coop::ui::PadGate gate_;
    // Shared (atomics only).
    uint32_t open_ = 0;          // producer writes (release), consumer reads (acquire)
    uint32_t request_ = 0;       // consumer writes (release), producer takes (exchange, acquire)
    uint32_t source_pub_ = kNpadNo1;
    uint64_t frame_ = 0;         // consumer writes (release), producer reads (acquire)
    uint64_t reads_[kPlayers] = {0, 0, 0, 0};
    uint64_t auto_closes_ = 0;
    Queue<Edge, 64> edges_;
    Queue<CoopUiEvent, 32> events_;
};

}  // namespace pad_hook
