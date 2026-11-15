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

## Next (not in this step)

- Our own menu screens drawn into the list (the Multiplayer and Mods tabs that Hookshot's menu draws today), for
  platforms with no Hookshot menu; on Longshot behind an opt-in switch only.
- One input model for that layer (pad navigation, text from a software keyboard, the pointer on PC) with a focus
  stack, so the game's pad is masked only while co-op's UI has it.
- The mods' drawing API (`coop_mods_runtime.cpp` Overlay) still draws on Hookshot's table directly.
