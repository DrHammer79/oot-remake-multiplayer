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
- **ExeFS** (v1 step 8): the list is rasterised in software into one RGBA picture the game draws with one picture
  pane of our own layout (the pane is step 8 e2, not written yet). Text comes from a font atlas built on the player's
  PC from the HUD pack (no game data in this repository). The alternatives (the game's own 2D layout system pane by
  pane; an NVN2 renderer of our own) and their sizes and risks are in the coordinator's v1 reports.

## Software raster (v1 step 8 e1)

`coop/ui/raster.h` `CoopRaster` draws a `CoopDrawList` into a picture the caller owns (`coop_raster_init` with the
pixels and a work buffer of `coop_raster_work_bytes`; no allocation, no function pointers, so it can go into the
payload). `coop_raster_draw` clears what the last frame drew and draws every command in order:

- Rects (rounded), circles and rings, triangles, convex polygons, polygon outlines (1.5 px bands along the edges, as
  the Longshot replay draws them), text from a `CoopRasterFont` atlas (bilinear, any size), and the HUD pack's images
  (`CoopRasterImage`, bilinear, tinted). A text whose font has no atlas, an image not set, NaN numbers or an unknown
  command are skipped and counted (`skipped`).
- The same pixels everywhere: numbers become 1/16 px integers once, then it is integer arithmetic only. Edges are
  anti-aliased from 4 sub-rows times 1/16 px per pixel. `tests/ui/raster_test.cpp` checks exact pixels and two golden
  pictures that every CI compiler must give byte for byte.
- The picture is 0xAABBGGRR with premultiplied alpha (draw it with "one, one minus source alpha");
  `coop_raster_to_straight` converts rows for a pane that wants straight alpha.
- Dirty rows: each frame hashes every row it drew (64-bit) and compares with the frame before; `coop_raster_dirty`
  gives the changed rows as bands, so the pane uploads only those.
- Text widths for the layout come from the same atlas: `coop_raster_font_advances` fills a `CoopFontAdvances`.
  The measure and the raster walk text with the same decoder (`coop/ui/utf8.hpp`), so bad bytes move the pen as
  the measure counts them.

Memory: a 1280 x 720 picture is 3.6 MB plus about 20 KB of work.

## Upload to the game's textures (v1 step 8 e2, first part)

`coop/ui/upload.h`, for the ExeFS picture pane (the pane itself comes after a research run):

- `coop_draw_list_hash`: a frame whose list hashes the same as the last one is not rasterised or uploaded again.
- `CoopUpload`: the picture is shown through two textures in turn (the GPU may still read the one shown last), so a
  changed row is pending for every texture until each has had it; a new texture starts with every row pending (the
  first frame sends the whole picture). `coop_upload_take` gives one texture's pending rows as bands, merging bands
  closer than a gap and stopping at a row budget per frame; what is left waits for the next frame, and a texture is
  shown only once nothing is pending for it.

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
back to. While it is open the game gets no pad buttons, keys or mouse, and after it closes no button that was held then
(B for Back, Minus, A) reaches the game until it is let go (`coop/ui/pad.hpp` `PadGate`). It opens only where
Hookshot draws through `hos_coop_draw2`: an older Hookshot (`hos_coop_draw` only), or one that never draws it within a
second, turns it off with one log line; drawn before and then not for a second (a long frame), it just closes. The Longshot replay draws a polygon as
triangles (an outline as thin bands).

## Input (v1 step 6b)

- `coop/ui/events.h` `CoopUiEvent`: up, down, left, right, confirm, back, tab previous / next, open, close, a typed
  character, backspace, text done, pointer move / click. The same on every platform.
- `coop/ui/pad.hpp` `PadReader`: the game's own NpadButton bits and left stick (what Longshot's `hos_coop_pad` and an
  ExeFS build's nn::hid hook both see) as events. A held direction repeats (after 400 ms, then every 110 ms). Closed,
  only L + R + Minus counts. Open: the D-pad or stick moves, A confirms, B goes back, L / R change the tab, Minus closes.
- `coop/ui/focus.hpp` `FocusStack`: menu, chat and cheats as owners. The top one gets input. Any owner takes the
  keyboard; only the menu takes the pad too, because the chat box and the cheats panel are typed into while the game is
  still played. On Longshot there is one stack for all three, in `coop_own_menu.cpp`, and it works with or without the
  opt-in. The chat box and the cheats panel tell it when they open and close (`SetFocus`, after each key in
  `TypeKey`). The typed keys go to the cheats panel first as before (when it is open, or when the box is closed and the
  cheats are on), except when the chat box is on top: then it keeps its keys (`CheatsFirst`).
- Keyboard (Longshot): the arrows, Enter, Esc / Backspace, Q / E or Page Up / Down. While the page types into a field,
  every key but F9 goes to that field. As with the pad, Enter, Esc, Backspace or F9 still held when the menu closes
  stay co-op's (`hos_coop_keys_taken`) until they are let go, so the game reads none of them as a new press
  (`KeyLatch`). On a console, text comes from the software keyboard as typed characters followed
  by "text done" (step 8).

## Next

- The ExeFS backend for the draw list, and the console input (software keyboard) (step 8).
- The mods' drawing API (`coop_mods_runtime.cpp` Overlay) still draws on Hookshot's table directly.
