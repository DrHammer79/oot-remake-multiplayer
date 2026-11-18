"""Tests for the pause menu's Multiplayer tab and page (pause_menu.py). Synthetic data only: no
game files.

    python tools/puppet_pack/tests/test_pause_menu.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import msbt, sarc, ui_pack, pause_menu as pm  # noqa: E402
from test_ui_pack import make_layout, make_msbt  # noqa: E402

# Shaped as the game's tab strip: four tabs under their own pane, inside the header; the L and R
# labels after them; a footer last. make_layout places pane i at (100 i, -10 i), so the tabs are
# 100 apart, and the header's panes move them (600, -60) from the root.
OVERLAY = [("RootPane", -1), ("N_All_00", 0), ("N_Header_00", 1), ("N_MenuChange_00", 2), ("Pa_Category_00", 3),
           ("Pa_Category_01", 3), ("Pa_Category_02", 3), ("Pa_Category_03", 3), ("N_InOut_01", 2),
           ("T_BtnL_00", 8), ("T_BtnR_00", 8), ("N_Footer_00", 0), ("Pa_Guide_00", 11)]
OVERLAY_PARTS = {"Pa_Category_%02d" % i: ("PaMenuCategory_00", [("State", float(i))]) for i in range(4)}
OVERLAY_PARTS["Pa_Guide_00"] = ("PaGuideKey_00", [])

# Shaped as the game's pages: four pages side by side on a strip, under the menu's In and Out pane,
# which reaches the last pane; an info pane after them.
PAGES = [("RootPane", -1), ("N_All_00", 0), ("N_InOut_00", 1), ("N_MenuChange_01", 2), ("Pa_Collect_00", 3),
         ("Pa_Equip_00", 3), ("Pa_Item_00", 3), ("Pa_System_00", 3), ("N_Info_00", 3), ("T_Text_00", 8),
         ("P_Black_00", 2)]
PAGES_PARTS = {"Pa_Collect_00": ("PaCollectMenu_00", []), "Pa_Equip_00": ("PaEquipmentMenu_00", []),
               "Pa_Item_00": ("PaItemMenu_00", []), "Pa_System_00": ("PaSystemMenu_00", [])}


def overlay():
    return make_layout(OVERLAY, OVERLAY_PARTS)


def pages():
    return make_layout(PAGES, PAGES_PARTS)


def pane(d, name):
    """(x, y, parent's name, end) of a pane."""
    panes, _ = ui_pack.read_layout(d)
    p = next(p for p in panes if p.name == name)
    x, y = struct.unpack_from("<ff", d, p.record + 0x1c)
    parent = struct.unpack_from("<I", d, p.record + 0x28)[0]
    return x, y, panes[parent].name if parent < len(panes) else None, struct.unpack_from("<I", d, p.record + 0x34)[0]


class TabTest(unittest.TestCase):
    def test_a_fifth_tab_and_the_strip_centred(self):
        d = overlay()
        n = pm.add_tab(d)
        panes, parts = ui_pack.read_layout(n)
        self.assertEqual(len(panes), len(OVERLAY) + 1)
        self.assertEqual((panes[-1].name, parts[-1].name), ("Pa_Category_04", "Pa_Category_04"))
        # the game's four, half a step (50) to the left
        self.assertEqual([pane(n, "Pa_Category_%02d" % i)[:2] for i in range(4)],
                         [(350.0, -40.0), (450.0, -50.0), (550.0, -60.0), (650.0, -70.0)])
        # the fifth one step after the fourth, in the root's frame (the header's (600, -60) added)
        x, y, parent, end = pane(n, "Pa_Category_04")
        self.assertEqual((x, y, parent, end), (650.0 + 100.0 + 600.0, -70.0 - 60.0, "RootPane", len(OVERLAY) + 1))
        self.assertEqual(pane(n, "T_BtnL_00")[:2], (850.0, -90.0))
        self.assertEqual(pane(n, "T_BtnR_00")[:2], (1050.0, -100.0))
        # the order holds: only the root reaches the copy, every other pane ends where it did
        old = [struct.unpack_from("<I", d, p.record + 0x34)[0] for p in ui_pack.read_layout(d)[0]]
        new = [struct.unpack_from("<I", n, p.record + 0x34)[0] for p in panes[:len(OVERLAY)]]
        self.assertEqual(new, [len(OVERLAY) + 1] + old[1:])
        # the copy is the fourth tab's part, with its own parameters
        rec = parts[-1].record
        self.assertEqual(ui_pack._cstr(n, struct.unpack_from("<Q", n, rec)[0]), "PaMenuCategory_00")
        self.assertEqual(panes[-1].id, pm._new_id("Pa_Category_04"))
        # stored hidden: only the co-op module shows it (bit 0 of the kind word; the source's has it)
        kind = lambda n, i: struct.unpack_from("<I", n, ui_pack.read_layout(n)[0][i].record + 0x38)[0]
        self.assertEqual(kind(n, -1), kind(d, 7) & ~1)
        self.assertTrue(kind(d, 7) & 1)

    def test_refusals(self):
        with self.assertRaises(pm.PauseMenuError):
            pm.add_tab(pm.add_tab(overlay()))   # the tab is there already
        three = [p for p in OVERLAY if p[0] != "Pa_Category_03"]
        with self.assertRaises(pm.PauseMenuError):
            pm.add_tab(make_layout(three, {k: v for k, v in OVERLAY_PARTS.items() if k != "Pa_Category_03"}))
        no_r = [p for p in OVERLAY if p[0] != "T_BtnR_00"]
        no_r = [(n, p - (p > 10)) for n, p in no_r]
        with self.assertRaises(pm.PauseMenuError):
            pm.add_tab(make_layout(no_r, OVERLAY_PARTS))
        # tabs not evenly spaced: the step is not known
        uneven = [p for p in OVERLAY]
        uneven.insert(6, ("N_Gap_00", 3))
        uneven = [(n, p + (p >= 6 and n != "N_Gap_00")) for n, p in uneven]
        with self.assertRaises(pm.PauseMenuError):
            pm.add_tab(make_layout(uneven, OVERLAY_PARTS))


class PageTest(unittest.TestCase):
    def test_the_page_where_a_shown_page_sits(self):
        d = pages()
        n = pm.add_page(d)
        panes, parts = ui_pack.read_layout(n)
        self.assertEqual((panes[-1].name, parts[-1].name), ("Pa_Coop_00", "Pa_Coop_00"))
        # under N_InOut_00, x 0 (the strip's slide taken off), the System page's y there
        self.assertEqual(pane(n, "Pa_Coop_00"), (0.0, -70.0 - 30.0, "N_InOut_00", len(PAGES) + 1))
        self.assertEqual(pane(n, "Pa_System_00"), pane(d, "Pa_System_00"))
        self.assertEqual(ui_pack._cstr(n, struct.unpack_from("<Q", n, parts[-1].record)[0]), "PaSystemMenu_00")
        word = struct.unpack_from("<I", n, panes[-1].record + 0x38)[0]
        self.assertEqual(word & 1, 0)   # stored hidden: the module shows it over System's
        # N_InOut_00 and its ancestors reach the copy; P_Black_00, the last pane before, as it was
        self.assertEqual([pane(n, p)[3] for p in ("RootPane", "N_All_00", "N_InOut_00", "P_Black_00")],
                         [len(PAGES) + 1] * 3 + [len(PAGES)])

    def test_refusals(self):
        with self.assertRaises(pm.PauseMenuError):
            pm.add_page(pm.add_page(pages()))
        with self.assertRaises(pm.PauseMenuError):
            pm.add_page(make_layout(PAGES[:7], {k: v for k, v in PAGES_PARTS.items() if k != "Pa_System_00"}))


class _Plain:
    """Dictionaries that do nothing: the synthetic files are not compressed."""
    def decompress(self, raw):
        return raw


class _Table:
    def __init__(self, sizes):
        self.sizes = dict(sizes)

    def get(self, key):
        return self.sizes.get(key)

    def set(self, key, value):
        self.sizes[key] = value


LANGS = ("EUfr", "JPja", "USen")


class BuildTest(unittest.TestCase):
    def romfs(self, root, broken=None):
        files = {pm.COMMON_ARCHIVE: sarc.write([(pm.OVERLAY_LAYOUT, overlay()), (pm.PAGES_LAYOUT, pages()),
                                                ("Other_00.buil", b"other")], data_align=16)}
        for lang in LANGS:
            over = make_msbt([("SystemMenu", "System")])
            if lang == broken:
                over = over[:0x30]   # cut short: it cannot be read
            files["Mals/%s.Product.110.sarc.zs" % lang] = sarc.write(
                {pm.OVERLAY_MSG: over, pm.PAGES_MSG: make_msbt([("Pa_System_00-Pa_Button_00-T_Btn_00", "Save")]),
                 "Other.msbt": b"x"})
        # a language archive without the pause menu's files is left alone
        files["Mals/XXen.Product.110.sarc.zs"] = sarc.write({"Other.msbt": b"x"})
        for rel, data in files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        return files

    def run_build(self, broken=None, strings=ui_pack.STRINGS):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            self.romfs(src, broken)
            table = _Table({pm.COMMON_ARCHIVE[:-3]: 100000, pm.OVERLAY_LAYOUT: 500, pm.PAGES_LAYOUT: 500})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            before = dict(table.sizes)
            written = {}

            def write(out, rel, data):
                written[rel] = data

            try:
                made = pm.build(src, "out", _Plain(), table, write, lambda d, raw, data: data, log=lambda s: None,
                                strings=strings)
            except (pm.PauseMenuError, struct.error, IndexError, ValueError) as e:
                return None, written, table.sizes, before, e
            return made, written, table.sizes, before, None

    def test_every_file_when_all_can_be_made(self):
        made, written, sizes, before, err = self.run_build()
        self.assertIsNone(err)
        self.assertEqual(set(made), {pm.COMMON_ARCHIVE} | {"Mals/%s.Product.110.sarc.zs" % l for l in LANGS})
        self.assertEqual(set(written), set(made))
        common = dict(sarc.read_entries(written[pm.COMMON_ARCHIVE]))
        self.assertIn("Pa_Category_04", [p.name for p in ui_pack.read_layout(common[pm.OVERLAY_LAYOUT])[0]])
        self.assertIn("Pa_Coop_00", [p.name for p in ui_pack.read_layout(common[pm.PAGES_LAYOUT])[0]])
        self.assertEqual(common["Other_00.buil"], b"other")
        ours = ui_pack.load_strings("USen")
        msgs = sarc.read(written["Mals/USen.Product.110.sarc.zs"])
        over = msbt.read(msgs[pm.OVERLAY_MSG])
        self.assertEqual(over.text("Pa_Category_04-T_Text_00"), [ours[pm.OVERLAY_MSG]["Pa_Category_04-T_Text_00"]])
        self.assertEqual(over.text("SystemMenu"), ["System"])
        page = msbt.read(msgs[pm.PAGES_MSG])
        for i in range(5):
            label = "Pa_Coop_00-Pa_Button_%02d-T_Btn_00" % i
            self.assertEqual(page.text(label), [ours[pm.PAGES_MSG][label]])
        self.assertEqual(page.text("Pa_System_00-Pa_Button_00-T_Btn_00"), ["Save"])
        self.assertTrue(all(sizes[k] > before[k] for k in before if k not in (pm.OVERLAY_LAYOUT, pm.PAGES_LAYOUT)))
        self.assertTrue(sizes[pm.OVERLAY_LAYOUT] > before[pm.OVERLAY_LAYOUT])

    def test_a_translation_over_english(self):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(ui_pack.STRINGS, "en.txt"), encoding="utf-8") as f:
                en = f.read()
            with open(os.path.join(folder, "en.txt"), "w", encoding="utf-8") as f:
                f.write(en)
            with open(os.path.join(folder, "fr.txt"), "w", encoding="utf-8") as f:
                f.write("[LayoutMsg/MenuOverlay_00.msbt]\nPa_Category_04-T_Text_00 = Multijoueur\n")
            made, written, _, _, err = self.run_build(strings=folder)
            self.assertIsNone(err)
            fr = msbt.read(sarc.read(written["Mals/EUfr.Product.110.sarc.zs"])[pm.OVERLAY_MSG])
            self.assertEqual(fr.text("Pa_Category_04-T_Text_00"), ["Multijoueur"])
            en_page = msbt.read(sarc.read(written["Mals/EUfr.Product.110.sarc.zs"])[pm.PAGES_MSG])
            self.assertEqual(en_page.text("Pa_Coop_00-Pa_Button_04-T_Btn_00"), ["Leave"])   # English kept

    def test_an_earlier_steps_labels_stay(self):
        """The title rows' step wrote a language archive first: the tab's labels go into that one, not the game's."""
        with tempfile.TemporaryDirectory() as root:
            src, out = os.path.join(root, "romfs"), os.path.join(root, "out")
            self.romfs(src)
            rel = "Mals/USen.Product.110.sarc.zs"
            game = sarc.read(open(os.path.join(src, *rel.split("/")), "rb").read())
            earlier = dict(game)
            earlier[pm.OVERLAY_MSG] = ui_pack.add_labels(game[pm.OVERLAY_MSG], {"TitleRow": "Host"})
            os.makedirs(os.path.join(out, "Mals"))
            with open(os.path.join(out, *rel.split("/")), "wb") as f:
                f.write(sarc.write(earlier))
            table = _Table({pm.COMMON_ARCHIVE[:-3]: 100000})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            written = {}
            pm.build(src, out, _Plain(), table, lambda o, r, data: written.__setitem__(r, data),
                     lambda d, raw, data: data, log=lambda s: None)
            over = msbt.read(sarc.read(written[rel])[pm.OVERLAY_MSG])
            self.assertEqual(over.text("TitleRow"), ["Host"])
            self.assertEqual(over.text("Pa_Category_04-T_Text_00"), ["Multiplayer"])

    def test_nothing_written_when_one_language_fails(self):
        made, written, sizes, before, err = self.run_build(broken="JPja")
        self.assertIsNotNone(err)
        self.assertEqual(written, {})
        self.assertEqual(sizes, before)


class NupFilesTest(unittest.TestCase):
    def test_the_menus_files_newest_language_archives(self):
        """A .nup build extracts the menus' archives too, and each language's newest message archive only."""
        import fnmatch
        from puppet_pack import build

        class FakeNup:
            files = ["UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs", pm.COMMON_ARCHIVE,
                     "Mals/USen.Product.100.sarc.zs", "Mals/USen.Product.110.sarc.zs", "Mals/EUfr.Product.110.sarc.zs",
                     "Mals/JPja.Product.100.sarc.zs", "Pack/ZsDic.pack.zs"]

            def glob(self, pattern):
                d, _, name = pattern.rpartition("/")
                return sorted(f for f in self.files if f.rpartition("/")[0] == d and fnmatch.fnmatchcase(f.rpartition("/")[2], name))

        self.assertEqual(build.nup_menu_files(FakeNup()), [
            "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs", pm.COMMON_ARCHIVE,
            "Mals/EUfr.Product.110.sarc.zs", "Mals/JPja.Product.100.sarc.zs", "Mals/USen.Product.110.sarc.zs"])


if __name__ == "__main__":
    unittest.main()
