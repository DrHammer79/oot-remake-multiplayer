// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h's logic: no game reads or calls, no log (the tests drive it).
#include "coop_pause_tab.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace CoopPauseTab {

namespace {

// A screen whose updates stopped this long ago is taken as gone (the game updates an open menu once a frame).
constexpr std::uint64_t kStaleMs = 250;
// A window over the page keeps it away this long after its last opening, open or closing update.
constexpr std::uint64_t kBlockMs = 250;

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

// What a screen shows, as one line (the part that is compared).
std::string Shows(const Facts& f) {
    char b[160];
    if (f.kind == Kind::kStrip) {
        std::snprintf(b, sizeof b, "%d tabs, %s menu, lights category %d, state %d", f.tabs,
                      f.mode == kPlusMode ? "the Plus" : "the Minus", f.lit, f.state);
    } else {
        std::snprintf(b, sizeof b, "page %d, asked %d, state %d", f.page, f.asked, f.state);
    }
    return b;
}

}  // namespace

Kind KindOf(std::uint64_t vtable_offset) {
    if (vtable_offset == kPagesVtable) return Kind::kPages;
    if (vtable_offset == kStripVtable) return Kind::kStrip;
    for (const std::uint64_t v : kBlockerVtables) {
        if (vtable_offset == v) return Kind::kBlocker;
    }
    return Kind::kOther;
}

// --- TextField ---

const std::string& TextField::Letters() const {
    static const std::string room = "abcdefghijklmnopqrstuvwxyz0123456789-_";
    // (a host's own password is 20 of the first 32, the letters of its codes, coop::server::nat::kLetters: those come
    // first, so the pad's up and down reach them soonest)
    static const std::string password =
        "ABCDEFGHJKLMNPQRSTUVWXYZ23456789IO01abcdefghijklmnopqrstuvwxyz!#$%&*+-.=?@_";
    return kind_ == Kind::kRoom ? room : password;
}

bool TextField::Allowed(char c) const {
    if (kind_ == Kind::kRoom) return Letters().find(c) != std::string::npos;
    return c > 0x20 && c < 0x7f;
}

void TextField::Cycle(int dir) {
    const std::string& letters = Letters();
    if (cursor_ >= text_.size()) {
        if (text_.size() >= Max()) return;
        text_.push_back(dir >= 0 ? letters.front() : letters.back());
        return;
    }
    const auto at = letters.find(text_[cursor_]);
    const std::size_t n = letters.size();
    const std::size_t i = at == std::string::npos ? 0 : (at + n + (dir >= 0 ? 1 : n - 1)) % n;
    text_[cursor_] = letters[i];
}

void TextField::Move(int dir) {
    if (dir < 0) {
        if (cursor_ > 0) --cursor_;
        return;
    }
    const std::size_t last = text_.size() < Max() ? text_.size() : text_.size() - 1;
    if (cursor_ < last) {
        ++cursor_;
    } else {
        cursor_ = last;   // (a full text: the cursor on its last letter, not past it)
    }
}

void TextField::Delete() {
    if (text_.empty()) return;
    if (cursor_ >= text_.size()) cursor_ = text_.size() - 1;
    text_.erase(cursor_, 1);
}

void TextField::Backspace() {
    if (cursor_ == 0 || text_.empty()) return;
    --cursor_;
    text_.erase(cursor_, 1);
}

bool TextField::Type(std::uint32_t cp) {
    if (cp >= 0x80) return false;
    char c = static_cast<char>(cp);
    if (kind_ == Kind::kRoom && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');   // (any case: the relay's)
    if (!Allowed(c) || text_.size() >= Max()) return false;
    if (cursor_ > text_.size()) cursor_ = text_.size();
    text_.insert(text_.begin() + static_cast<std::ptrdiff_t>(cursor_), c);
    ++cursor_;
    return true;
}

std::string TextField::Shown(bool typing, std::size_t room) const {
    std::string shown;
    std::size_t mark = std::string::npos;   // where the cursor's bracket went (a password may hold '[' itself)
    for (std::size_t i = 0; i <= text_.size(); ++i) {
        const bool here = typing && i == cursor_;
        if (here) mark = shown.size();
        if (i == text_.size()) {
            if (here) shown += "[ ]";
            break;
        }
        const char c = kind_ == Kind::kPassword && !here ? '*' : text_[i];
        shown += here ? std::string("[") + c + "]" : std::string(1, c);
    }
    if (shown.size() <= room) return shown;
    // (too long: the part around the cursor, so its letter stays in view)
    if (mark == std::string::npos) mark = shown.size();
    std::size_t from = mark > room / 2 ? mark - room / 2 : 0;
    if (from + room > shown.size()) from = shown.size() - room;
    return shown.substr(from, room);
}

// --- Page ---

// The menu open on System, the strip with its five tabs: R shows our page.
bool Page::CanShow(std::uint64_t now_ms) const {
    return CanStay(now_ms) && pages_.state == 2 && strip_.state == 2;
}

// The menu open, or opening, on System: our page stays, or comes back as the menu opens again. (Opening too: the menu
// opening again shows our page from its first update, and a run showed the pages open, state 2, while the strip still
// opens, state 1.)
bool Page::CanStay(std::uint64_t now_ms) const {
    // The menu's first frames (a run, 2026-11-19): the strip's first update of the opening comes a frame before the
    // pages', whose last update is still the closed one (state 0, old); then the pages are at 0 while the strip
    // opens, System's page already drawn. Both are the menu opening, on the page the pages last had.
    const bool pages_fresh = now_ms - pages_ms_ < kStaleMs;
    const bool pages_open = pages_fresh ? pages_.state == 1 || pages_.state == 2 || (pages_.state == 0 && strip_.state == 1)
                                        : strip_.state == 1;
    return usable_ && pages_.screen != 0 && strip_.screen != 0 && now_ms - strip_ms_ < kStaleMs && pages_open &&
           pages_.page == kSystemPage && pages_.asked == kSystemPage && (strip_.state == 1 || strip_.state == 2) &&
           strip_.mode == kPlusMode && strip_.tabs == 5 && now_ms >= blocked_until_ms_;
}

void Page::Hide(const char* why, bool keep) {
    if (!shown_) {
        return;
    }
    shown_ = false;
    sub_ = false, back_ = false;
    typing_ = -1;
    again_ = keep;
    why_ = why;
    // Every button held now stays the module's until let go, so none reaches System as a fresh press (an A held
    // as the page goes would otherwise be the System page's A).
    swallow_ |= pad_last_ & ~kPadPassed;
}

std::uint64_t Page::Track(const Facts& f) {
    if (f.kind != Kind::kPages && f.kind != Kind::kStrip) return 0;
    Seen& s = f.kind == Kind::kPages ? pages_seen_ : strip_seen_;
    const bool reopened = f.state == 1 && s.state != 1;
    if (s.gen == 0 || f.screen != s.screen || f.layout != s.layout || reopened) {
        s.screen = f.screen, s.layout = f.layout, s.gen = ++next_gen_;
        if (f.kind == Kind::kPages) {
            page_set_ = -1, texts_set_ = false;
        } else {
            tab_key_ = -1, tab_filled_ = false;
        }
    }
    s.state = f.state;
    return s.gen;
}

void Page::SetUsable(bool usable) {
    usable_ = usable;
    if (!usable_) Hide("its panes or the game's calls are not there");
}

Page::Do Page::OnScreen(const Facts& f, std::uint64_t now_ms) {
    Do d;
    Track(f);
    switch (f.kind) {
    case Kind::kBlocker:
        if (f.state >= 1 && f.state <= 3) {
            blocked_until_ms_ = now_ms + kBlockMs;
            Hide("a window opened over it");
        }
        return d;
    case Kind::kPages: {
        pages_ = f, pages_ms_ = now_ms;
        if (shown_ && !CanStay(now_ms)) {
            const bool closing = f.state == 3 || f.state == 0;
            Hide(closing ? "the menu is closing" : "the menu left the System page", closing);
        }
        ShowAgain(now_ms);
        const int want = shown_ ? 1 : 0;
        if (page_set_ != want) {
            d.set_page = true, d.page_shown = shown_, page_set_ = want;
        }
        if (!texts_set_ && f.state == 2) {
            d.set_texts = true, texts_set_ = true;
        }
        return d;
    }
    case Kind::kStrip: {
        strip_ = f, strip_ms_ = now_ms;
        if (shown_ && !CanStay(now_ms)) {
            Hide(f.mode != kPlusMode ? "the Minus menu" : "the tab strip is closing", f.mode == kPlusMode);
        }
        ShowAgain(now_ms);
        // (back at the strip's first update of the opening: the pages' own comes a frame later, so the module shows
        // our page through the pages screen it knows, in this update)
        if (shown_ && page_set_ != 1 && pages_.screen != 0) {
            d.set_page = true, d.page_shown = true, page_set_ = 1;
        }
        // (shown from the strip's opening to the end of its closing, as the game's four tabs are: shown only once
        // it was open, the fifth came in a moment after them; the module moves and fades it with the fourth. Filled
        // once per opening: a fill again restarts the tab's own opening.)
        const bool visible = f.tabs == 5 && f.mode == kPlusMode && f.state >= 1 && f.state <= 3;
        const int key = (visible ? 1 : 0) | (shown_ ? 2 : 0) | ((f.lit & 0xff) << 2) | ((f.state & 0xf) << 10) |
                        ((f.mode & 0xf) << 14);
        if (f.tabs == 5 && key != tab_key_) {
            tab_key_ = key;
            d.set_tabs = true, d.tab_shown = visible, d.tab_lit = visible && shown_;
            d.fill_tab = visible && !tab_filled_;
            tab_filled_ = visible;
        }
        return d;
    }
    case Kind::kOther:
        break;
    }
    return d;
}

std::uint64_t Page::OnPad(std::uint64_t buttons, std::uint64_t now_ms) {
    const std::uint64_t down = buttons & ~pad_last_;
    pad_last_ = buttons;
    swallow_ &= buttons;   // (let go: the game gets it again)
    if (shown_ && !CanStay(now_ms)) {
        Hide("the menu's state changed", pages_.state == 3 || pages_.state == 0 || strip_.state == 3 || strip_.state == 0);
    }
    if (!shown_) {
        if ((down & (kPadR | kPadZR)) != 0 && CanShow(now_ms)) {
            shown_ = true, decide_ = false, why_.clear();
            strip_sound_ = "mc_TabToRight";
            swallow_ |= buttons & ~kPadPassed;   // (R and anything held with it: none of it reaches System)
            return buttons & kPadPassed & ~swallow_;
        }
        return buttons & ~swallow_;
    }
    if (typing_ >= 0) {   // (a field typed into: every button is its, none reaches System)
        TypingPad(down, buttons, now_ms);
        swallow_ |= buttons;
        return 0;
    }
    if ((down & (kPadL | kPadZL)) != 0) {
        Hide("L");
        strip_sound_ = "mc_TabToLeft";
        return buttons & ~swallow_;
    }
    if ((down & kPadA) != 0) {
        decide_ = true;
        pages_sound_ = "button_decide";
    }
    if (sub_ && (down & kPadB) != 0) {   // (a view: B goes back to the five rows, kept until let go)
        back_ = true;
        swallow_ |= kPadB;
    }
    return buttons & kPadPassed & ~swallow_;
}

bool Page::TakeBack() {
    const bool b = back_;
    back_ = false;
    return b;
}

void Page::StartTyping(int which) {
    if (!shown_) return;
    typing_ = which == 1 ? 1 : 0;
    repeat_since_ = repeat_last_ = 0;
    swallow_ |= pad_last_;   // (the A that started it is not the field's first press)
}

void Page::TypingPad(std::uint64_t down, std::uint64_t held, std::uint64_t now_ms) {
    TextField& f = Field(typing_);
    if ((down & (kPadA | kPadB | kPadPlus)) != 0) {   // (done: the row shows what was typed)
        typing_ = -1;
        pages_sound_ = "button_decide";
        return;
    }
    if ((down & kPadY) != 0) f.Delete();
    if ((down & kPadX) != 0) f.Clear();
    const std::uint64_t up = kPadUp | kPadStickUp, dn = kPadDown | kPadStickDown;
    const std::uint64_t left = kPadLeft | kPadStickLeft, right = kPadRight | kPadStickRight;
    const std::uint64_t dirs = up | dn | left | right;
    const auto step = [&](std::uint64_t bits) {
        if ((bits & up) != 0) f.Cycle(+1);
        if ((bits & dn) != 0) f.Cycle(-1);
        if ((bits & left) != 0) f.Move(-1);
        if ((bits & right) != 0) f.Move(+1);
    };
    if ((down & dirs) != 0) {
        step(down & dirs);
        repeat_since_ = repeat_last_ = now_ms;
        pages_sound_ = "button_on";
    } else if ((held & dirs) != 0 && repeat_since_ != 0 && now_ms - repeat_since_ > 400 && now_ms - repeat_last_ > 90) {
        step(held & dirs);   // (held: again and again, as the game's own lists scroll)
        repeat_last_ = now_ms;
    }
    if ((held & dirs) == 0) repeat_since_ = 0;
}

bool Page::Key(std::uint32_t vk, bool repeat) {
    if (typing_ < 0) return false;
    TextField& f = Field(typing_);
    switch (vk) {
    case 0x25: f.Move(-1); return true;      // VK_LEFT
    case 0x27: f.Move(+1); return true;      // VK_RIGHT
    case 0x26: f.Cycle(+1); return true;     // VK_UP
    case 0x28: f.Cycle(-1); return true;     // VK_DOWN
    case 0x08: f.Backspace(); return true;   // VK_BACK
    case 0x2e: f.Delete(); return true;      // VK_DELETE
    case 0x0d:                               // VK_RETURN, VK_ESCAPE: done
    case 0x1b:
        if (!repeat) typing_ = -1;
        return true;
    default:
        return false;   // (a letter comes as its character, Char)
    }
}

bool Page::Char(std::uint32_t cp) {
    if (typing_ < 0) return false;
    Field(typing_).Type(cp);
    return true;   // (every character is the field's while it is typed into, taken or not)
}

void Page::ShowAgain(std::uint64_t now_ms) {
    if (!again_ || shown_) {
        return;
    }
    if (pages_.state == 2 && pages_.page != kSystemPage && pages_.page >= 0) {
        again_ = false;   // (the menu open on another page: not ours)
        return;
    }
    if (CanStay(now_ms)) {
        shown_ = true, again_ = false, decide_ = false, why_.clear();
    }
}

const char* Page::TakeStripSound() {
    return std::exchange(strip_sound_, nullptr);
}

const char* Page::TakePagesSound() {
    return std::exchange(pages_sound_, nullptr);
}

bool Page::TakeDecide() {
    const bool d = decide_;
    decide_ = false;
    return d;
}

std::string Page::TakeWhy() {
    std::string w;
    w.swap(why_);
    return w;
}

// --- the rows ---

int RowOfButton(const std::string& name) {
    if (name.size() != 12 || name.compare(0, 11, "Pa_Button_0") != 0) return -1;
    const int row = name[11] - '0';
    return row >= 0 && row < kRows ? row : -1;
}

float Ahead(float prev, float now, float limit) {
    const float step = now - prev, next = now + step;
    if ((step > 0 && now <= limit && next > limit) || (step < 0 && now >= limit && next < limit)) {
        return limit;
    }
    return next;
}

RowAction ActionFor(int row, const Session& s) {
    switch (row) {
    case kRowSession:
        return s.in_one || s.by_launcher || s.failed ? RowAction::kViewSession : RowAction::kHost;
    case kRowJoinCode:
        if (s.wait_host) return RowAction::kCopyAnswer;
        if (s.hosting) return s.code ? RowAction::kCopyCode : RowAction::kViewSession;   // (the code still coming)
        return s.in_one || s.by_launcher ? RowAction::kViewSession : RowAction::kViewJoin;
    case kRowPlayers:
        if (!s.in_one && !s.by_launcher) return RowAction::kViewLan;   // (not in a session: the games on this network)
        return RowAction::kViewPlayers;
    case kRowLetIn:
        if (!s.in_one && !s.by_launcher) return RowAction::kAutoRejoin;   // (not in a session: the switch)
        if (!s.hosting) return s.in_one && s.can_resync ? RowAction::kResync : RowAction::kNone;   // (only a host lets in)
        return s.invite && !s.asking ? RowAction::kPasteAnswer : RowAction::kViewLetIn;   // (a knock first)
    case kRowLeave:
        if (s.by_launcher) return RowAction::kOpenMain;   // (not stopped here: the main page says to start the game again)
        return s.in_one ? RowAction::kConfirmLeave : RowAction::kNone;   // (a second A leaves: never one stray press)
    default:
        return RowAction::kNone;
    }
}

View BackOf(View view) {
    switch (view) {
    case View::kPlayerHost: return View::kPlayer;
    case View::kPlayer: return View::kPlayers;
    case View::kLanGame: return View::kLan;
    default: return View::kMain;
    }
}

int PlayerPages(int listed) {
    return listed <= kRows - 1 ? 1 : (listed + kRows - 3) / (kRows - 2);
}

int PlayerIndex(int listed, int page, int row) {
    if (row < 0 || row >= kRows - 1 || listed <= 0) return -1;
    if (listed <= kRows - 1) return row < listed ? row : -1;
    if (row == kRows - 2) return -1;   // (the page's turn)
    const int pages = PlayerPages(listed);
    const int i = ((page % pages + pages) % pages) * (kRows - 2) + row;
    return i < listed ? i : -1;
}

RowAction ActionFor(View view, int row, const Session& s) {
    if (row < 0 || row >= kRows) return RowAction::kNone;
    if (view == View::kMain) return ActionFor(row, s);
    if (row == kRows - 1) return RowAction::kBack;
    switch (view) {
    case View::kMain: break;
    case View::kPlayers:
        if (s.listed > kRows - 1 && row == kRows - 2) return RowAction::kMorePlayers;
        return PlayerIndex(s.listed, s.page, row) >= 0 ? RowAction::kViewPlayer : RowAction::kNone;
    case View::kPlayer:
        if (s.player != 2) return RowAction::kNone;   // (us, or they left: nothing to do to them)
        if (row == 0) return RowAction::kTeleport;
        if (row == 1) return RowAction::kOwnVoice;
        if (row == 2) return RowAction::kOwnChat;
        return s.hosting ? RowAction::kHostControls : RowAction::kNone;
    case View::kPlayerHost:
        if (s.player != 2 || !s.hosting) return RowAction::kNone;
        if (row == 0) return RowAction::kHostVoice;
        if (row == 1) return RowAction::kHostChat;
        return row == 2 ? RowAction::kKick : RowAction::kBan;
    case View::kLan: {
        if (s.in_one || s.by_launcher) return RowAction::kNone;
        if (s.lan_listed > kRows - 1 && row == kRows - 2) return RowAction::kMoreLan;
        const int i = PlayerIndex(s.lan_listed, s.lan_page, row);
        return i >= 0 && i < 32 && (s.lan_joinable >> i & 1u) != 0 ? RowAction::kJoinLan : RowAction::kNone;
    }
    case View::kLanGame:
        // (0 the game, 1 its password typed on the row, 2 Join this game once one is typed; the last row is Back)
        if (s.in_one || s.by_launcher) return RowAction::kNone;
        if (row == 1) return RowAction::kTypePassword;
        if (row == 2) return s.lan_password ? RowAction::kJoinLanGame : RowAction::kTypePassword;
        return RowAction::kNone;
    case View::kLetIn:
        if (row == 0 && s.hosting && s.asking) return RowAction::kAllow;
        if (row == 1 && s.hosting && s.asking) return RowAction::kDeny;
        return RowAction::kNone;
    case View::kJoin: {
        const bool free = !s.in_one && !s.by_launcher;   // (as co-op's menu: nobody joins from inside a session)
        // (0 the clipboard's code, 1 and 2 the room name and its password typed on their rows, 3 that room once it
        // has a name, else the last game again; the last row is Back)
        if (!free) return RowAction::kNone;
        if (row == 0) return RowAction::kJoinPaste;
        if (row == 1) return RowAction::kTypeRoom;
        if (row == 2) return RowAction::kTypePassword;
        if (row == 3) return s.room_ready ? RowAction::kJoinRoom : s.rejoin ? RowAction::kRejoin : RowAction::kNone;
        return RowAction::kNone;
    }
    case View::kSession:
        if (s.hosting) {
            if (row == 0) return s.has_password ? RowAction::kShowPassword : RowAction::kNone;
            if (row == 1) return s.code ? RowAction::kCopyCode : RowAction::kNone;
            return row == 2 ? RowAction::kCopyLan : row == 3 ? RowAction::kCopyVpn : RowAction::kNone;
        }
        if (s.failed && !s.in_one) return row == 3 && !s.by_launcher ? RowAction::kHost : RowAction::kNone;
        if (row == 1 && s.wait_host) return RowAction::kCopyAnswer;
        if (row == 3 && s.in_one && s.can_resync) return RowAction::kResync;
        return RowAction::kNone;
    }
    return RowAction::kNone;
}

namespace {

// At most kRowChars bytes, cut before a UTF-8 sequence's start, with "..." when cut.
std::string Fit(std::string t) {
    if (t.size() <= kRowChars) return t;
    std::size_t n = kRowChars - 3;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xc0) == 0x80) --n;
    return t.substr(0, n) + "...";
}

// A line over up to `rows` rows, broken at spaces (a word longer than a row is cut); the last row ends in "..." when
// more is left.
std::vector<std::string> Wrap(const std::string& text, std::size_t rows) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (out.size() < rows) {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size()) break;
        if (out.size() + 1 == rows) {   // (the last row: the rest, Fit cuts it)
            out.push_back(Fit(text.substr(at)));
            break;
        }
        std::size_t end = std::min(text.size(), at + kRowChars);
        if (end < text.size() && text[end] != ' ') {
            const std::size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > at) end = space;
        }
        out.push_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}

std::string Players(int n) {
    return n == 1 ? "just you" : std::to_string(n) + " players";
}

}  // namespace

std::vector<std::string> RowTexts(const SessionView& v) {
    std::vector<std::string> rows(kRows);
    if (v.s.failed && !v.s.in_one) {
        rows[kRowSession] = "Co-op failed: A says why";
    } else if (v.connecting) {
        rows[kRowSession] = "Connecting...";
    } else if (v.s.hosting) {
        rows[kRowSession] = "Hosting";
    } else if (v.s.in_one) {
        rows[kRowSession] = "In a session";
    } else {
        rows[kRowSession] = "Host a game";
    }
    if (!v.note.empty() && (v.s.wait_host || v.s.hosting)) {
        rows[kRowJoinCode] = v.note;   // (what the copy on this row did)
    } else if (v.s.wait_host) {
        rows[kRowJoinCode] = "Copy answer code";
    } else if (v.s.hosting) {
        rows[kRowJoinCode] = !v.room.empty() ? "Room: " + v.room : v.s.code ? "Copy invite code" : "Room: coming...";
    } else {
        rows[kRowJoinCode] = v.s.in_one || v.s.by_launcher ? "Session details" : "Join a game";
    }
    rows[kRowPlayers] = !v.s.in_one && !v.s.by_launcher ? "Games on this network"
                        : v.players <= 0   ? "Players"
                        : v.players == 1 ? "Players: just you"
                                         : "Players: " + std::to_string(v.players);
    if (v.s.hosting) {
        if (v.asking.empty()) {
            rows[kRowLetIn] = v.s.invite ? "Paste answer code" : "Let in: nobody asks";
        } else if (v.asking.size() == 1) {
            rows[kRowLetIn] = "Let in " + v.asking.front();
        } else {
            rows[kRowLetIn] = "Let in " + v.asking.front() + " (+" + std::to_string(v.asking.size() - 1) + ")";
        }
    } else if (v.s.in_one) {
        rows[kRowLetIn] = v.s.can_resync ? "Resync from host" : "Let in: the host does";
    } else if (!v.s.by_launcher) {
        rows[kRowLetIn] = !v.s.auto_rejoin ? "Auto-rejoin: Off" : v.s.rejoining ? "Auto-rejoin: On (trying...)" : "Auto-rejoin: On";
    } else {
        rows[kRowLetIn] = "Let in: when you host";
    }
    if (v.s.by_launcher) {
        rows[kRowLeave] = "Leave: start the game again";
    } else if (v.confirm_leave) {
        rows[kRowLeave] = v.s.hosting ? "A again: stop hosting" : "A again: leave";
    } else {
        rows[kRowLeave] = !v.s.in_one ? "Leave (not in a session)" : v.s.hosting ? "Stop hosting" : "Leave";
    }
    for (std::string& r : rows) r = Fit(std::move(r));
    return rows;
}

std::vector<std::string> RowTexts(View view, const SessionView& v) {
    if (view == View::kMain) return RowTexts(v);
    std::vector<std::string> rows(kRows);
    rows[kRows - 1] = "Back";
    switch (view) {
    case View::kMain: break;
    case View::kPlayers: {
        const int listed = static_cast<int>(v.names.size());
        if (listed == 0) rows[0] = v.s.in_one ? "Players: coming..." : "Not in a session";
        for (int r = 0; r < kRows - 1; ++r) {
            const int i = PlayerIndex(listed, v.s.page, r);
            if (i >= 0) rows[static_cast<std::size_t>(r)] = v.names[static_cast<std::size_t>(i)];
        }
        if (listed > kRows - 1) {
            const int pages = PlayerPages(listed);
            rows[kRows - 2] = "More players (" + std::to_string((v.s.page % pages + pages) % pages + 1) + "/" +
                              std::to_string(pages) + ")";
        }
        break;
    }
    case View::kLan: {
        const int listed = static_cast<int>(v.lan_rows.size());
        if (listed == 0) rows[0] = "Looking for games...";
        for (int r = 0; r < kRows - 1; ++r) {
            const int i = PlayerIndex(listed, v.s.lan_page, r);
            if (i >= 0) rows[static_cast<std::size_t>(r)] = v.lan_rows[static_cast<std::size_t>(i)];
        }
        if (listed > kRows - 1) {
            const int pages = PlayerPages(listed);
            rows[kRows - 2] = "More games (" + std::to_string((v.s.lan_page % pages + pages) % pages + 1) + "/" +
                              std::to_string(pages) + ")";
        }
        break;
    }
    case View::kLanGame:
        rows[0] = v.lan_game;
        if (v.s.in_one || v.s.by_launcher) {
            rows[1] = v.s.by_launcher ? "Co-op came with the game" : "In a session already";
            break;
        }
        rows[1] = "Password: " + (v.password_field.empty() && v.typing != 1 ? std::string("A to type it") : v.password_field);
        rows[2] = v.s.lan_password ? "Join this game" : "Join: type its password";
        break;
    case View::kPlayer:
        if (v.s.player == 0) {
            rows[0] = "They left the session";
        } else if (v.s.player == 1) {
            rows[0] = v.player_name + " (you)";
            rows[1] = "This is you";
        } else {
            rows[0] = "Teleport to " + v.player_name;
            rows[1] = (v.own_mutes & kMuteVoice) != 0 ? "Hear their voice again" : "Mute their voice (you)";
            rows[2] = (v.own_mutes & kMuteChat) != 0 ? "Show their chat again" : "Mute their chat (you)";
            if (v.s.hosting) rows[3] = "Host controls";
        }
        break;
    case View::kPlayerHost:
        if (v.s.player != 2 || !v.s.hosting) {
            rows[0] = "They left the session";
        } else {
            rows[0] = (v.host_mutes & kMuteVoice) != 0 ? "Unmute voice for everyone" : "Mute voice for everyone";
            rows[1] = (v.host_mutes & kMuteChat) != 0 ? "Unmute chat for everyone" : "Mute chat for everyone";
            rows[2] = v.confirm_kick ? "A again: kick " + v.player_name : "Kick " + v.player_name;
            rows[3] = v.confirm_ban ? "A again: ban " + v.player_name : "Ban for this session";
        }
        break;
    case View::kLetIn:
        if (v.asking.empty()) {
            rows[0] = "Nobody asks to join";
        } else {
            rows[0] = "Let in " + v.asking.front();
            rows[1] = "Refuse " + v.asking.front();
            if (v.asking.size() > 1) rows[2] = "+" + std::to_string(v.asking.size() - 1) + " more ask";
        }
        break;
    case View::kJoin:
        rows[0] = v.s.by_launcher ? "Co-op came with the game" : v.s.in_one ? "In a session already" : "Paste code and join";
        if (!v.s.in_one && !v.s.by_launcher) {
            // (the fields: the row's label, then what was typed, the letter at the cursor in brackets while typing)
            rows[1] = "Room: " + (v.room_field.empty() && v.typing != 0 ? std::string("A to type its name") : v.room_field);
            rows[2] = "Password: " + (v.password_field.empty() && v.typing != 1 ? std::string("A to type it") : v.password_field);
            if (v.room_ready) {
                rows[3] = v.relay_set ? "Join this room" : "Join room: no relay set";
            } else if (v.s.rejoin) {
                rows[3] = v.rejoin_host.empty() ? "Join the last game again" : "Join " + v.rejoin_host + " again";
            }
        }
        break;
    case View::kSession: {
        if (!v.says.empty()) {   // (what co-op's menu would have said, on our rows: the reason over three)
            rows[0] = "Co-op says:";
            const std::vector<std::string> says = Wrap(v.says, 3);
            for (std::size_t i = 0; i < says.size(); ++i) rows[1 + i] = says[i];
            break;
        }
        if (v.s.hosting) {
            if (!v.note.empty()) {
                rows[0] = v.note;
            } else if (v.password_shown && !v.host_password.empty()) {
                rows[0] = "Password: " + v.host_password;
            } else {
                rows[0] = "Hosting: " + Players(std::max(v.players, 1)) + (v.s.has_password ? " (A: password)" : "");
            }
            rows[1] = !v.room.empty() ? "Copy room: " + v.room : v.s.code ? "Copy invite code" : "Room: coming...";
            rows[2] = "Copy same-network line";
            rows[3] = "Copy VPN line";
            break;
        }
        std::size_t first = 1;   // (the rows co-op's own line, its problem or hint, may take: first .. the last free)
        if (v.s.failed && !v.s.in_one) {
            rows[0] = "Co-op failed:";
            if (!v.s.by_launcher) rows[3] = "Host again";
        } else if (!v.note.empty()) {
            rows[0] = v.note;
        } else if (v.s.wait_host) {
            rows[0] = "Waiting for the host";
        } else if (v.connecting) {
            rows[0] = "Connecting...";
        } else if (v.s.in_one) {
            rows[0] = v.host_name.empty() ? "In a session" : "Joined " + v.host_name;
        } else {
            rows[0] = v.s.by_launcher ? "Co-op came with the game" : "Not in a session";
        }
        if (v.s.wait_host) {
            rows[1] = "Copy answer code";
            first = 2;
        } else if (v.s.in_one && !v.connecting && v.players > 0) {
            rows[1] = "With " + Players(v.players);
            first = 2;
        }
        if (v.s.in_one && v.s.can_resync) rows[3] = "Resync from host";
        const std::size_t last = rows[3].empty() ? 4 : 3;
        const std::vector<std::string> why = Wrap(v.why, last - first);
        for (std::size_t i = 0; i < why.size(); ++i) rows[first + i] = why[i];
        break;
    }
    }
    for (std::string& r : rows) r = r.empty() ? std::string(" ") : Fit(std::move(r));
    return rows;
}

const char* ActionWords(RowAction a) {
    switch (a) {
    case RowAction::kOpenMain: return "co-op's menu, its main page";
    case RowAction::kCopyCode: return "the room name, join code or invite code to the clipboard";
    case RowAction::kAllow: return "the oldest player who asks to join let in (/allow)";
    case RowAction::kHost: return "host now, with the kit's settings";
    case RowAction::kConfirmLeave: return "leave (or stop hosting) after a second A";
    case RowAction::kPasteAnswer: return "a friend's answer code from the clipboard (Paste answer), then co-op's main page";
    case RowAction::kCopyAnswer: return "this game's answer code to the clipboard (Copy answer)";
    case RowAction::kViewPlayers: return "the Players view";
    case RowAction::kViewLetIn: return "the Let in view";
    case RowAction::kViewJoin: return "the Join view";
    case RowAction::kBack: return "back";
    case RowAction::kDeny: return "the oldest player who asks to join refused (/deny)";
    case RowAction::kJoinPaste: return "join with the code on the clipboard";
    case RowAction::kRejoin: return "join the last game again";
    case RowAction::kViewSession: return "the Session view";
    case RowAction::kCopyLan: return "the same-network join line to the clipboard";
    case RowAction::kCopyVpn: return "the VPN join line to the clipboard";
    case RowAction::kResync: return "the host's save again (Resync from host)";
    case RowAction::kViewPlayer: return "a player's view";
    case RowAction::kMorePlayers: return "the Players view's next page";
    case RowAction::kHostControls: return "the host's controls for that player";
    case RowAction::kTeleport: return "teleport to that player (/teleport)";
    case RowAction::kOwnVoice: return "their voice muted or heard again (only here)";
    case RowAction::kOwnChat: return "their chat muted or shown again (only here)";
    case RowAction::kHostVoice: return "their voice muted or unmuted for everyone";
    case RowAction::kHostChat: return "their chat muted or unmuted for everyone";
    case RowAction::kKick: return "kick that player after a second A";
    case RowAction::kTypeRoom: return "the room name typed on its row";
    case RowAction::kViewLan: return "the games on this network";
    case RowAction::kJoinLan: return "that game's view: its password, then Join this game";
    case RowAction::kJoinLanGame: return "join that game on this network, with the password typed";
    case RowAction::kMoreLan: return "the next page of games on this network";
    case RowAction::kAutoRejoin: return "auto-rejoin on or off";
    case RowAction::kShowPassword: return "the join password shown or hidden";
    case RowAction::kTypePassword: return "the join password typed on its row";
    case RowAction::kJoinRoom: return "join that room through the kit's relay";
    case RowAction::kBan: return "ban that player for this session after a second A";
    case RowAction::kNone: break;
    }
    return "nothing";
}

// --- ScreenGuard ---

namespace {
thread_local bool t_in_screen = false;
}  // namespace

ScreenGuard::ScreenGuard() : nested_(t_in_screen) {
    t_in_screen = true;
}

ScreenGuard::~ScreenGuard() {
    if (!nested_) t_in_screen = false;
}

// --- Probe ---

std::vector<std::string> Probe::Seen(const Facts& f, std::uint64_t now_ms) {
    std::vector<std::string> out;
    if ((f.kind != Kind::kPages && f.kind != Kind::kStrip) || f.screen == 0) {
        return out;
    }
    Last& last = f.kind == Kind::kPages ? pages_ : strip_;
    const char* what = f.kind == Kind::kPages ? "the pause menu's pages (PlusMenu_00)" : "the tab strip (MenuOverlay_00)";
    if (last.screen != f.screen) {
        last = Last{};
        last.screen = f.screen;
        last.first_ms = now_ms;
        out.push_back("coop: pause tab: " + std::string(what) + ", screen " + Hex(f.screen) +
                      ": its updates reach the module (patch 0030); " + Shows(f));
        last.facts = f;
    } else if (Shows(f) != Shows(last.facts)) {
        out.push_back("coop: pause tab: " + std::string(what) + ": " + Shows(f) + " (was " + Shows(last.facts) + ")");
        last.facts = f;
    }
    ++last.updates;
    if (!last.counted && now_ms - last.first_ms >= 1000) {
        last.counted = true;
        out.push_back("coop: pause tab: " + std::string(what) + ": " + std::to_string(last.updates) +
                      " updates in its first " + std::to_string(now_ms - last.first_ms) + " ms");
    }
    return out;
}

}  // namespace CoopPauseTab
