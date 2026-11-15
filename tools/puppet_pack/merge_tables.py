"""One merged resource size table at each start (#48: a randomizer seed from OOTRR's website).

Longshot serves ONE ResourceSizeTable: the one of the mod indexed last (host/mods.c with our patch 0011: the Mods
tab's list `coop-mods.list`, its first row last; mods the list does not name under the listed ones, by name). A seed
folder made on OOTRR's page has its own table, made without the puppet pack's entries, so the puppets cannot load.
This writes ONE table where the game reads it: the winner's own entries, and from every other mod that is on, each
entry it has (the larger size where both have one). The winner's original is kept in
`<winner>/_coop-size-table/` (Longshot skips folders whose name starts with '_'), with `merged.txt`, the files the
merge was made from and the file it wrote (`<path under mods> <length> <FNV-1a 64>`), so the next start merges again
only when one of them changed. Last, `<winner>/coop-merged.txt` (beside its romfs) tells the co-op module the table
it serves holds the puppets' entries (coop_rando MergedByKit): `coop-merged 1` / `table <file name> <FNV-1a 64 of
that table>` / `pack <FNV-1a 64 of the puppet pack's table of that name>`; it holds the puppets again when either
file changed since. `--undo` puts every original back.

Our own seeds (`randomizer-*`) and the puppet pack are left alone when they win: the module builds a seed beside
every other table already (coop_rando StageSizeTables).

    python -I merge_tables.py --mods <game>/mods [--romfs <game>/romfs] [--undo]

Prints what it did, one line each. Exit 0 also when there is nothing to do; 1 when a table could not be merged
(said why; the game starts anyway and the module says the puppets are off).
"""
import argparse
import os
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import rstb, zs  # noqa: E402
else:
    from . import rstb, zs

LIST_FILE = "coop-mods.list"
KEEP_DIR = "_coop-size-table"
STAMP = "merged.txt"
MARK = "coop-merged.txt"   # (the module's kMergedFile)
STAMP_HEAD = "coop-merged 1"
RESOURCE = ("romfs", "System", "Resource")
TAIL = ".rsizetable.zs"
CONTROL = "oot-remake-randomizer"
PACK_DIR = "contents/0400486024DEC000/coop-puppet"   # (the module's kPackFolder)
PACK = PACK_DIR.lower()   # (folded)
MAX_DEPTH = 4   # mods/contents/<title>/coop-puppet is at depth 3


class MergeError(Exception):
    pass


def fold(name):
    """Names as Hookshot compares them: ASCII letters without case, '\\' as '/'."""
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in name.replace("\\", "/"))


def find_mods(root):
    """Every mod folder (a folder holding romfs or exefs) under root: {name with '/': path}."""
    out = {}

    def walk(path, rel, depth):
        try:
            entries = sorted(os.scandir(path), key=lambda e: e.name)
        except OSError:
            return
        for e in entries:
            if not e.is_dir() or e.name[:1] in "-_":
                continue
            name = f"{rel}/{e.name}" if rel else e.name
            if os.path.isdir(os.path.join(e.path, "romfs")) or os.path.isdir(os.path.join(e.path, "exefs")):
                out[name] = e.path
            elif depth < MAX_DEPTH:
                walk(e.path, name, depth + 1)

    walk(root, "", 1)
    return out


def read_list(root):
    """The Mods tab's rows [(name, off)] from coop-mods.list, or [] (no list, or not version 1)."""
    try:
        with open(os.path.join(root, LIST_FILE), "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return []
    lines = text.split("\n")
    if lines[0].rstrip("\r") != "coop-mods 1":
        return []
    rows, seen = [], set()
    for line in lines[1:]:
        line = line.rstrip("\r")
        if line.startswith("on ") and len(line) > 3:
            name, off = line[3:], False
        elif line.startswith("off ") and len(line) > 4:
            name, off = line[4:], True
        else:
            continue
        if fold(name) in seen:
            continue
        seen.add(fold(name))
        rows.append((name, off))
    return rows


def load_order(root):
    """The mods that are on, in the order Longshot indexes them (the last one's file wins)."""
    rows = read_list(root)
    rank = {fold(n): (-1 if off else len(rows) - i) for i, (n, off) in enumerate(rows)}
    mods = find_mods(root)
    on = [(rank.get(fold(n), 0), fold(n), n) for n in mods if rank.get(fold(n), 0) >= 0]
    return [(n, mods[n]) for _, _, n in sorted(on)]


def tables_of(path):
    d = os.path.join(path, *RESOURCE)
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(TAIL) and len(f) > len(TAIL)
                      and os.path.isfile(os.path.join(d, f)))
    except OSError:
        return []


def is_ours(name):
    top = fold(name).split("/", 1)[0]
    return top.startswith("randomizer-") or top == CONTROL


def fnv64_bytes(data):
    """FNV-1a 64 of bytes, 16 lower-case hex: coop_rando's Fnv64Hex."""
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def fnv64(path):
    """(FNV-1a 64 hex of the file's bytes, its length)."""
    with open(path, "rb") as f:
        data = f.read()
    return fnv64_bytes(data), len(data)


def file_line(root, path):
    """`<path under root with '/'> <length> <FNV-1a 64 hex>` (coop_rando TablesStamp's line)."""
    h, n = fnv64(path)
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    return f"{rel} {n} {h}"


def write_mark(root, win_path, fname):
    """The module's mark (coop_rando MergedByKit), written last. None without a puppet pack table of that name."""
    pack = os.path.join(root, *PACK_DIR.split("/"), *RESOURCE, fname)
    mark = os.path.join(win_path, MARK)
    if not os.path.isfile(pack):
        if os.path.isfile(mark):
            os.remove(mark)
        return None
    table = fnv64(os.path.join(win_path, *RESOURCE, fname))[0]
    text = f"coop-merged 1\ntable {fname} {table}\npack {fnv64(pack)[0]}\n"
    write_atomic(mark, text.encode("utf-8"))
    return mark


def stamp_text(root, out_path, inputs, out_line=None):
    lines = [STAMP_HEAD, "out " + (out_line or file_line(root, out_path))]
    lines += sorted("in " + file_line(root, p) for p in inputs)
    return "\n".join(lines) + "\n"


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


class Codec:
    """Decompresses and compresses tables; the game's dictionaries only when a frame names one."""

    def __init__(self, romfs):
        self.romfs = romfs
        self._dicts = None

    def dicts(self):
        if self._dicts is None:
            if not self.romfs or not os.path.isfile(os.path.join(self.romfs, "Pack", "ZsDic.pack.zs")):
                raise MergeError("a size table needs the game's dictionaries (romfs/Pack/ZsDic.pack.zs), not found")
            self._dicts = zs.Dicts.from_romfs(self.romfs)
        return self._dicts

    def load(self, raw):
        if raw[:4] != zs.ZSTD_MAGIC:
            return rstb.Rstb.loads(raw)
        if zs.frame_dict_id(raw) == 0:
            z = zs._zstd()
            return rstb.Rstb.loads(z.ZstdDecompressor().decompressobj().decompress(raw))
        return rstb.Rstb.loads(self.dicts().decompress(raw))

    def save(self, table, like_raw):
        data = table.dumps()
        if like_raw[:4] != zs.ZSTD_MAGIC:
            return data
        did = zs.frame_dict_id(like_raw)
        if did == 0:
            z = zs._zstd()
            return z.ZstdCompressor(level=16, write_content_size=True, write_checksum=False).compress(data)
        d = self.dicts()
        name = next((n for n, x in d.by_name.items() if x.dict_id() == did), None)
        if name is None:
            raise MergeError(f"no dictionary with id {did}")
        return d.compress(data, name)


def merge(base, others):
    """base with every entry of others (the larger size where both have one). (added, raised)"""
    added = raised = 0
    for t in others:
        for rows, theirs in ((base.by_crc, t.by_crc), (base.by_name, t.by_name)):
            for k, size in theirs.items():
                have = rows.get(k)
                if have is None:
                    rows[k] = size
                    added += 1
                elif size > have:
                    rows[k] = size
                    raised += 1
    return added, raised


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def merge_mods(root, romfs, say=print):
    """Merges the winning table of each table name. Returns the number of tables it could not merge."""
    order = [(n, p, tables_of(p)) for n, p in load_order(root)]
    codec = Codec(romfs)
    failed = 0
    for fname in sorted({f for _, _, fs_ in order for f in fs_}):
        having = [(n, p) for n, p, fs_ in order if fname in fs_]
        win_name, win_path = having[-1]
        if len(having) < 2 or is_ours(win_name) or fold(win_name).startswith("contents/"):
            continue  # (one table, one of our seeds, or a title's own mod such as the puppet pack)
        try:
            failed += merge_one(root, codec, fname, win_name, win_path, having[:-1], say)
        except (OSError, ValueError, MergeError) as e:
            try:
                os.remove(os.path.join(win_path, MARK))  # (an old mark must not keep the puppets on)
            except OSError:
                pass
            say(f"Size tables: {win_name} could not be merged ({e}): the other players can't be shown with it on.")
            failed += 1
    return failed


def merge_one(root, codec, fname, win_name, win_path, others, say):
    table_path = os.path.join(win_path, *RESOURCE, fname)
    keep = os.path.join(win_path, KEEP_DIR)
    orig_path = os.path.join(keep, fname)
    stamp_path = os.path.join(keep, STAMP)
    inputs = [os.path.join(p, *RESOURCE, fname) for _, p in others]
    old = read_text(stamp_path)
    if old is not None and old == stamp_text(root, table_path, inputs):
        write_mark(root, win_path, fname)
        say(f"Size tables: {win_name} has the other mods' entries already (merged at an earlier start).")
        return 0
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, table_path)] \
        and os.path.isfile(orig_path)
    if not ours:  # (the mod's own table: a new seed put there, or the first start with it)
        os.makedirs(keep, exist_ok=True)
        shutil.copyfile(table_path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    table = codec.load(orig_raw)
    loaded = []
    for (n, _), p in zip(others, inputs):
        with open(p, "rb") as f:
            loaded.append(codec.load(f.read()))
    added, raised = merge(table, loaded)
    data = codec.save(table, orig_raw) if (added or raised) else orig_raw
    # The stamp names the table about to be written BEFORE it is written: stopped in between, the next start sees the
    # table is not that one (still the original) and keeps it as the original, never a merged table as the original.
    rel = os.path.relpath(table_path, root).replace(os.sep, "/")
    write_atomic(stamp_path, stamp_text(root, table_path, inputs,
                                        f"{rel} {len(data)} {fnv64_bytes(data)}").encode("utf-8"))
    write_atomic(table_path, data)
    write_mark(root, win_path, fname)
    names = ", ".join("the puppet pack" if fold(n) == PACK else n for n, _ in others)
    say(f"Size tables: the game reads {win_name}'s; merged in the entries of {names} "
        f"({added} added, {raised} made larger). Its own table is kept in {win_name}/{KEEP_DIR}"
        " (merge_tables.py --undo puts it back).")
    return 0


def undo(root, say=print):
    """Puts each kept original back, but only over the table the merge wrote: a new seed put into the folder since
    keeps its own table (the kept one is the old seed's). The kept folder and the mark go either way."""
    n = 0
    for name, path in find_mods(root).items():
        keep = os.path.join(path, KEEP_DIR)
        stamp = read_text(os.path.join(keep, STAMP))
        if stamp is None:
            continue
        out = next((l[4:] for l in stamp.split("\n") if l.startswith("out ")), None)
        back = False
        for f in os.listdir(keep):
            table = os.path.join(path, *RESOURCE, f)
            if f.endswith(TAIL) and os.path.isfile(table) and out == file_line(root, table):
                shutil.copyfile(os.path.join(keep, f), table)
                back = True
        shutil.rmtree(keep)
        if os.path.isfile(os.path.join(path, MARK)):
            os.remove(os.path.join(path, MARK))
        say(f"Size tables: {name}'s own table is back." if back else
            f"Size tables: {name} has another table than the merged one now (a new seed?): left as it is.")
        n += 1
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--mods", required=True)
    ap.add_argument("--romfs", default="")
    ap.add_argument("--undo", action="store_true")
    a = ap.parse_args(argv)
    if not os.path.isdir(a.mods):
        return 0
    if a.undo:
        undo(a.mods)
        return 0
    try:
        return 1 if merge_mods(a.mods, a.romfs) else 0
    except SystemExit:  # (zs._zstd: no zstandard package)
        print("Size tables: not merged (this Python has no zstandard package): with a mod that has its own size "
              "table, the other players can't be shown.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
