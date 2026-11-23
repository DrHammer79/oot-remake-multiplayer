"""Character Select voices, built on the player's PC from the player's own game files (docs/puppet.md,
"Character voices").

No sound is shipped. For each character, every one of Link's voice clips (Vo_Pl<Age>_*) gets a stand-in cut
from clips the game already has for that character's voice type (voice_table.json). The clips are trimmed,
scaled to Link's loudness, encoded and written into the shape of Link's own clip: an archive
PlayerVoice_<Age>_<key>.bars under Link's name hashes, plus a stream file for each clip that Link streams. The
Character Select hook turns a request for Player_<Age> into the list Player_<Age>_V<NN>; the list names our
archive before Link's, so the game finds our clip first. voice_map.check() is the gate: when a rule breaks, no
voice is written and the pack is built as before.
"""
import json
import os
import zlib

import numpy as np

from . import bwav
from . import voice_map as vm

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_table.json")
RESOURCE = "Sound/Resource"
STREAM = "Sound/Resource/Stream"
VARIANTS = ("Child", "Adult")
SILENCE_DB = 40.0      # a 10 ms frame this far below the loudest is silence at the ends
PEAK = 32767 * 10 ** (-1 / 20)
FADE_S = 0.02          # fade-out when a clip is cut to the length cap


class VoiceError(ValueError):
    pass


class RomfsFiles:
    """The player's game files for build(): archives unpacked, stream files as they are."""

    def __init__(self, romfs, dicts):
        self.romfs, self.dicts = romfs, dicts

    def bars(self, rel):
        p = os.path.join(self.romfs, *(rel + ".zs").split("/"))
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return self.dicts.decompress(f.read())

    def stream(self, rel):
        p = os.path.join(self.romfs, *rel.split("/"))
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return f.read()


def load_table(path=TABLE):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("format") != 1:
        raise VoiceError(f"{os.path.basename(path)}: format 1 wanted")
    return doc["characters"]


def bank_of(voice):
    """The archive a voice type's clips are in: Custom_X -> NV_USen_Custom_X; enemies, animals and NV_ names as
    they are; any other NPC type X -> NV_X."""
    if voice.startswith("Custom_"):
        return "NV_USen_" + voice
    if voice.startswith(("Enemy_", "Animal_", "NV_")):
        return voice
    return "NV_" + voice


def db(x):
    return 20 * np.log10(max(float(x), 1e-9) / 32768.0)


def trim(x, rate):
    """x without the silence at both ends (10 ms frames more than SILENCE_DB below the loudest)."""
    hop = max(1, rate // 100)
    m = len(x) // hop
    if m < 2:
        return x
    f = np.sqrt((x[:m * hop].astype(np.float64).reshape(m, hop) ** 2).mean(axis=1))
    if f.max() <= 0:
        return x[:hop]
    on = np.nonzero(f >= f.max() * 10 ** (-SILENCE_DB / 20))[0]
    return x[on[0] * hop:min(len(x), (on[-1] + 1) * hop)]


def rms_db(x):
    x = x.astype(np.float64)
    return db(np.sqrt((x * x).mean())) if len(x) else -120.0


def resample(x, rate, to):
    if rate == to or not len(x):
        return x
    n = max(1, int(round(len(x) * to / rate)))
    return np.interp(np.arange(n) * rate / to, np.arange(len(x)), x.astype(np.float64)).astype(np.int16)


def half_db(g):
    """A gain to the nearest 0.5 dB: clips that differ less share one encoded clip."""
    return round(g * 2) / 2


def fit(x, gain_db):
    """x scaled by gain_db, its peaks above -1 dBFS bent down softly (no hard clipping)."""
    y = x.astype(np.float64) * 10 ** (gain_db / 20)
    knee = 0.7 * PEAK
    a = np.abs(y)
    over = a > knee
    a[over] = knee + (PEAK - knee) * np.tanh((a[over] - knee) / (PEAK - knee))
    return (np.sign(y) * a).round().astype(np.int16)


def cut(x, rate, seconds):
    n = int(seconds * rate)
    if len(x) <= n:
        return x
    y = x[:n].astype(np.float64)
    k = min(n, int(FADE_S * rate))
    y[n - k:] *= np.linspace(1, 0, k)
    return y.astype(np.int16)


class Bank:
    """One archive of the player's game: its clip names and their samples."""

    def __init__(self, files, name):
        self.files, self.name = files, name
        self.data = files.bars(f"{RESOURCE}/{name}.bars")
        self.names = bwav.bars_names(self.data) if self.data is not None else {}

    def samples(self, name):
        _, at = self.names[name]
        if at is None:
            return None, 0
        if bwav.header(self.data, at)["prefetch"]:
            raw = self.files.stream(f"{STREAM}/{name}.bwav")
            if raw is None:
                return None, 0
            return bwav.decode(raw)
        return bwav.decode(self.data, at)


class Voice:
    """A voice type's usable clips: {class: [(name, trimmed samples, rms dB)]}, at Link's rate."""

    def __init__(self, files, voice, rate):
        self.voice = voice
        self.by_class = {}
        bank = Bank(files, bank_of(voice))
        for name in sorted(bank.names):
            c = vm.clip_class(name)
            if c is None:
                continue
            x, r = bank.samples(name)
            if x is None or not len(x):
                continue
            x = trim(resample(x, r, rate), rate)
            self.by_class.setdefault(c, []).append((name, x, rms_db(x)))

    def __bool__(self):
        return bool(self.by_class)


class Link:
    """Link's voice clips for one age: names, BWAV offsets, AMTA blocks, lengths and loudness."""

    def __init__(self, files, age):
        self.age = age
        self.data = files.bars(f"{RESOURCE}/Player_{age}.bars")
        if self.data is None:
            raise VoiceError(f"no {RESOURCE}/Player_{age}.bars")
        self.names = {n: e for n, e in bwav.bars_names(self.data).items()
                      if n.startswith(f"Vo_Pl{age}_") and e[1] is not None}
        bank = Bank.__new__(Bank)
        bank.files, bank.data, bank.names = files, self.data, self.names
        self.length, self.rms = {}, {}
        rates = set()
        for n in self.names:
            x, r = bank.samples(n)
            if x is None:
                raise VoiceError(f"Link's {n} could not be read")
            rates.add(r)
            self.length[n] = len(x) / r
            self.rms[n] = rms_db(trim(x, r))
        if len(rates) != 1:
            raise VoiceError(f"Link's {age} clips have rates {sorted(rates)}")
        self.rate = rates.pop()
        loud = sorted(v for v in self.rms.values() if v > -60)
        self.median_rms = loud[len(loud) // 2] if loud else -20.0
        self.events = {}
        for n in sorted(self.names):
            self.events.setdefault(vm.event_of(n), []).append(n)

    def target(self, event):
        """The loudness a stand-in aims at: the median of Link's clips for the event (near-silent ones left out),
        or Link's median over all his clips."""
        loud = sorted(self.rms[n] for n in self.events[event] if self.rms[n] > -60)
        return loud[len(loud) // 2] if loud else self.median_rms

    def longest(self, event):
        return max(self.length[n] for n in self.events[event])


def choose(event, voices, n, cap, rate):
    """n (voice, class, name, samples, rms) for one event: the first voice of the chain with a clip of a class
    in prefs(event) that fits the cap, rotated to n. When none fits, the shortest clip there is, cut to the cap."""
    for v in voices:
        for c in vm.prefs(event):
            fits = [k for k in v.by_class.get(c, []) if cap is None or len(k[1]) <= cap * rate]
            if fits:
                return [(v.voice, c) + fits[i % len(fits)] for i in range(n)]
    for v in voices:
        for c in vm.prefs(event):
            if v.by_class.get(c):
                name, x, r = min(v.by_class[c], key=lambda k: len(k[1]))
                y = cut(x, rate, cap)
                return [(v.voice, c, name, y, rms_db(y))] * n
    return []


def stream_code(i):
    """Two letters for a voice archive's stream files: 'VAaPl...' keeps the length of 'Vo_Pl...'."""
    if i >= 26 * 26:
        raise VoiceError("too many voice archives")
    return chr(65 + i // 26) + chr(97 + i % 26)


def build(files, characters, table=None, log=print):
    """The voice files and lists for the characters ({stored number: name}).

    files: .bars(rel) -> the unpacked archive or None; .stream(rel) -> a stream file's bytes or None.
    -> {"files": {rel: bytes} (archives unpacked, streams as they are), "lists": {age: {number: archive}},
        "clips": [vm.Clip]}. Raises VoiceError or ValueError (a broken rule) when no voice may be written."""
    table = load_table() if table is None else table
    links = {a: Link(files, a) for a in VARIANTS}
    # Each character's row; the characters that share a chain and default share an archive.
    keys, chars = {}, []
    for number, name in sorted(characters.items()):
        row = table.get(name)
        if row is None:
            raise VoiceError(f"{name} (#{number}) is not in {os.path.basename(TABLE)}")
        if row.get("link"):
            chars.append(vm.Character(name, (), vm.LINK))
            continue
        chain, default = tuple(row["chain"]), row["default"]
        chars.append(vm.Character(name, chain, default, bool(row.get("creature"))))
        keys.setdefault(chain + (default,), []).append(number)
    wanted = sorted({v for k in keys for v in k})
    out_files, lists, clips = {}, {a: {} for a in VARIANTS}, []
    built, creatures, cache, encoded = set(), set(), {}, {}
    for age in VARIANTS:
        link = links[age]
        voices = {}
        for v in wanted:
            if (v, link.rate) not in cache:
                cache[v, link.rate] = Voice(files, v, link.rate)
            voices[v] = cache[v, link.rate]
            if voices[v]:
                built.add(v)
            if v.startswith(("Enemy_", "Animal_", "NV_Cow")):
                creatures.add(v)
        for i, key in enumerate(sorted(keys)):
            archive = f"PlayerVoice_{age}_{key[0]}" + ("" if sum(k[0] == key[0] for k in keys) == 1 else f"_{i}")
            code = stream_code(i)
            chain = [voices[v] for v in key if voices[v]]
            slots = {}
            for event, names in link.events.items():
                cap = vm.cap_s(event, link.longest(event))
                for name, got in zip(names, choose(event, chain, len(names), cap, link.rate)):
                    slots[name] = got
            # Each slot's clip at the gain that meets Link's loudness for the event; one clip at one gain is
            # encoded once for every archive.
            order = sorted(slots)
            todo = []
            for n in order:
                voice, _, source, x, level = slots[n]
                gain = half_db(link.target(vm.event_of(n)) - level)
                k = (age, voice, source, len(x), gain)
                if k not in encoded:
                    encoded[k] = None
                    todo.append((k, fit(x, gain)))
            for (k, y), cd in zip(todo, bwav.encode_adpcm([y for _, y in todo]) if todo else ()):
                encoded[k] = (y, cd)
            assets, streams = [], {}
            for n in order:
                voice, _, source, x, level = slots[n]
                y, (coefs, data) = encoded[(age, voice, source, len(x),
                                            half_db(link.target(vm.event_of(n)) - level))]
                amta, at = link.names[n]
                head, stream = bwav.clip_bytes(link.data, len(y), coefs, data, at)
                if stream is not None:
                    # One stream file for equal clips whose names have the same length (the AMTA keeps its size).
                    same = zlib.crc32(stream), len(stream), len(n)
                    sname = streams.get(same)
                    if sname is None:
                        sname = streams[same] = "V" + code + n[3:]
                        out_files[f"{STREAM}/{sname}.bwav"] = stream
                    amta = bwav.with_amta_name(amta, sname)
                assets.append((zlib.crc32(n.encode("latin1")), amta, head))
                ev = vm.event_of(n)
                clips.append(vm.Clip(archive, age, n, source, len(y) / link.rate, link.longest(ev), rms_db(y),
                                     link.target(ev)))
            out_files[f"{RESOURCE}/{archive}.bars"] = bwav.bars_build(assets)
            for number in keys[key]:
                lists[age][number] = archive + ".bars"
        log(f"voices: {age}: {len(keys)} voice archives, {len(link.names)} of Link's clips each")
    link_names = {a: set(links[a].names) for a in VARIANTS}
    vm.check(chars, built, creatures, clips, link_names)
    return {"files": out_files, "lists": lists, "clips": clips}


def list_entry(base, archive):
    """A Player_<Age>_V<NN> list: the voice archive first, then the game's own Player_<Age> list."""
    entry = {k: list(v) if isinstance(v, list) else v for k, v in base.items()}
    entry["Bars"] = [archive] + [b for b in base.get("Bars", []) if b != archive]
    return entry
