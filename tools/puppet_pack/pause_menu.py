"""The pause menu's Multiplayer tab (issue #264, piece 4; docs/hookshot.md "The pause menu's
Multiplayer tab"): a fifth tab after System, and its page, in the game's own look. Made from the
player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs
      MenuOverlay_00.buil (the tab strip): Pa_Category_04, a copy of the fourth tab, after System;
          the R label moved right of it. The game's four tabs and L stay where they are: the
          strip's own animation places the tabs (seen in a run, 2026-11-18: moved ones went back)
      PlusMenu_00.buil (the pages): Pa_Coop_00, a copy of the System page, where a page shows
  Mals/<language>.Product.110.sarc.zs
      LayoutMsg/MenuOverlay_00.msbt: the tab's name; LayoutMsg/PlusMenu_00.msbt: the page's five
      rows (strings/en.txt and its translations: labels named for the panes they fill, as the
      game's own Pa_System_00-Pa_Button_00-T_Btn_00)
and grows those files' entries in the resource size table.

Both copies are stored hidden (ui_pack's Copy.hidden): without the co-op module the pause menu looks
as it always did, and only the module shows them.

What the game does with them (read from the game's code, not seen in a game yet):
  - the tab strip's set-up counts its Pa_Category_%02d parts and makes a tab for each, so it
    makes the fifth itself; it fills and lights only the four it knows (System and before);
  - no code of the game binds Pa_Coop_00 (the menu binds its four pages by name), and the game
    makes a page's buttons in that page's own code, so the copy has no game buttons;
  - the co-op module (hookshot/src/coop_pause_page) shows the page over System's and lights the tab
    when R is pressed on System, and gives System back on L.

Where the copies go (ui_pack.py's notes): neither the tabs' pane nor the pages' pane reaches the
layout's last pane, so a copy under them would break the pane order. The tab goes under RootPane,
where the fifth tab sits on the screen; the page under N_InOut_00 (the pages' and the menu's
In and Out), where the shown page sits: the pages are side by side on a strip (System at x 12840)
that the menu slides, and the System page shows when the strip has slid by its x.
"""
import glob
import os
import struct

from . import ui_pack

COMMON_ARCHIVE = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
OVERLAY_LAYOUT = "MenuOverlay_00.buil"
PAGES_LAYOUT = "PlusMenu_00.buil"
OVERLAY_MSG = "LayoutMsg/MenuOverlay_00.msbt"
PAGES_MSG = "LayoutMsg/PlusMenu_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"

TAB = "Pa_Category_%02d"
GAME_TABS = 4                 # Pa_Category_00..03
NEW_TAB = TAB % GAME_TABS     # Pa_Category_04
TAB_ROOT = "RootPane"
L_LABEL, R_LABEL = "T_BtnL_00", "T_BtnR_00"
SYSTEM_PAGE = "Pa_System_00"
NEW_PAGE = "Pa_Coop_00"
PAGE_PARENT = "N_InOut_00"

PauseMenuError = ui_pack.UiPackError


def _new_id(name):
    """The game's own id for a pane of that name (ui_pack.part_id, CityHash64): it finds panes by it."""
    return ui_pack.part_id(name)


FIFTH_GAP = 30.0   # the fifth tab this much closer to System than the game's step (430 for 460): R fits before the
R_MOVE = 340.0     # rupee counter's icon, R moved this far right (990 -> 1330 in the game's layout)


def _x(d, name):
    panes, _ = ui_pack.read_layout(d)
    for p in panes:
        if p.name == name:
            return struct.unpack_from("<f", d, p.record + 0x1c)[0]
    raise PauseMenuError("the layout has no %s" % name)


def add_tab(d):
    """MenuOverlay_00.buil with a fifth tab: a copy of the fourth (Pa_Category_03) a little less
    than the game's step to its right (the step from tab 0 to tab 1, less FIFTH_GAP), under
    RootPane, and the R label moved R_MOVE to the right of it. The game's four tabs and L stay:
    the strip's animation sets the tabs' places, so moving them in the layout does nothing."""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for i in range(GAME_TABS):
        if TAB % i not in names:
            raise PauseMenuError("the layout has no %s" % (TAB % i))
    if NEW_TAB in names:
        raise PauseMenuError("the layout has %s already" % NEW_TAB)
    for name in (L_LABEL, R_LABEL, TAB_ROOT):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    last = TAB % (GAME_TABS - 1)
    step = _x(d, TAB % 1) - _x(d, TAB % 0)
    if step <= 0 or any(abs(_x(d, TAB % (i + 1)) - _x(d, TAB % i) - step) > 0.5 for i in range(GAME_TABS - 1)):
        raise PauseMenuError("the game's tabs are not evenly spaced from left to right")
    off = ui_pack.frame_offset(d, last, TAB_ROOT)
    d = ui_pack.clone_parts(d, [ui_pack.Copy(last, NEW_TAB, (step - FIFTH_GAP + off[0], off[1]), parent=TAB_ROOT,
                                             hidden=True)], new_id=_new_id)
    return ui_pack.move_panes(d, {R_LABEL: (R_MOVE, 0.0)})


def add_page(d):
    """PlusMenu_00.buil with the Multiplayer page: a copy of the System page (Pa_System_00) under
    N_InOut_00 where a shown page sits (the strip's slide taken off: x 0, the System page's y)."""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for name in (SYSTEM_PAGE, PAGE_PARENT):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    if NEW_PAGE in names:
        raise PauseMenuError("the layout has %s already" % NEW_PAGE)
    off = ui_pack.frame_offset(d, SYSTEM_PAGE, PAGE_PARENT)
    move = (-_x(d, SYSTEM_PAGE), off[1])   # x: the page's place on the strip taken off (the strip itself sits at 0)
    return ui_pack.clone_parts(d, [ui_pack.Copy(SYSTEM_PAGE, NEW_PAGE, move, parent=PAGE_PARENT, hidden=True)], new_id=_new_id)


# ---------------------------------------------------------------------------------------- build

class _Dir:
    def __init__(self, root):
        self.root = root

    def read(self, rel):
        with open(os.path.join(self.root, *rel.split("/")), "rb") as f:
            return f.read()

    def glob(self, pattern):
        n = len(self.root.rstrip("/\\")) + 1
        return [p[n:].replace("\\", "/") for p in glob.glob(os.path.join(self.root, *pattern.split("/")))]


class _Layered(_Dir):
    """The mod's romfs over the game's: a file an earlier pack step wrote (title_menu.py's language archives) is read
    as it wrote it, so its labels stay; the game's files otherwise."""
    def __init__(self, top, root):
        super().__init__(root)
        self.top = top

    def read(self, rel):
        path = os.path.join(self.top, *rel.split("/"))
        if os.path.isfile(path):
            with open(path, "rb") as f:
                return f.read()
        return super().read(rel)


def build(romfs, out_romfs, dicts, table, write, recompress, log=print, strings=ui_pack.STRINGS):
    """The Common archive and every language's messages from the romfs folder, into out_romfs
    (write(out_romfs, rel, bytes)); their size table entries grown (`table`: rstb.Rstb). Returns
    the files written. All or nothing, as title_menu.build: a failure raises before anything is
    written, so the pack never holds the tab without its page and texts."""
    files, growth = make(_Layered(out_romfs, romfs), dicts, recompress, strings)
    for rel, _, required in growth:
        if required and table.get(_key(rel)) is None:
            raise PauseMenuError("the size table has no %s" % _key(rel))
    for rel, by, required in growth:
        _grow(table, rel, by, required)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("pause menu: the Multiplayer tab and page (%s, %s; %d languages)" % (OVERLAY_LAYOUT, PAGES_LAYOUT, len(files) - 1))
    return list(files)


def make(reader, dicts, recompress, strings=ui_pack.STRINGS):
    """({rel: compressed bytes}, [(rel, bytes grown, required)]) for the Common archive and every
    language's messages; raises PauseMenuError (or the reader's error) when any one cannot be made."""
    from . import sarc
    files, growth = {}, []
    raw = reader.read(COMMON_ARCHIVE)
    plain = dicts.decompress(raw)
    entries = sarc.read_entries(plain)
    for layout, edit in ((OVERLAY_LAYOUT, add_tab), (PAGES_LAYOUT, add_page)):
        olds = [data for name, data in entries if name == layout]
        if not olds:
            raise PauseMenuError("%s has no %s" % (COMMON_ARCHIVE, layout))
        entries, new = ui_pack.edit_entries(entries, layout, edit)   # (every copy of the layout)
        growth.append((layout, len(new[olds[0]]) - len(olds[0]), False))
    archive = sarc.write(entries, data_align=16)   # (as the game's own: 16)
    growth.append((COMMON_ARCHIVE, len(archive) - len(plain), True))
    files[COMMON_ARCHIVE] = recompress(dicts, raw, archive)
    for rel in sorted(reader.glob(MALS_GLOB)):
        ours = ui_pack.load_strings(ui_pack.language_of(rel), strings)
        raw = reader.read(rel)
        plain = dicts.decompress(raw)
        msgs = sarc.read_entries(plain)
        have = {name for name, _ in msgs}
        if OVERLAY_MSG not in have or PAGES_MSG not in have:
            continue
        for f in (OVERLAY_MSG, PAGES_MSG):
            if not ours.get(f):
                raise PauseMenuError("the strings have no [%s]" % f)
        packed = sarc.write([(name, ui_pack.add_labels(data, ours[name], replace=True)
                              if name in (OVERLAY_MSG, PAGES_MSG) else data) for name, data in msgs], data_align=16)
        growth.append((rel, len(packed) - len(plain), True))
        files[rel] = recompress(dicts, raw, packed)
    if len(files) < 2:
        raise PauseMenuError("no language has %s and %s" % (OVERLAY_MSG, PAGES_MSG))
    return files, growth


def _key(rel):
    return rel[:-3] if rel.endswith(".zs") else rel


def _grow(table, rel, by, required=True):
    key = _key(rel)
    size = table.get(key)
    if size is None:
        if required:
            raise PauseMenuError("the size table has no %s" % key)
        return
    table.set(key, size + max(0, by) + 0x1000)
