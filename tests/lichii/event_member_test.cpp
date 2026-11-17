// Tests for which actors our event is with (lichii/hook/coop_event_member.h, row 97).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_event_member.h"

namespace {

// Guest memory as 8-byte words; anything not put cannot be read.
struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    void Put(std::uint64_t at, std::uint64_t v) { words[at] = v; }
    void PutText(std::uint64_t at, const std::string& text) {
        std::string padded = text;
        padded.resize((text.size() / 8 + 1) * 8, '\0');
        for (std::size_t i = 0; i < padded.size(); i += 8) {
            std::uint64_t w = 0;
            std::memcpy(&w, padded.data() + i, 8);
            Put(at + i, w);
        }
    }
    CoopEventMember::ReadWord Reader() const {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            const auto it = words.find(at);
            if (it == words.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

constexpr std::uint64_t kActor = 0x10'0000;
constexpr std::uint64_t kParts = 0x20'0000;
constexpr std::uint64_t kPerformer = 0x30'0000;

} // namespace

TEST_CASE("event member: in our Link's event only when its fixed-slot performer holds our Link's link (row 97)") {
    using namespace CoopEventMember;
    constexpr std::uint64_t kLink = 0x50'0000, kLinkParts = 0x51'0000, kLinkPerformer = 0x52'0000;
    constexpr std::uint64_t kOurEvent = 0x106d7f9dd0;  // a link as the probe read it
    Memory m;
    // Our Link: its performer at the player's slot 0x90.
    m.Put(kLink + kActorParts, kLinkParts);
    m.Put(kLinkParts + kPerformerSlotAlt, kLinkPerformer);
    m.Put(kLinkPerformer + kPerformerActor, kLink);
    m.Put(kLinkPerformer + kPerformerLink, 0);
    CHECK(OurLinkOf(m.Reader(), kLink) == 0);  // in no event
    m.Put(kLinkPerformer + kPerformerLink, kOurEvent);
    const std::uint64_t ours = OurLinkOf(m.Reader(), kLink);
    CHECK(ours == kOurEvent);
    // The actor (a scrub, say), its performer at 0x88. Nothing here reads the event manager's
    // word: a Deku Scrub's talk, which leaves it 0, is seen all the same.
    m.Put(kActor + kActorParts, kParts);
    m.Put(kParts + kPerformerSlot, kPerformer);
    m.Put(kPerformer + kPerformerActor, kActor);
    m.Put(kPerformer + kPerformerLink, 0);
    REQUIRE(InOurEvent(m.Reader(), kActor, ours));
    CHECK_FALSE(*InOurEvent(m.Reader(), kActor, ours));  // not bound
    m.Put(kPerformer + kPerformerLink, kOurEvent);
    CHECK(*InOurEvent(m.Reader(), kActor, ours));        // bound into our Link's event
    CHECK_FALSE(*InOurEvent(m.Reader(), kActor, 0));     // our Link in no event: nothing is ours
    // Another event's link (a butterfly's own) and heap neighbours of ours never count.
    for (const std::uint64_t other : {std::uint64_t{0x106d900000}, kOurEvent + 8, kOurEvent - 8, kOurEvent + 0x880}) {
        CAPTURE(other);
        m.Put(kPerformer + kPerformerLink, other);
        CHECK_FALSE(*InOurEvent(m.Reader(), kActor, ours));
    }
    // A stray component of its own (owner back-pointer at +0x10, our link at +0x18) in another
    // slot is never taken as the performer: no performer at 0x88/0x90, not known.
    m.Put(kParts + kPerformerSlot, 0);
    m.Put(kParts + 0xa8, 0x53'0000);
    m.Put(0x53'0000 + kPerformerActor, kActor);
    m.Put(0x53'0000 + kPerformerLink, kOurEvent);
    CHECK_FALSE(InOurEvent(m.Reader(), kActor, ours));
    // ... only the diagnostic scan names that slot.
    const auto slots = ScanLinked(m.Reader(), kActor, ours);
    REQUIRE(slots.size() == 1);
    CHECK(slots[0] == 0xa8);
    CHECK(ScanLinked(m.Reader(), kActor, 0).empty());
    // The fixed slot holding another actor's performer: not ours either.
    m.Put(kParts + kPerformerSlot, kPerformer);
    m.Put(kPerformer + kPerformerActor, 0x99);
    CHECK_FALSE(InOurEvent(m.Reader(), kActor, ours));
    CHECK_FALSE(InOurEvent(m.Reader(), 0, ours));
}

TEST_CASE("event member: our running event's kind and the names it points at, for the log") {
    using namespace CoopEventMember;
    constexpr std::uint64_t kMain = 0x7100000000;
    constexpr std::uint64_t kManager = 0x50'0000, kSession = 0x60'0000, kEvents = 0x70'0000;
    constexpr std::uint64_t kEvent = 0x80'0000, kName = 0x90'0000, kJunk = 0xa0'0000;
    Memory m;
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);  // nothing readable
    m.Put(kMain + kEventManagerPtr, kManager);
    m.Put(kManager + kEventSession, 0);
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);  // no event runs
    m.Put(kManager + kEventSession, kSession);
    m.Put(kSession + kSessionEvents, kEvents);
    m.Put(kSession + kSessionCount, 2);
    m.Put(kSession + kSessionCurrent, 1);
    m.Put(kEvents + 8, kEvent);
    m.Put(kEvent + kEventKind, 0);
    m.Put(kEvent + 0x10, kName);
    m.PutText(kName, "InsideTheDekuTree_Scrub_Talk");
    m.Put(kEvent + 0x18, kJunk);
    m.PutText(kJunk, "\x01\x02zz");          // not a name
    m.Put(kEvent + 0x20, kName);             // the same name again: once
    const Running r = ReadRunning(m.Reader(), kMain);
    CHECK(r.event == kEvent);
    CHECK(r.count == 2);
    REQUIRE(r.kind);
    CHECK(*r.kind == 0);
    REQUIRE(r.names.size() == 1);
    CHECK(r.names[0] == "InsideTheDekuTree_Scrub_Talk");
    m.Put(kSession + kSessionCurrent, 2);  // past the count: none
    CHECK(ReadRunning(m.Reader(), kMain).event == 0);
}
