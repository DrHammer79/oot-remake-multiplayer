# Chat as the game's own UI

Co-op's chat is shown by the game itself, not drawn over the picture: no box, no panel, no drawn text. The module
(`hookshot/src/coop_native_ui.*`, glue in `coop_hookshot.cpp` `NativeStep`) hands our text to the game's own screens
on the game's thread at the frame hook, so the game's font (in the game's language: Chinese, Japanese and Korean
games show those letters), frames, animations and sounds are used, and the game hides these screens itself in
cutscenes and menus.

Game: main of build `e0f3ccfaf50065ae` (1.1.1, [game-builds.md](game-builds.md)). Addresses are `main+` offsets in the
code; `f_<address>` below is the function at that address with main at `0x80020000`. Each was read again on the 1.1.1
main with `tools/game_scan` for this change (2026-11-23); the screens themselves are in [game-ui.md](game-ui.md).

## What shows where

| Co-op | Game screen | Layout and panes |
|---|---|---|
| A chat line | the in-game message window: the speaker on its name plate, in the player's colour, the line under it | `MessageInGame_00` (`T_Name_00`, `T_Message_00` = the pane `T_MessageWhiteOut_00`) |
| A room notice ("Saria joined") | the banner | `SimpleInfo_00` (`T_Text_00`) |
| What you type (T) | the message window: "you", the text and a caret, changing as you type; the banner while the window holds a message of the game's own | `MessageInGame_00` |
| A held line (down but not out: "You are down  0:32", from `coop_dbno_draw`) | the banner, kept up while held; notices wait | `SimpleInfo_00` |

## How

- A text pane shows its text from a message reference `{const char* utf8, u32 bytes, i64 -1}` and keeps the pointer,
  so our texts live in one block of the game's memory from its own malloc (`TextRing`, 48 slots of 512 bytes used in
  turn, and a scratch for call arguments). A pane is set with the game's setter `f_8045b970(layout, pane name,
  &reference, 1, 0)`; the pane names are strings the game already has (`T_Name_00` `0x83d141c6`, `T_Text_00`
  `0x83de2306`).
- Other players' text is cleaned first: the game's text has tags (byte `0x0E`, group, type, size, parameters), so every
  byte below `0x20` and `DEL` goes, a bad UTF-8 sequence becomes `?`, and text is cut at a whole character.
- **It fits.** Lines are wrapped for the panes as the layout sizes them (`T_MessageWhiteOut_00` 2200 x 200 at 0.9: two
  lines; `T_Text_00` 2100 x 86 at 0.8: one line; the name plate 1250 at 0.8): 44 columns a line, two lines a page in
  the window, 48 columns on the banner, 20 for a name, a Chinese, Japanese or Korean character two columns. A long line
  is several pages shown in turn, never cut and never past the pane. The column counts are a safe guess at the game's
  font (NOT proven: the LANE1 screenshot run settles them; `kLineColumns` and the others are in one place).
- **A chat line**, as the game's own `f_833d25c0` shows a message: the window from `f_833d2690(*0x84ea4518)` (the
  hash of `MessageInGame_00`), checked by its vtable (`0x84c09678`); the speaker into `T_Name_00`; the line's
  reference where `f_834091d0` puts the game's (`+0x2b0`); its time at `+0x408` (counted down by the window's slot 40
  step, closed at 0 by its slot 6; read as 30 a second, NOT proven); the show flag `+0x2d0` unless the window is
  closed with no open asked (state `+0x1a6` 0 and request `+0x1a5` below 1, as at `0x833d262c`); then the open call
  (slot 5, mode 1). A line that changes while it shows (the typed line) only swaps its text when the window is open.
- **Never over the game's own.** While the window holds a text not in our block and its time is not out, our line
  waits (tried again each second, dropped after 20 s). Lines show one page at a time, 3 s plus 60 ms a column (at most
  8 s); at most 12 pages wait, the oldest go first.
- **Colours.** The speaker's name plate is in the player's colour (the HUD's and tags' `CoopPuppet::NameColor`): the
  text pane's own two colours (`f_8152c740`, `f_8152c810`; the colour tag has only 6). The pane comes from
  `f_8078e2b0(layout, "T_Name_00")` and is used only when it is a text pane (kind 2) whose text box holds colours;
  the game's own colours are read first and put back as soon as no line of ours is on the window.
- **A notice**: the banner is found in the UI manager's list (`**0x84c9e098`, count `+8`, screens `+0x10`) by its
  vtable (`0x84c0d7c8`); our text goes into `T_Text_00`, then its open call (mode 2).
- **A held line** (`CoopNativeUi::HoldBanner(text)` / `EndBanner()`, from any thread): the banner is opened when it is
  shut, its text set only when it changes, and it is closed (slot 6) when let go.
- **Typing**: while the chat box is open the typed line is on the message window; lines that come meanwhile go to the
  banner; closing the box ends the line at once.

## Switches and fallbacks

- `LICHII_COOP_NATIVE_UI=0`, or a game that gives no memory for the texts: the chat is drawn as before, as text only
  (no box behind the lines or the chat box, no red frame: a send that failed shows in red).
- On the title and during loads the game has no message window: lines wait (up to 20 s) for it.

## Log lines

- `coop: native ui: chat in the game's message window and banner (texts at 0x...)`
- `coop: native ui: first chat line shown by the game (message window|banner)`
- `coop: native ui: the game's message window is not free now; the line waits`
- `coop: native ui: typing in the game's message window`
- `coop: native ui: the game gave no memory for its texts; chat drawn as before`

## Not proven yet (one LANE1 screenshot run)

Every step above in a game: the window's time unit, that the window shows our text on the name plate and the line,
the name colour, the banner's own close, that the columns fit at 16:9 and a narrow window, in English and in Chinese.
