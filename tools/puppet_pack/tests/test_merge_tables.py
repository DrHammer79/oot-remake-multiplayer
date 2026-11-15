"""Tests for merge_tables.py (#48: a seed from OOTRR's website beside the puppet pack). Made-up tables only.

    python tools/puppet_pack/tests/test_merge_tables.py
"""
import contextlib
import io
import os
import random
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import merge_tables as mt, rstb, sarc  # noqa: E402

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None

NAME = "ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs"
PACK = "contents/0400486024DEC000/coop-puppet"


def read(path):
    with open(path, "rb") as f:
        return f.read()


@unittest.skipIf(zstandard is None, "needs zstandard")
class MergeTablesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_tables_test_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        os.makedirs(self.mods)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def say(self, line):
        self.lines.append(line)

    def path(self, mod, *rest):
        return os.path.join(self.mods, *mod.split("/"), *rest)

    def put_table(self, mod, by_crc, by_name=None, compress=True, dict_data=None):
        t = rstb.Rstb(by_crc=by_crc, by_name=by_name or {})
        raw = t.dumps()
        if compress:
            raw = zstandard.ZstdCompressor(dict_data=dict_data, write_content_size=True).compress(raw)
        p = self.path(mod, "romfs", "System", "Resource", NAME)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(raw)
        return p

    def table(self, mod):
        with open(self.path(mod, "romfs", "System", "Resource", NAME), "rb") as f:
            return mt.Codec(self.romfs).load(f.read())

    def run_merge(self):
        self.lines = []
        return mt.merge_mods(self.mods, self.romfs, self.say)

    def test_a_site_seed_that_wins_gets_the_pack_entries_and_keeps_its_own(self):
        self.put_table(PACK, {1: 100, 2: 50, 3: 9}, {"Puppet/new.bfres": 77})
        site = self.put_table("ootrr-site", {1: 120, 2: 40, 4: 5})
        with open(site, "rb") as f:
            own = f.read()
        self.assertEqual(self.run_merge(), 0)
        t = self.table("ootrr-site")
        self.assertEqual(t.by_crc, {1: 120, 2: 50, 3: 9, 4: 5})  # each entry, the larger size
        self.assertEqual(t.by_name, {"Puppet/new.bfres": 77})
        self.assertIn("ootrr-site", self.lines[0])
        self.assertIn("the puppet pack", self.lines[0])
        with open(self.path("ootrr-site", mt.KEEP_DIR, NAME), "rb") as f:
            self.assertEqual(f.read(), own)  # the original, kept
        # The next start: nothing to do; the stamp is what the module checks.
        self.run_merge()
        self.assertIn("already", self.lines[0])
        stamp = read(self.path("ootrr-site", mt.KEEP_DIR, mt.STAMP)).decode("utf-8").split("\n")
        self.assertEqual(stamp[0], "coop-merged 1")
        self.assertTrue(stamp[1].startswith(f"out ootrr-site/romfs/System/Resource/{NAME} "))
        self.assertTrue(stamp[2].startswith(f"in {PACK}/romfs/System/Resource/{NAME} "))
        # The module's mark (coop_rando MergedByKit): the table written and the pack's, by FNV-1a 64.
        mark = read(self.path("ootrr-site", mt.MARK)).decode("utf-8")
        table_hash = mt.fnv64(site)[0]
        pack_hash = mt.fnv64(self.path(PACK, "romfs", "System", "Resource", NAME))[0]
        self.assertEqual(mark, f"coop-merged 1\ntable {NAME} {table_hash}\npack {pack_hash}\n")
        # Undo puts the site's own table back.
        mt.undo(self.mods, self.say)
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.MARK)))
        with open(site, "rb") as f:
            self.assertEqual(f.read(), own)
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.KEEP_DIR)))

    def test_a_pack_made_again_is_merged_again_from_the_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table(PACK, {1: 100, 3: 30})  # (a kit update made the pack again)
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 2: 5, 3: 30})
        self.assertEqual(rstb.Rstb.loads(zstandard.ZstdDecompressor().decompress(read(
            self.path("ootrr-site", mt.KEEP_DIR, NAME)))).by_crc, {2: 5})  # still the site's own

    def test_a_new_seed_put_over_the_merged_one_is_kept_as_the_new_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table("ootrr-site", {7: 70})  # another seed from the site, same folder
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 7: 70})
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {7: 70})

    def test_undo_leaves_a_new_seed_put_in_after_the_merge_alone(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table("ootrr-site", {7: 70})  # another seed from the site, no start since
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {7: 70})  # not the old seed's {2: 5}
        self.assertIn("left as it is", self.lines[-1])
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.KEEP_DIR)))
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.MARK)))

    def test_a_merge_stopped_after_the_stamp_keeps_the_true_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        real = mt.write_atomic
        calls = []

        def stop_at_the_table(path, data):
            if path.endswith(NAME):
                calls.append(path)
                raise OSError("stopped")
            real(path, data)

        mt.write_atomic = stop_at_the_table
        try:
            self.assertEqual(self.run_merge(), 1)
        finally:
            mt.write_atomic = real
        self.assertTrue(calls)  # the stamp was written, the table not
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 2: 5})
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {2: 5})  # the site's own, not a merged one

    def test_no_zstandard_is_one_line_not_a_stop(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        real = mt.zs._zstd

        def none():
            raise SystemExit("this tool needs the 'zstandard' Python package")

        mt.zs._zstd = none
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                self.assertEqual(mt.main(["--mods", self.mods]), 1)
        finally:
            mt.zs._zstd = real
        self.assertIn("no zstandard", out.getvalue())

    def test_our_seeds_and_the_pack_are_left_alone(self):
        self.put_table(PACK, {1: 100})
        self.put_table("randomizer-S1", {2: 5})  # ours: built beside every table already
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.lines, [])
        self.assertEqual(self.table("randomizer-S1").by_crc, {2: 5})
        self.put_table("aaa-textures", {3: 3})  # under the pack (by name): the pack's table wins
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.lines, [])
        self.assertEqual(self.table("aaa-textures").by_crc, {3: 3})

    def test_the_mods_list_decides_the_winner_and_mods_switched_off_are_left_out(self):
        self.put_table(PACK, {1: 100})
        self.put_table("zz-site", {2: 5})
        self.put_table("aa-site", {3: 6})
        self.put_table("off-mod", {9: 9})
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\non aa-site\non zz-site\noff off-mod\n")
        self.run_merge()
        self.assertEqual(self.table("aa-site").by_crc, {1: 100, 2: 5, 3: 6})  # the first row wins, 9 left out
        self.assertEqual(self.table("zz-site").by_crc, {2: 5})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], [PACK, "zz-site", "aa-site"])

    def test_folders_with_a_leading_dash_or_underscore_are_no_mods(self):
        self.put_table(PACK, {1: 100})
        self.put_table("-old-site", {2: 5})
        self.put_table("_backup", {3: 5})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], [PACK])

    def test_an_uncompressed_table_stays_uncompressed(self):
        self.put_table(PACK, {1: 100})
        self.put_table("site", {2: 5}, compress=False)
        self.run_merge()
        raw = read(self.path("site", "romfs", "System", "Resource", NAME))
        self.assertEqual(rstb.Rstb.loads(raw).by_crc, {1: 100, 2: 5})

    def test_a_table_with_the_games_dictionary_is_merged_with_it(self):
        rnd = random.Random(2)
        samples = [bytes(rnd.randrange(8) for _ in range(200)) for _ in range(300)]
        d = zstandard.train_dictionary(2048, samples, dict_id=7)
        os.makedirs(os.path.join(self.romfs, "Pack"))
        with open(os.path.join(self.romfs, "Pack", "ZsDic.pack.zs"), "wb") as f:
            f.write(zstandard.ZstdCompressor().compress(sarc.write({"zs.zsdic": d.as_bytes()})))
        self.put_table(PACK, {1: 100}, dict_data=d)
        self.put_table("site", {2: 5}, dict_data=d)
        self.run_merge()
        raw = read(self.path("site", "romfs", "System", "Resource", NAME))
        self.assertEqual(mt.zs.frame_dict_id(raw), 7)  # compressed as the site's was
        self.assertEqual(self.table("site").by_crc, {1: 100, 2: 5})

    def test_a_table_that_needs_a_missing_dictionary_is_said_and_left_as_it_was(self):
        rnd = random.Random(3)
        d = zstandard.train_dictionary(2048, [bytes(rnd.randrange(8) for _ in range(200)) for _ in range(300)],
                                       dict_id=7)
        self.put_table(PACK, {1: 100})
        site = self.put_table("site", {2: 5}, dict_data=d)
        before = read(site)
        self.assertEqual(self.run_merge(), 1)
        self.assertIn("could not be merged", self.lines[0])
        self.assertEqual(read(site), before)
        self.assertFalse(os.path.exists(self.path("site", mt.KEEP_DIR, mt.STAMP)))
        self.assertFalse(os.path.exists(self.path("site", mt.MARK)))

    def test_the_stamp_line_is_the_modules(self):
        p = os.path.join(self.mods, "a b", "t.bin")
        os.makedirs(os.path.dirname(p))
        with open(p, "wb") as f:
            f.write(b"abc")
        self.assertEqual(mt.file_line(self.mods, p), "a b/t.bin 3 e71fa2190541574b")  # FNV-1a 64 of "abc"
        self.assertEqual(mt.fnv64(p), ("e71fa2190541574b", 3))


if __name__ == "__main__":
    unittest.main()
