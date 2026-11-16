// SPDX-License-Identifier: GPL-3.0-or-later
// Whether the local Link holds his sword and shield in hand, as the game's own
// query says (sword-3).
//
// PlayerReader::read_drawn (coop/host) tells a drawn piece by the request
// byte at EquipmentUser + 0x13bc + slot. The game sets that byte and clears it
// once the change is done, often inside one game frame. The reader runs at
// present, on another thread, so it saw some draws and put-aways and missed
// others: "each sheathe or draw is a coin toss" (the owner, 0.11 kit). A missed
// draw left the other screen's puppet with the sword on its back; a missed
// put-away left it holding the sword.
//
// The game's own read of a slot, 0xce7ab0(eu, slot) (CoopPuppet::addr::
// kEquipState: 1 in hand, 2 put away, 0 none; a request in flight first,
// else the state the piece itself keeps), does not miss a change. The puppet
// director (calc thread) asks it for the local Link every frame, in turn for
// these slots, and stores the answer here. The reader's drawn bits for those
// slots are then the game's answer while it is fresh.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <coop/os.hpp>
#include <cstdint>

namespace CoopPuppetOwn {

// The slots asked: the sword (Weapon) and the shield. The carried slots (held
// item, tools) are already told by their piece's own state (read_drawn).
inline constexpr std::array<std::uint32_t, 2> kSlots{0, 4};
// An answer older than this is not used (the director stopped: a load, or no
// local Link); the request bytes stand then, as before.
inline constexpr std::uint64_t kFreshMs = 500;

inline std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          coop::os::SteadyClock::now().time_since_epoch())
                                          .count());
}

class Drawn {
public:
    // The calc thread: the game's state of slot kSlots[index] (1 in hand,
    // 2 put away, anything else: no piece or not known) at at_ms.
    void Set(std::size_t index, std::uint32_t state, std::uint64_t at_ms) {
        if (index >= kSlots.size()) {
            return;
        }
        std::uint64_t word = 0;  // not known
        if (state == 1 || state == 2) {
            word = (at_ms << 2) | 2u | (state == 1 ? 1u : 0u);
        }
        words_[index].store(word, std::memory_order_relaxed);
    }
    // Any thread: `drawn` (bit n: slot n in hand) with every slot the game
    // answered for in the last kFreshMs taken from that answer.
    std::uint8_t Apply(std::uint8_t drawn, std::uint64_t now_ms) const {
        for (std::size_t i = 0; i < kSlots.size(); ++i) {
            const std::uint64_t word = words_[i].load(std::memory_order_relaxed);
            if ((word & 2u) == 0) {
                continue;
            }
            const std::uint64_t at = word >> 2;
            if (now_ms >= at + kFreshMs || at > now_ms + kFreshMs) {
                continue;
            }
            const auto bit = static_cast<std::uint8_t>(1u << kSlots[i]);
            drawn = static_cast<std::uint8_t>((word & 1u) != 0 ? (drawn | bit) : (drawn & ~bit));
        }
        return drawn;
    }
    void Clear() {
        for (auto& w : words_) {
            w.store(0, std::memory_order_relaxed);
        }
    }

private:
    std::array<std::atomic<std::uint64_t>, kSlots.size()> words_{};
};

// The one store: the director writes it, the hook's reader applies it.
inline Drawn& TheDrawn() {
    static Drawn drawn;
    return drawn;
}

}  // namespace CoopPuppetOwn
