# Co-op's own UI layer

Co-op draws its own UI (chat, name tags, the other players under your hearts, the player list, the panel, item
cheats, title lines, the down ring, players on the maps) instead of hooking the game's menu screens. On every
platform it is drawn the same way: the overlay code appends commands to a **draw list**, and a **backend** draws
the list. This is v1 step 6 (the 'everywhere' plan): the list is the part an ExeFS build and Longshot share.

## The draw list (`coop/ui/include/coop/ui/drawlist.h`, library `coop_ui`)

`CoopDrawList` is plain C: a fixed array of `CoopDrawCmd` (2048) and a text arena (32 KB), no allocation and no
function pointers, so it can live inside a payload a recomp lifts and on a console.

| command | data |
|---|---|
| `COOP_DRAW_RECT` | filled rectangle x0 y0 x1 y1, corner rounding |
| `COOP_DRAW_CIRCLE` | centre, radius, thickness (0: filled) |
| `COOP_DRAW_TRIANGLE` | three points, filled |
| `COOP_DRAW_POLY` | convex polygon of 3 to 8 points, filled or outline |
| `COOP_DRAW_TEXT` | top left, size in px, UTF-8 words in the arena |
| `COOP_DRAW_IMAGE` | a logical image (the HUD pack's hearts) into a rectangle, tinted |
| `COOP_DRAW_FONT` | the logical font for the texts after it |
| `COOP_DRAW_CLIP_PUSH` / `POP` | a clip rectangle |

- Colours are `0xAABBGGRR` (Dear ImGui's IM_COL32), coordinates pixels from the picture's top left.
- Fonts and images are logical ids (`COOP_FONT_UI`, `COOP_FONT_REGULAR`, `COOP_FONT_MEDIUM`, `COOP_IMAGE_HEART_0..4`);
  each backend maps them to its own.
- Repeated words (a text's outline and shadow copies) are stored once.
- When the list is full the rest of the frame is dropped and counted in `dropped`; nothing is written past the end.
  On Longshot `CoopDraw::Draw` returns that count, the module logs it once ("the overlay's draw list was full"), and
  Hookshot's own font is set back after every replay even when the list's last font command was lost. Sixteen
  players with everything on fit with at least 128 commands to spare (`drawlist_test.cpp`).
- Text widths: `coop_text_width` sums a font's advances from a table (`CoopFontAdvances`): data, no font engine.

## Who emits it

`hookshot/src/coop_draw.cpp` `CoopDraw::Build(list, measure, scene, game_look)` and `coop_dbno_draw.cpp` draw onto a
`CoopDraw::Canvas` (the list, the picture's size and a `Measure` for text widths). `Measure` takes Hookshot's own
`text_width` on Longshot (so the layout is exactly what it was) or the advance tables elsewhere.

## Backends

- **Longshot / Hookshot** (`hookshot/src/coop_draw_hos.cpp`): `Replay` turns each command into the HosCoopDraw call the
  overlay made before the list existed, in the same order. `CoopDraw::Draw(table, scene, look)` is Build then Replay,
  so the module's exports did not change. `tests/hookshot/draw_golden_test.cpp` records every call of 25 scenes
  (plain and game look, old and new table) from the code before the list and must keep passing.
- **ExeFS** (later, v1 step 8): not written yet. Recommended route: rasterise the list in software (portable,
  unit-tested, the same pixels everywhere) into one RGBA texture the game draws with one picture pane of our own
  layout; text from a font atlas built on the player's PC from the HUD pack (no game data in this repository).
  The alternatives (the game's own 2D layout system pane by pane; an NVN2 renderer of our own) and their sizes and
  risks are in the coordinator's v1 reports.

## Own menu (v1 step 6b)

Co-op's own menu screen, for platforms with no Hookshot menu. `coop/ui/menu_view.hpp` `DrawMenu` lays out a tab strip,
the page's rows (action, choice with arrows, Off / On toggle, heading, note; fixed rows dim; scrolled so the selected
row shows), a help column with the page's picture (its shapes as `COOP_DRAW_POLY`) and a prompt strip into the draw
list; `RowAt` / `TabAt` say what the pointer is on. The rows are the ones Hookshot's menu shows on Longshot
(`hos_coop_menu.h`), so the page code (CoopPause) is shared. It is our own screen; it never touches the game's menus.

On Longshot it is **opt-in**: `LICHII_COOP_OWN_MENU=1` (`hookshot/src/coop_own_menu.*`). Off, which is the default,
none of it runs and Hookshot's menu is as before (the golden test pins the overlay). On, F9 or the pad's L + R held
then Minus opens it over the picture (drawn through `hos_coop_draw2`; an older Hookshot's table draws none), and it
shows the same Multiplayer and Mods tabs as Hookshot's menu. Minus or F9 closes it, or Back with nothing left to go
back to. While it is open the game gets no pad buttons, keys or mouse. The Longshot replay draws a polygon as
triangles (an outline as thin bands).

## Input (v1 step 6b)

- `coop/ui/events.h` `CoopUiEvent`: up, down, left, right, confirm, back, tab previous / next, open, close, a typed
  character, backspace, text done, pointer move / click. The same on every platform.
- `coop/ui/pad.hpp` `PadReader`: the game's own NpadButton bits and left stick (what Longshot's `hos_coop_pad` and an
  ExeFS build's nn::hid hook both see) as events. A held direction repeats (after 400 ms, then every 110 ms). Closed,
  only L + R + Minus counts. Open: the D-pad or stick moves, A confirms, B goes back, L / R change the tab, Minus closes.
- `coop/ui/focus.hpp` `FocusStack`: menu, chat and cheats as owners. The top one gets input, and the game's pad is
  masked while any owner is on the stack. Step 6b puts the own menu on it; the chat box and cheats keep their own
  key handling for now and move onto it with the ExeFS adapter.
- Keyboard (Longshot): the arrows, Enter, Esc / Backspace, Q / E or Page Up / Down. While the page types into a field,
  every key but F9 goes to that field. On a console, text comes from the software keyboard as typed characters followed
  by "text done" (step 8).

## Next

- The ExeFS backend for the draw list, and the console input (software keyboard) (step 8).
- The mods' drawing API (`coop_mods_runtime.cpp` Overlay) still draws on Hookshot's table directly.
