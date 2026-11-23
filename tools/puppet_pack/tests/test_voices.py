"""Tests for the Character Select voice builder (voices.py). Made-up archives and clips only: no game files.

    python tools/puppet_pack/tests/test_voices.py
"""
import os
import sys
import unittest
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import bwav, voices  # noqa: E402
from test_bwav import amta, sine, template  # noqa: E402

RATE = 48000


def clip(x, prefetch=False):
    """(archive BWAV, stream file or None) for samples x."""
    (coefs, data), = bwav.encode_adpcm([x])
    return bwav.clip_bytes(template(1, 280, len(x)) if prefetch else template(0, 14), len(x), coefs, data)


class Files:
    """A made-up game: archives {rel: [(name, samples, prefetch)]}."""

    def __init__(self, archives):
        self.archives, self.streams = {}, {}
        for rel, rows in archives.items():
            assets = []
            for name, x, prefetch in rows:
                head, stream = clip(x, prefetch)
                if stream is not None:
                    self.streams[f"{voices.STREAM}/{name}.bwav"] = stream
                assets.append((zlib.crc32(name.encode()), amta(name), head))
            self.archives[rel] = bwav.bars_build(assets)

    def bars(self, rel):
        return self.archives.get(rel)

    def stream(self, rel):
        return self.streams.get(rel)


def game(saria=True):
    link = {}
    for age in ("Child", "Adult"):
        p = f"Vo_Pl{age}_"
        link[f"{voices.RESOURCE}/Player_{age}.bars"] = [
            (p + "SwordAttack_S_01", sine(int(0.30 * RATE), 300), False),
            (p + "SwordAttack_S_02", sine(int(0.25 * RATE), 320), False),
            (p + "Wait_01", sine(int(0.80 * RATE), 200, 3000), True),
            ("SE_Player_Not_A_Voice", sine(2000), False)]
    banks = {
        "NV_Hylia_Girl_Spry": [("NV_Hylia_Girl_Spry_Painful00", sine(int(0.30 * RATE), 600, 2000), False)],
        "Enemy_ReDead": [("Enemy_ReDead_DamageVoice_00", sine(int(0.40 * RATE), 90, 12000), False),
                         ("Enemy_ReDead_Footstep_00", sine(int(0.10 * RATE), 90), False)],
    }
    if saria:
        banks["NV_USen_Custom_Saria"] = [
            ("NV_Custom_Saria_WeaponSwingS00", sine(int(0.33 * RATE), 700, 1500), False),
            ("NV_Custom_Saria_WeaponSwingS01", sine(int(0.90 * RATE), 650, 1500), False),   # too long for a swing
            ("NV_Custom_Saria_TalkPositive00", sine(int(0.30 * RATE), 500), False),         # speech: never used
            ("NV_Custom_Saria_Ease00", sine(int(1.20 * RATE), 400, 1500), True)]
    link.update({f"{voices.RESOURCE}/{b}.bars": rows for b, rows in banks.items()})
    return Files(link)


TABLE = {"Link": {"link": True},
         "Saria": {"chain": ["Custom_Saria"], "default": "Hylia_Girl_Spry", "creature": False},
         "Fado": {"chain": ["Kokiri_Girl_Spry"], "default": "Hylia_Girl_Spry", "creature": False},
         "Gibdo": {"chain": ["Enemy_ReDead"], "default": "Enemy_ReDead", "creature": True}}
CHARS = {0: "Link", 2: "Saria", 7: "Fado", 108: "Gibdo"}


class Build(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = game()
        cls.out = voices.build(cls.files, CHARS, TABLE, log=lambda *_: None)

    def test_each_character_but_link_gets_an_archive_per_age(self):
        for age in ("Child", "Adult"):
            got = self.out["lists"][age]
            self.assertNotIn(0, got)
            self.assertEqual(got[2], f"PlayerVoice_{age}_Custom_Saria.bars")
            self.assertEqual(got[7], f"PlayerVoice_{age}_Kokiri_Girl_Spry.bars")
            self.assertEqual(got[108], f"PlayerVoice_{age}_Enemy_ReDead.bars")
            self.assertIn(f"{voices.RESOURCE}/{got[2]}", self.out["files"])

    def test_every_link_voice_clip_has_a_stand_in_and_nothing_else(self):
        d = self.out["files"][f"{voices.RESOURCE}/PlayerVoice_Child_Custom_Saria.bars"]
        assets, _ = bwav.read_bars(d)
        want = {zlib.crc32(n.encode()) for n in ("Vo_PlChild_SwordAttack_S_01", "Vo_PlChild_SwordAttack_S_02",
                                                 "Vo_PlChild_Wait_01")}
        self.assertEqual(set(assets), want)

    def test_clips_come_from_the_characters_own_voice_and_fit(self):
        by = {(c.voice, c.link_name): c for c in self.out["clips"]}
        swing = by["PlayerVoice_Child_Custom_Saria", "Vo_PlChild_SwordAttack_S_01"]
        self.assertEqual(swing.source, "NV_Custom_Saria_WeaponSwingS00")
        self.assertLessEqual(swing.length, swing.link_longest + 0.15)
        self.assertEqual(by["PlayerVoice_Child_Custom_Saria", "Vo_PlChild_Wait_01"].source,
                         "NV_Custom_Saria_Ease00")
        self.assertNotIn("NV_Custom_Saria_TalkPositive00", {c.source for c in self.out["clips"]})
        # A voice type the game lacks falls to the default of the character's sex and age.
        self.assertEqual(by["PlayerVoice_Child_Kokiri_Girl_Spry", "Vo_PlChild_Wait_01"].source,
                         "NV_Hylia_Girl_Spry_Painful00")
        self.assertEqual(by["PlayerVoice_Child_Enemy_ReDead", "Vo_PlChild_SwordAttack_S_01"].source,
                         "Enemy_ReDead_DamageVoice_00")

    def test_loudness_meets_links(self):
        for c in self.out["clips"]:
            self.assertLessEqual(abs(c.rms_db - c.link_rms_db), 3.0, (c.voice, c.link_name))

    def test_a_streamed_slot_gets_its_own_stream_file(self):
        d = self.out["files"][f"{voices.RESOURCE}/PlayerVoice_Child_Custom_Saria.bars"]
        assets, _ = bwav.read_bars(d)
        meta, at = assets[zlib.crc32(b"Vo_PlChild_Wait_01")]
        name = bwav.amta_name(meta)
        self.assertEqual(len(name), len("Vo_PlChild_Wait_01"))
        self.assertTrue(name.startswith("V") and not name.startswith("Vo_"))
        stream = self.out["files"][f"{voices.STREAM}/{name}.bwav"]
        self.assertEqual(stream[8:12], d[at + 8:at + 12])
        x, _ = bwav.decode(stream)
        self.assertGreater(np.abs(x.astype(np.int32)).max(), 1000)

    def test_list_entry_puts_the_voice_archive_first(self):
        base = {"Bars": ["Player.bars", "Player_Child.bars"], "ChoiceAsset": ["a.casset.byml"]}
        e = voices.list_entry(base, "PlayerVoice_Child_Custom_Saria.bars")
        self.assertEqual(e["Bars"], ["PlayerVoice_Child_Custom_Saria.bars", "Player.bars", "Player_Child.bars"])
        self.assertEqual(e["ChoiceAsset"], ["a.casset.byml"])
        self.assertEqual(base["Bars"], ["Player.bars", "Player_Child.bars"])


class Refused(unittest.TestCase):
    def test_a_character_not_in_the_table(self):
        with self.assertRaises(voices.VoiceError):
            voices.build(game(), {2: "Saria", 50: "Someone New"}, TABLE, log=lambda *_: None)

    def test_a_missing_default_voice_stops_the_voices(self):
        table = dict(TABLE, Saria={"chain": ["Custom_Saria"], "default": "Hylia_Woman_1", "creature": False})
        with self.assertRaises(ValueError) as e:
            voices.build(game(saria=False), {2: "Saria"}, table, log=lambda *_: None)
        self.assertIn("default voice Hylia_Woman_1 was not built", str(e.exception))

    def test_links_other_models_build_with_links_clips(self):
        table = dict(TABLE, **{"Link (opening)": {"link": True}})
        out = voices.build(game(), {1: "Link", 116: "Link (opening)", 2: "Saria"}, table, log=lambda *_: None)
        self.assertEqual(set(out["lists"]["Child"]), {2})

    def test_bank_names(self):
        self.assertEqual(voices.bank_of("Custom_Saria"), "NV_USen_Custom_Saria")
        self.assertEqual(voices.bank_of("Hylia_Boy"), "NV_Hylia_Boy")
        self.assertEqual(voices.bank_of("Enemy_Wolfos"), "Enemy_Wolfos")
        self.assertEqual(voices.bank_of("NV_Cow"), "NV_Cow")

    def test_the_shipped_table_names_a_default_for_every_character(self):
        table = voices.load_table()
        for name, row in table.items():
            if row.get("link"):
                continue
            self.assertTrue(row["chain"] and row["default"], name)
            for v in row["chain"] + [row["default"]]:
                self.assertFalse(voices.vm.is_link_voice(v), (name, v))   # no non-Link character uses Link's clips
            if row["creature"]:
                self.assertTrue(row["default"].startswith(("Enemy_", "Animal_", "NV_Cow")), name)


if __name__ == "__main__":
    unittest.main()
