#!/usr/bin/env python3
"""Fetches the chart voice's DI recordings and packs them into the sample packs
the built-in guitar and bass voice plays (src/dsp/voice/dcvoice_pack.h reads
them):

  guitar.dcvp          semi-hollow (Gretsch Anniversary, Karoryfer, CC0)
  bass.dcvp            bass: fingers (Washburn AB-95, neck pickup) + pick
                       (Squier Jazz, both pickups) (Karoryfer, CC0)
  strat-neck.dcvp, strat-neck-middle.dcvp, strat-middle.dcvp,
  strat-middle-bridge.dcvp, strat-bridge.dcvp
                       an American Fender Stratocaster, every string and fret
                       in each of its five pickup positions (EGFxSet, CC BY 4.0)
  guitar-arts.dcvp     the guitar ARTICULATIONS every guitar model shares: real
                       palm mutes (every string x fret), pinch harmonics (every
                       string, a dozen frets each) and natural harmonics (every
                       string at the 12th / 7th / 5th fret nodes) of a Les Paul on its
                       bridge humbucker (Guitar-TECHS player 1, CC BY 4.0). Player
                       2 of the same dataset is NEVER packed: it is the held-out
                       reference tools/voice/realism_eval.py measures against.

ONE PACK PER STRAT POSITION, on purpose: the voice loads only the position(s)
the selected instrument model needs (dcvoice_models.h), never all five.

The recordings are NOT in git. This tool is the reproducible way to get them:

  refresh   resolve SELECTION (below) against each source at its PINNED
            commit / archive, download every file, and write the manifest
            (voice_pack_manifest.json: URL + SHA-256 + role per file). Run it
            only to change the selection; commit the manifest.
  build     download (or reuse the cache) exactly the manifest's files, verify
            every SHA-256, and write every pack and <out>/VOICE-CREDITS.txt.
            Deterministic: same manifest, same bytes. A full build (no --only)
            also writes <out>/voice-pack.stamp — the SHA-256 of the manifest and
            of this tool — which tools/release/New-Release.ps1 compares, so a
            release rebuilds STALE packs, not only missing ones.

  codec-golden
            print the codec's cross-language test vector (the coded bytes of
            codec_golden_signal) that tests/dsp/sample_voice_test.cpp decodes.

  python build_voice_pack.py build --out <dir> [--cache <dir>] [--only strat-bridge,...]
  python build_voice_pack.py refresh [--cache <dir>] [--only-source <repo or archive key>]

Needs Python 3.9+ and numpy. No ffmpeg: the sources are PCM WAV (EGFxSet's
come in one pinned zip from Zenodo, read in place).

PACK FORMAT (little-endian, version 2 since #408; the reader still loads 1)
  header  64 B   "DCVP", u32 version, u32 instrument (0 guitar, 1 bass),
                 u32 zoneCount, f32 sampleRate, u32 codec (v2: 1 = the ZONE
                 CODEC below; v1: reserved 0),
                 u64 zonesOffset, u64 dataOffset, u64 dataSamples (the DECODED
                 total), u64 textOffset, u32 textBytes, u32 reserved
  zones   40 B each: u8 articulation, u8 variant, u8 layer, u8 rr,
                 f32 rootHz, f32 gain, u32 length, u64 start (in DECODED
                 samples), u32 codedBytes, u8 string+1, u8 fret+1, 2 B reserved,
                 u64 codedOffset (into the data region)
                 (v1: codedBytes / codedOffset are reserved zeros, 24..27 was an
                 f32, and the data region is the raw int16 samples)
                 (string 0 = the LOWEST-pitched string, the chart's numbering;
                 string+1 / fret+1 = 0 = unknown. Recorded for EGFxSet, ESTIMATED
                 for the chromatic Karoryfer sets: the lowest string that plays
                 the pitch at fret <= 4. The voice uses them to prefer the
                 recording made on the charted string and to know where on the
                 string the recording's pickup sat — the pickup-position
                 simulation needs the vibrating length.)
  data    v2: each zone's samples CODED (below), zones back to back; v1: int16
          mono PCM. Either way each zone is int16 scaled to its own peak, float
          = s * gain, and the reader decodes v2 into exactly the v1 samples.
  text    UTF-8 credits (the same text as VOICE-CREDITS.txt)

  A version the reader does not know is REJECTED, so a DLL older than #408
  refuses a v2 pack ("unsupported pack version 2") instead of reading coded
  bytes as audio: the packs and the DLL ship together.

ZONE CODEC (pack version 2, codec 1; encode_zone here, dcvoice_codec.h reads it)
  LOSSLESS. One MSB-first bit stream per zone, zero-padded to a byte, that must
  end in its last byte. Per BLOCK of 4096 samples (the last one shorter):
    6 bits  LPC order (0..32)
    if order > 0: 4 bits shift, then `order` x 16-bit two's-complement coefs
    per PARTITION of 256 samples (the last one shorter):
      5 bits Rice parameter k (0..24), then per sample: (u >> k) zero bits, a
      1, and the low k bits of u, where u = 2e (e >= 0) or -2e - 1 and
      e = x[n] - ((sum_j coef[j] * x[n-1-j]) >> shift) — an int64 sum, an
      arithmetic (flooring) shift, x before the zone's first sample = 0, the
      history running on across blocks. u >> k < 65536 always.
  The encoder fits each block's predictor (Levinson on the integer
  autocorrelation, orders 0/1/2/4/8/12/16, the one with the least absolute
  residual) and each partition's k; every step is integer or plain-Python float
  arithmetic, so the bytes stay deterministic. Measured on the 2026-10-07 build:
  the 8 packs code to 36 % of their PCM (guitar.dcvp 51 %, the others 27-36 %).

Articulations: 0 sustain, 1 palm/staccato, 2 legato (hammer-on), 3 release
noise, 4 ghost, 5 dead (muted string), 6 slap thumb (unpitched, on muted
strings), 7 natural harmonic (rootHz = the harmonic's pitch, fret = the node it
was touched at), 8 pinch harmonic (rootHz = the fretted note's pitch), 9 slap
note (a recorded thumb slap of a pitched note; string / fret MEASURED). A DLL
older than #109 rejects code 9, so it never loads a #109 bass pack. Variant: 0 = default / fingers,
1 = pick.

LONG RECORDINGS (Guitar-TECHS: one WAV per technique, a note every 4 s) are
cut at refresh time: each manifest entry names the member, the note's onset
(`offset`, seconds, found in the audio) and its labels (string / fret from the
dataset's per-string MIDI, the natural harmonic's number from its measured
pitch). `build` only slices.
"""
import argparse
import hashlib
import io
import json
import math
import os
import struct
import sys
import time
import urllib.request
import wave
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "voice_pack_manifest.json")
FS = 44100

SUSTAIN, PALM, LEGATO, RELEASE, GHOST, DEAD, SLAP, NATHARM, PINCH, SLAPNOTE = range(10)
ART_NAMES = ["sustain", "palm", "legato", "release", "ghost", "dead", "slap", "natural-harmonic", "pinch-harmonic",
             "slap-note"]
# Longest kept per articulation, seconds. Sustains are what a let-ring hand
# shape plays out, so they keep their natural decay up to here.
MAX_SECONDS = {SUSTAIN: 5.0, PALM: 0.9, LEGATO: 4.0, RELEASE: 0.5, GHOST: 0.6, DEAD: 0.45, SLAP: 0.35,
               NATHARM: 3.5, PINCH: 3.0, SLAPNOTE: 2.5}

# --- the sources, pinned ------------------------------------------------------
# The Karoryfer ones are CC0 1.0 (Karoryfer Samples), verified 2026-10-04 at the
# LICENSE file of the pinned commit.
REPOS = {
    "bag": ("sfzinstruments/karoryfer.black-and-green-guitars", "b3b3249d37dc977a1a297bd2dc053e6d9b6b805c",
            "Black And Green Guitars (green: Gretsch Anniversary), by Brian Wood"),
    "shiny": ("sfzinstruments/karoryfer.shinyguitar", "57243cca85277dbcc120ce17c6178032f93c80f3",
              "Shinyguitar (archtop, magnetic pickup), by D. Smolken"),
    "babb": ("sfzinstruments/karoryfer.black-and-blue-basses", "6e7d674cdb41be7a54dbccb15472401ad01099b9",
             "Black And Blue Basses (darkblack)"),
    "growly": ("sfzinstruments/karoryfer.growlybass", "4f483268fc66b5a6d5781d421c0d11b8d08d3fc6",
               "Growlybass (Squier Jazz Bass)"),
    "swag": ("sfzinstruments/karoryfer.swagbass", "9d10fcae71af1975988ddecd5af1c95d372c7355",
             "Swagbass (Ibanez BTB, flatwounds), by D. Smolken"),
}

# EGFxSet (Pedroza, Meza, Roman 2022): CC BY 4.0, verified 2026-10-04 at the
# Zenodo record's metadata (license id "cc-by-4.0"). ONE pinned archive — the
# Clean.zip of record 7044411 (version 1.0): 690 five-second mono 24-bit 48 kHz
# DI notes of an American Fender Stratocaster, every string x frets 0..22 in
# each of the five pickup positions, plucked with a 2 mm pick into an Audient
# iD14 instrument input. Zenodo publishes the MD5; the SHA-256 is ours.
ARCHIVES = {
    "egfxset": {
        "url": "https://zenodo.org/api/records/7044411/files/Clean.zip/content",
        "md5": "cdb1b401960f56becc8640387910e78a",
        "sha256": "",   # filled by refresh
        "title": "EGFxSet - clean tones of an American Fender Stratocaster, five pickup positions, "
                 "by Hegel Pedroza, Gerardo Meza and Iran R. Roman",
        "doi": "10.5281/zenodo.7044411",
        "license": "CC-BY-4.0",
        "changes": "the 'Clean' notes only; trimmed, resampled to 44.1 kHz, converted to 16-bit, "
                   "level-normalised, one pack per pickup position",
    },
    # Guitar-TECHS (Pedroza, Taheri, Abreu, Corey, Roman, ICASSP 2025): CC BY 4.0,
    # verified 2026-10-04 at the Zenodo record's metadata (license id
    # "cc-by-4.0"). Player 1's TECHNIQUES archive: an Ibanez PF-300 Les Paul
    # type, 11-50 flatwound strings, BRIDGE humbucker throughout, DI at 48 kHz.
    # Player 1 and not 2: player 2 recorded on the NECK humbucker, 10-30 dB
    # darker than the Strat sets above 1 kHz (measured 2026-10-04), which no
    # pickup conversion can give back — a pinch harmonic lives up there.
    "guitartechs-p1": {
        "url": "https://zenodo.org/api/records/14963133/files/P1_techniques.zip/content",
        "md5": "18634a41a6db5a8de10d07eb3122a872",
        "sha256": "",   # filled by refresh
        "title": "Guitar-TECHS - an electric guitar dataset covering techniques, musical excerpts, chords and "
                 "scales using a diverse array of hardware, by Hegel Pedroza, Termeh Taheri, Wallace Abreu, "
                 "Ryan Corey and Iran R. Roman (player 1's techniques)",
        "doi": "10.5281/zenodo.14963133",
        "license": "CC-BY-4.0",
        "changes": "player 1's palm mutes, pinch harmonics and natural harmonics (direct input) only; cut "
                   "into single notes, trimmed, resampled to 44.1 kHz, converted to 16-bit, level-normalised",
    },
}
# Guitar-TECHS player 1: technique -> articulation. Player 2 is the held-out
# reference of tools/voice/realism_eval.py and must never be added here.
GT_TECHNIQUES = {"PalmMute": PALM, "PinchHarmonics": PINCH, "Harmonics": NATHARM}
# A natural harmonic's number -> the node fret it is charted at.
HARMONIC_NODE = {2: 12, 3: 7, 4: 5, 5: 4, 6: 3}
# EGFxSet folder -> pack name. String numbers in the file names are the
# guitarist's (1 = high E ... 6 = low E); the pack stores the chart's (0 = low E).
STRAT_POSITIONS = {
    "Neck": "strat-neck",
    "Middle-Neck": "strat-neck-middle",
    "Middle": "strat-middle",
    "Bridge-Middle": "strat-middle-bridge",
    "Bridge": "strat-bridge",
}
STRAT_PACKS = list(STRAT_POSITIONS.values())
STD_GUITAR = [40, 45, 50, 55, 59, 64]          # E2 A2 D3 G3 B3 E4 (MIDI)
BASS5 = [23, 28, 33, 38, 43]                    # B0 E1 A1 D2 G2 (darkblack)
BASS4 = [28, 33, 38, 43]                        # E1 A1 D2 G2 (growly)
# Each Strat pack also carries the GUITAR pack's unpitched noises, so a Strat
# model never falls silent on a dead note or loses its release noise.
STRAT_SHARED = ("RELEASE", "DEAD")

NOTE = {"c": 0, "db": 1, "d": 2, "eb": 3, "e": 4, "f": 5, "gb": 6, "g": 7, "ab": 8, "a": 9, "bb": 10, "b": 11}
NAMES = ["c", "db", "d", "eb", "e", "f", "gb", "g", "ab", "a", "bb", "b"]


def name_hz(name, octave_shift=0):
    # names are like "a3", "bb4", "db2"
    letters = name.rstrip("0123456789")
    octave = int(name[len(letters):])
    midi = 12 * (octave + 1 + octave_shift) + NOTE[letters]
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def selection(tree):
    """(repo_key, path) -> role. `tree` maps repo_key -> set of paths at the pin.
    Every Karoryfer name used here reads ONE OCTAVE HIGH (twang_a3 sounds A2), so
    every nominal pitch is shifted down an octave; the builder measures f0 anyway."""
    roles = []

    def add(repo, path, instrument, art, variant, layer, rr, hint):
        if path in tree[repo]:
            roles.append({"repo": repo, "path": path, "instrument": instrument, "art": art, "variant": variant,
                          "layer": layer, "rr": rr, "hint": round(hint, 3)})

    for p in tree["bag"]:
        f = p.split("/")[-1]
        if p.startswith("Samples/green/ord/twang_"):
            stem = f[len("twang_"):-4]                  # a3_mf_rr2
            note, layer, rr = stem.split("_")
            if layer in ("p", "mf", "f") and rr in ("rr1", "rr2"):
                add("bag", p, "guitar", SUSTAIN, 0, ["p", "mf", "f"].index(layer), int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("Samples/green/stac/staccato_"):
            note, rr = f[len("staccato_"):-4].split("_")
            if rr in ("rr1", "rr2"):
                add("bag", p, "guitar", PALM, 0, 0, int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("Samples/green/hammer/hammer_"):
            note, rr = f[len("hammer_"):-4].split("_")
            add("bag", p, "guitar", LEGATO, 0, 0, int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("Samples/green/rel/release_"):
            note, rr = f[len("release_"):-4].split("_")
            if rr == "rr1":
                add("bag", p, "guitar", RELEASE, 0, 0, 0, name_hz(note, -1))
    for p in tree["shiny"]:
        f = p.split("/")[-1]
        if p.startswith("Samples/electric/mute_string") and not f.startswith("mute_strings"):
            s = int(f[len("mute_string")])
            rr = int(f.split("_rr")[1].split("_")[0])
            if rr <= 3:
                # string 1 = high E ... 6 = low E (verified by measured pitch below)
                open_hz = [329.63, 246.94, 196.0, 146.83, 110.0, 82.41][s - 1]
                add("shiny", p, "guitar", DEAD, 0, 0, rr - 1, open_hz)
    for p in tree["babb"]:
        f = p.split("/")[-1]
        if p.startswith("Samples/darkblack/reg/darkblack_"):
            note, layer, rr = f[len("darkblack_"):-4].split("_")
            if layer in ("mf", "f") and rr in ("rr1", "rr2"):
                add("babb", p, "bass", SUSTAIN, 0, ["mf", "f"].index(layer), int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("Samples/darkblack/ghost/darkblack_"):
            parts = f[len("darkblack_"):-4].split("_")  # a2 ghost f rr1
            if parts[2] == "f" and parts[3] in ("rr1", "rr2"):
                add("babb", p, "bass", GHOST, 0, 0, int(parts[3][2:]) - 1, name_hz(parts[0], -1))
        elif p.startswith("Samples/darkblack/stac/darkblack_"):
            note, _, rr = f[len("darkblack_"):-4].split("_")
            if rr in ("rr1", "rr2"):
                add("babb", p, "bass", PALM, 0, 0, int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("Samples/darkblack/rel/darkblack_"):
            note, _, rr = f[len("darkblack_"):-4].split("_")
            if rr == "rr1":
                add("babb", p, "bass", RELEASE, 0, 0, 0, name_hz(note, -1))
    for p in tree["growly"]:
        f = p.split("/")[-1]
        if p.startswith("sustain/"):
            note, layer, rr = f[:-4].split("_")
            if layer in ("f", "ff") and rr in ("rr1", "rr2"):
                add("growly", p, "bass", SUSTAIN, 1, ["f", "ff"].index(layer), int(rr[2:]) - 1, name_hz(note, -1))
        elif p.startswith("staccato/"):
            note, _, rr = f[:-4].split("_")
            if rr in ("rr1", "rr2"):
                add("growly", p, "bass", PALM, 1, 0, int(rr[2:]) - 1, name_hz(note, -1))
    for p in tree["swag"]:
        f = p.split("/")[-1]
        if p.startswith("noises/noise_mutedslap_rr"):
            add("swag", p, "bass", SLAP, 0, 0, int(f[len("noise_mutedslap_rr"):-4]) - 1, 100.0)
    for p in tree.get("egfxset", ()):
        parts = p.split("/")
        if len(parts) != 3 or parts[0] != "Clean" or parts[1] not in STRAT_POSITIONS or not parts[2].endswith(".wav"):
            continue
        s, k = (int(v) for v in parts[2][:-4].split("-"))
        string = 6 - s                               # 0 = low E
        hint = 440.0 * 2.0 ** ((STD_GUITAR[string] + k - 69) / 12.0)
        roles.append({"repo": "egfxset", "path": p, "instrument": STRAT_POSITIONS[parts[1]], "art": SUSTAIN,
                      "variant": 0, "layer": 0, "rr": 0, "hint": round(hint, 3), "string": string, "fret": k})
    roles.sort(key=lambda r: (r["instrument"], r["art"], r["variant"], r["hint"], r.get("string", -1), r["layer"],
                              r["rr"], r["path"]))
    return roles


def estimate_string_fret(hz, tuning):
    """(string, fret) a chromatic sample set most likely played `hz` on: the
    LOWEST string that reaches it at fret <= 4, else the top string. An
    estimate — the Karoryfer sets do not say."""
    midi = int(round(69 + 12 * math.log2(hz / 440.0)))
    for s, open_midi in enumerate(tuning):
        if 0 <= midi - open_midi <= 4:
            return s, midi - open_midi
    if midi < tuning[0]:
        return 0, 0
    return len(tuning) - 1, max(0, midi - tuning[-1])


def tuning_of(f):
    if f["instrument"] == "guitar":
        return STD_GUITAR
    if f["instrument"] == "bass":
        return BASS4 if f["repo"] == "growly" else BASS5
    return STD_GUITAR


# --- downloading ----------------------------------------------------------------

def url_of(repo_key, path):
    if repo_key in ARCHIVES:
        return ARCHIVES[repo_key]["url"] + "#" + path
    repo, sha, _ = REPOS[repo_key]
    return f"https://raw.githubusercontent.com/{repo}/{sha}/{urllib.request.quote(path)}"


def fetch(url, cache, expect_sha=None, suffix=".wav", expect_md5=None):
    name = hashlib.sha1(url.encode()).hexdigest() + suffix
    dest = os.path.join(cache, name)
    if os.path.exists(dest):
        data = open(dest, "rb").read()
        if expect_sha is None or hashlib.sha256(data).hexdigest() == expect_sha:
            return data
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=600) as r:
                data = r.read()
            break
        except Exception as e:  # noqa: BLE001 - network: retry, then fail loudly
            if attempt == 3:
                raise RuntimeError(f"download failed: {url}: {e}")
            time.sleep(2 * (attempt + 1))
    digest = hashlib.sha256(data).hexdigest()
    if expect_sha is not None and digest != expect_sha:
        raise RuntimeError(f"SHA-256 mismatch for {url}: got {digest}, manifest says {expect_sha}")
    if expect_md5 is not None and hashlib.md5(data).hexdigest() != expect_md5:
        raise RuntimeError(f"MD5 mismatch for {url}: the publisher says {expect_md5}")
    tmp = dest + ".partial"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)
    return data


_ZIPS = {}


def archive(key, cache, pinned_sha=None):
    """The pinned archive, opened once (cached on disk by URL, verified by the
    publisher's MD5 and, once the manifest pins it, by SHA-256)."""
    if key not in _ZIPS:
        a = ARCHIVES[key]
        data = fetch(a["url"], cache, pinned_sha or None, suffix=".zip", expect_md5=a["md5"])
        _ZIPS[key] = (zipfile.ZipFile(io.BytesIO(data)), hashlib.sha256(data).hexdigest())
    return _ZIPS[key]


def fetch_member(f, cache, manifest=None):
    """A manifest file's bytes: a raw GitHub file, or a member of a pinned archive."""
    if f["repo"] in ARCHIVES:
        pinned = (manifest or {}).get("archives", {}).get(f["repo"], {}).get("sha256")
        z, _ = archive(f["repo"], cache, pinned)
        data = z.read(f["path"])
        if f.get("sha256") and hashlib.sha256(data).hexdigest() != f["sha256"]:
            raise RuntimeError(f"SHA-256 mismatch for {f['repo']}:{f['path']}")
        return data
    return fetch(f["url"], cache, f.get("sha256"))


_LONG = {}


def note_of(f, cache, manifest):
    """One note cut out of a long recording: `offset` s into the member, at
    most MAX_SECONDS of its articulation plus a little, resampled to FS."""
    key = (f["repo"], f["path"])
    if key not in _LONG:
        _LONG.clear()   # one long member in memory at a time (files are sorted by art)
        _LONG[key] = decode_raw(fetch_member(f, cache, manifest))
    x, rate = _LONG[key]
    a = int(round(f["offset"] * rate))
    seg = x[a:a + int(f.get("length", MAX_SECONDS[f["art"]] + 0.05) * rate)]
    if rate == FS:
        return seg.copy()
    if rate == 48000:
        return resample_48k(seg)
    raise RuntimeError(f"unexpected rate {rate}")


def list_tree(repo_key):
    if repo_key in ARCHIVES:
        return None   # resolved from the archive itself in refresh()
    repo, sha, _ = REPOS[repo_key]
    url = f"https://api.github.com/repos/{repo}/git/trees/{sha}?recursive=1"
    with urllib.request.urlopen(urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"}),
                                timeout=60) as r:
        tree = json.load(r)
    return {t["path"] for t in tree["tree"] if t["type"] == "blob"}


# --- audio -----------------------------------------------------------------------

def decode_wav(data):
    with wave.open(io.BytesIO(data)) as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x).astype(np.float64) / (1 << 23)
    elif width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise RuntimeError(f"unsupported sample width {width}")
    x = x.reshape(-1, ch).mean(axis=1)
    if rate == FS:
        return x
    if rate == 48000:
        return resample_48k(x)
    raise RuntimeError(f"unexpected rate {rate}")


def resample_48k(x):
    """48 kHz -> 44.1 kHz, band-limited by the FFT (numpy only, deterministic):
    zero-padded to twice its length so the note's tail cannot wrap into its
    attack, the spectrum cut at the new Nyquist with a 1 kHz raised-cosine
    roll-off ending AT it (no alias, no Gibbs ringing), and cropped back."""
    n = len(x)
    pad = 1 << int(math.ceil(math.log2(2 * n)))
    spec = np.fft.rfft(x, pad)
    m = pad * FS // 48000
    keep = m // 2 + 1
    f = np.arange(keep) * (48000.0 / pad)
    lo, hi = FS / 2 - 1000.0, FS / 2
    w = np.where(f <= lo, 1.0, np.where(f >= hi, 0.0, 0.5 + 0.5 * np.cos(np.pi * (f - lo) / (hi - lo))))
    y = np.fft.irfft(spec[:keep] * w, m) * (m / pad)
    return y[:int(n * FS / 48000)]


def measure_f0(x, hint, seconds):
    """Pitch by HARMONIC SUM: every candidate within +-60 cents of the name is
    scored by the summed (log) magnitude of its first six partials — one weak or
    beating fundamental cannot pull it away, which a single-peak search on a
    low, short bass note does. Falls back to the name when the evidence is flat."""
    a = int(0.03 * FS)
    seg = x[a:a + int(seconds * FS)]
    if len(seg) < 2048:
        return hint
    nfft = 1 << 21
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
    df = FS / nfft
    logm = np.log(mag + 1e-12)
    cands = hint * 2.0 ** (np.linspace(-60, 60, 241) / 1200.0)
    scores = np.zeros(len(cands))
    for n in range(1, 7):
        idx = cands * n / df
        ok = idx < len(mag) - 2
        i0 = np.floor(idx).astype(np.int64)
        fr = idx - i0
        i0c = np.clip(i0, 0, len(mag) - 2)
        v = logm[i0c] * (1 - fr) + logm[i0c + 1] * fr
        scores += np.where(ok, v, 0.0) / n
    best = int(np.argmax(scores))
    if scores[best] - np.median(scores) < 0.5:
        return hint
    return float(cands[best])


def trim(x, art):
    peak = float(np.max(np.abs(x))) + 1e-12
    on = int(np.argmax(np.abs(x) > 0.02 * peak))
    on = max(0, on - int(0.001 * FS))
    y = x[on:on + int(MAX_SECONDS[art] * FS)].copy()
    hop = int(0.01 * FS)
    env = np.array([np.sqrt(np.mean(y[i:i + hop] ** 2)) for i in range(0, max(len(y) - hop, 1), hop)])
    thr = env.max() * 10 ** (-60 / 20)
    last = len(env) - 1
    while last > 0 and env[last] < thr:
        last -= 1
    n = min(len(y), (last + 2) * hop)
    y = y[:n]
    fade = min(int(0.15 * FS), max(int(0.3 * n), 16))
    y[n - fade:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    rise = int(0.001 * FS)
    y[:rise] *= np.linspace(0.0, 1.0, rise)
    return y


# --- commands ----------------------------------------------------------------------

def _midi_notes(data):
    """Minimal Standard MIDI File reader: [(track, start_s, pitch, velocity)],
    honouring the tempo map. Enough for the dataset's per-string annotations."""
    def vlq(b, i):
        v = 0
        while True:
            c = b[i]
            i += 1
            v = (v << 7) | (c & 0x7F)
            if c < 0x80:
                return v, i
    b = data
    ntr, div = struct.unpack(">HH", b[10:14])
    i = 8 + struct.unpack(">I", b[4:8])[0]
    tempo, tracks = [], []
    for _ in range(ntr):
        ln = struct.unpack(">I", b[i + 4:i + 8])[0]
        j, end, i = i + 8, i + 8 + ln, i + 8 + ln
        tick, run, ev = 0, 0, []
        while j < end:
            d, j = vlq(b, j)
            tick += d
            st = b[j]
            if st == 0xFF:
                typ = b[j + 1]
                ln2, j2 = vlq(b, j + 2)
                if typ == 0x51:
                    tempo.append((tick, int.from_bytes(b[j2:j2 + ln2], "big")))
                j = j2 + ln2
                continue
            if st in (0xF0, 0xF7):
                ln2, j2 = vlq(b, j + 1)
                j = j2 + ln2
                continue
            if st & 0x80:
                run = st
                j += 1
            kind = run & 0xF0
            if kind in (0xC0, 0xD0):
                j += 1
                continue
            d1, d2 = b[j], b[j + 1]
            j += 2
            if kind == 0x90 and d2 > 0:
                ev.append((tick, d1, d2))
        tracks.append(ev)
    tempo.sort()
    if not tempo or tempo[0][0] != 0:
        tempo.insert(0, (0, 500000))

    def sec(tick):
        s_, pt, pu = 0.0, 0, tempo[0][1]
        for tt, us in tempo[1:]:
            if tt >= tick:
                break
            s_ += (tt - pt) * pu / 1e6 / div
            pt, pu = tt, us
        return s_ + (tick - pt) * pu / 1e6 / div
    return sorted((ti, sec(t), p, v) for ti, ev in enumerate(tracks) for t, p, v in ev)


def decode_raw(data):
    """WAV bytes -> (mono float64, rate), no resampling (long recordings are
    sliced first)."""
    with wave.open(io.BytesIO(data)) as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    elif width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    else:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x).astype(np.float64) / (1 << 23)
    return x.reshape(-1, ch)[:, 0], rate


def slot_onset(x, rate, t0, t1, differentiate=True):
    """The attack inside [t0, t1) s: the 1 ms frame with the steepest 10 ms rise
    of the (by default differentiated: a crude high-pass, so the previous note
    ringing into the slot does not fool it) signal's level, walked back to where
    it began. A natural harmonic has too little top for the derivative: its
    slots use the plain level."""
    a, b = int(t0 * rate), int(t1 * rate)
    seg = np.diff(x[max(a - 1, 0):b]) if differentiate else x[a:b]
    hop = rate // 1000
    n = len(seg) // hop
    e = 10 * np.log10(np.mean(seg[:n * hop].reshape(n, hop) ** 2, axis=1) + 1e-20)
    pk = e.max()
    rise = np.full(n, -1e9)
    for i in range(n - 10):
        if e[i + 10] > pk - 24.0:
            rise[i] = e[i + 10] - e[i]
    i = int(np.argmax(rise))
    j = i
    while j > 0 and e[j - 1] <= e[j] + 0.5 and e[j - 1] > e[i] - 3.0 and i - j < 20:
        j -= 1
    while j < i + 10 and e[j + 1] - e[j] < 1.0:
        j += 1
    return (a + j * hop) / rate, float(pk)


def guitartechs_roles(z, player="P1", harmonic_slots=4):
    """One player's techniques -> one role per note (see the module docstring).
    The natural harmonics are `harmonic_slots` slots per string, low E first."""
    roles = []
    open_hz = [440.0 * 2.0 ** ((m - 69) / 12.0) for m in STD_GUITAR]
    for tech, art in GT_TECHNIQUES.items():
        audio = f"{player}_techniques/audio/directinput/directinput_{tech}.wav"
        midi = _midi_notes(z.read(f"{player}_techniques/midi/midi_{tech}.mid"))
        x, rate = decode_raw(z.read(audio))
        slots = len(x) // (4 * rate)
        by_slot = {}
        for tr, t, p, v in midi:
            by_slot.setdefault(int((t + 0.25) // 4), []).append((t, tr, p, v))
        for i in range(slots):
            # The harmonics were played freely (a note can land a second into
            # its slot): their window is longer.
            onset, _ = slot_onset(x, rate, max(0.0, 4 * i - 0.05), 4 * i + (2.0 if art == NATHARM else 0.8),
                                  differentiate=art != NATHARM)
            if np.max(np.abs(x[int(onset * rate):int((onset + 0.5) * rate)])) < 0.01:
                continue   # an empty slot
            if art == NATHARM:
                # A few slots per string, low E first; the harmonic's number from
                # its pitch (the MIDI pickup misses some of them).
                string = i // harmonic_slots
                if string > 5:
                    continue
                a = int((onset + 0.15) * rate)
                seg = x[a:a + int(0.6 * rate)]
                spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 18))
                f = np.fft.rfftfreq(1 << 18, 1.0 / rate)
                # The strongest partial is a whole multiple r of the open string;
                # k is the largest node number dividing r whose own partial is
                # within 15 dB of it (a 5th-fret harmonic often peaks on 8 x open).
                band = f > 0.8 * open_hz[string]
                pk = float(f[np.argmax(spec * band)])
                r = pk / open_hz[string]
                if abs(r - round(r)) > 0.06:
                    continue   # not a natural harmonic of this string
                r = int(round(r))
                top = spec.max()

                def mag(c):
                    m = (f > c * 0.98) & (f < c * 1.02)
                    return float(spec[m].max()) if np.any(m) else 0.0
                ks = [k for k in sorted(HARMONIC_NODE, reverse=True)
                      if r % k == 0 and mag(k * open_hz[string]) > top * 10 ** (-15 / 20)]
                if not ks:
                    continue
                best_k = ks[0]
                hint, fret = best_k * open_hz[string], HARMONIC_NODE[best_k]
            else:
                ev = sorted(e for e in by_slot.get(i, []) if 1 <= e[1] <= 6 and e[3] > 5)
                if not ev:
                    continue
                t, tr, p, v = ev[0]
                string = 6 - tr
                fret = p - STD_GUITAR[string]
                if not 0 <= fret <= 22:
                    continue
                hint = 440.0 * 2.0 ** ((p - 69) / 12.0)
            roles.append({"repo": "guitartechs-" + player.lower(), "path": audio, "instrument": "guitar-arts", "art": art,
                          "variant": 0, "layer": 0, "rr": 0, "hint": round(hint, 3), "string": string,
                          "fret": fret, "offset": round(onset - 0.002, 4),
                          # never past the next slot's start (its note)
                          "length": round(min(MAX_SECONDS[art] + 0.05, 4 * (i + 1) - 0.03 - onset), 4)})
    # Two takes of one (art, string, fret): round robins.
    seen = {}
    for r in roles:
        key = (r["art"], r["string"], r["fret"])
        r["rr"] = seen.get(key, 0)
        seen[key] = r["rr"] + 1
    return roles


def refresh(args):
    if args.only_source:
        # Re-resolve ONE source and keep every other manifest entry as it is
        # (so adding a source does not need every other download).
        manifest = json.load(open(MANIFEST, encoding="utf-8"))
        key = args.only_source
        if key in REPOS:
            refresh_repo(manifest, key, args.cache)
            return
        z, digest = archive(key, args.cache)
        roles = guitartechs_roles(z) if key == "guitartechs-p1" else []
        # Also drops entries of a source no longer in REPOS / ARCHIVES.
        files = [f for f in manifest["files"] if f["repo"] != key and (f["repo"] in REPOS or f["repo"] in ARCHIVES)]
        for k in list(manifest.get("archives", {})):
            if k not in ARCHIVES:
                del manifest["archives"][k]
        member_sha = {}
        for r in roles:
            if r["path"] not in member_sha:
                data = z.read(r["path"])
                member_sha[r["path"]] = (hashlib.sha256(data).hexdigest(), len(data))
            sha, size = member_sha[r["path"]]
            files.append(dict(r, url=url_of(key, r["path"]), sha256=sha, bytes=size))
        files.sort(key=lambda r: (r["instrument"], r["art"], r["variant"], r["hint"], r.get("string", -1), r["layer"],
                                  r["rr"], r["path"], r.get("offset", 0.0)))
        manifest["archives"][key] = dict(ARCHIVES[key], sha256=digest)
        manifest["files"] = files
        manifest["comment"] = MANIFEST_COMMENT
        with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"manifest: {len(roles)} {key} notes; {len(files)} files in all")
        return
    tree = {k: list_tree(k) for k in REPOS}
    archives = {}
    extra = []
    for key in ARCHIVES:
        z, digest = archive(key, args.cache)
        tree[key] = {n for n in z.namelist() if not n.endswith("/")}
        archives[key] = dict(ARCHIVES[key], sha256=digest)
        if key == "guitartechs-p1":
            extra = guitartechs_roles(z)
    roles = selection(tree) + extra
    files = []
    for i, r in enumerate(roles):
        url = url_of(r["repo"], r["path"])
        data = fetch_member(dict(r, url=url), args.cache)
        files.append(dict(r, url=url, sha256=hashlib.sha256(data).hexdigest(), bytes=len(data)))
        if i % 50 == 0:
            print(f"  {i}/{len(roles)}", flush=True)
    manifest = {
        "format": 2,
        "comment": MANIFEST_COMMENT,
        "repos": {k: repo_entry(k) for k in REPOS},
        "archives": archives,
        "files": files,
    }
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"manifest: {len(files)} files, {sum(f['bytes'] for f in files) / 1e6:.1f} MB of sources")


MANIFEST_COMMENT = ("Generated by build_voice_pack.py refresh. Karoryfer sources are CC0 1.0; EGFxSet and "
                    "Guitar-TECHS are CC BY 4.0 (attribution in VOICE-CREDITS.txt).")


def repo_entry(key):
    repo, sha, title = REPOS[key]
    entry = {"repo": repo, "commit": sha, "title": title, "license": "CC0-1.0"}
    return entry


def refresh_repo(manifest, key, cache):
    """Re-resolve ONE repository (its tree at the pin, the selection, every
    file's SHA-256) and keep every other manifest entry as it is."""
    tree = {k: set() for k in REPOS}
    tree[key] = list_tree(key)
    roles = [r for r in selection(tree) if r["repo"] == key]
    files = [f for f in manifest["files"] if f["repo"] != key]
    for i, r in enumerate(roles):
        url = url_of(key, r["path"])
        data = fetch(url, cache)
        files.append(dict(r, url=url, sha256=hashlib.sha256(data).hexdigest(), bytes=len(data)))
        if i % 50 == 0:
            print(f"  {i}/{len(roles)}", flush=True)
    files.sort(key=lambda r: (r["instrument"], r["art"], r["variant"], r["hint"], r.get("string", -1), r["layer"],
                              r["rr"], r["path"], r.get("offset", 0.0)))
    manifest["repos"][key] = repo_entry(key)
    manifest["files"] = files
    manifest["comment"] = MANIFEST_COMMENT
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"manifest: {len(roles)} {key} files; {len(files)} files in all")


def credits_text(manifest):
    lines = [
        "DotCharter built-in guitar and bass voice - sample credits",
        "",
        "The voice plays short DI recordings. Trimmed, converted to mono 16-bit",
        "44.1 kHz (EGFxSet: resampled from 48 kHz), level-normalised and packed by",
        "JuceSharp/src/JuceSharp.Native/tools/voice/build_voice_pack.py.",
        "",
        "Karoryfer Samples (Karoryfer Lecolds) - released under CC0 1.0 Universal",
        "(public domain dedication; no attribution is required - credited with thanks):",
        "",
    ]
    for k, v in sorted(manifest["repos"].items()):
        if v["license"] == "CC0-1.0":
            lines.append(f"* {v['title']} - https://github.com/{v['repo']} @ {v['commit'][:12]} - {v['license']}")
    for k, v in sorted(manifest.get("archives", {}).items()):
        lines += [
            "",
            f"{v['title']}.",
            f"https://doi.org/{v['doi']} - licensed under Creative Commons Attribution 4.0",
            "International (CC BY 4.0), https://creativecommons.org/licenses/by/4.0/ .",
            f"Changes: {v.get('changes', 'trimmed, converted, level-normalised')}. The voice",
            "also FILTERS them to simulate other pickups and guitars (Telecaster, Les",
            "Paul, the HSS bridge humbucker); those sounds are derived from, and not",
            "recordings of, those instruments.",
        ]
    lines.append("")
    return "\n".join(lines)


# --- the lossless sample codec (pack version 2, #408) -------------------------
# The bit stream of one zone is documented under "ZONE CODEC" at the top of this
# file; src/dsp/voice/dcvoice_codec.h decodes it. Everything here is integer or
# plain-Python float arithmetic, so the same zone always encodes to the same
# bytes (the build stays deterministic).
PACK_VERSION, PACK_CODEC = 2, 1
CODEC_BLOCK, CODEC_PART, CODEC_MAX_ORDER, CODEC_MAX_K = 4096, 256, 32, 24
CODEC_ORDERS = (0, 1, 2, 4, 8, 12, 16)
CODEC_COEF_BITS = 14


def _levinson(r, orders):
    """Levinson-Durbin on the autocorrelation `r` (Python floats): the
    prediction coefficients of every order in `orders` (x[n] ~ sum c[j] x[n-1-j])."""
    top = max(orders)
    a = [1.0] + [0.0] * top
    e = r[0]
    out = {}
    for i in range(1, top + 1):
        if not e > 0.0:
            break
        acc = r[i]
        for j in range(1, i):
            acc += a[j] * r[i - j]
        k = -acc / e
        prev = a[:]
        for j in range(1, i):
            a[j] = prev[j] + k * prev[i - j]
        a[i] = k
        e *= 1.0 - k * k
        if i in orders:
            out[i] = [-v for v in a[1:i + 1]]
    return out


def _quantise(c):
    """Coefficients -> (int16 list, shift) with sum(q x) >> shift ~ sum(c x)."""
    m = max(abs(v) for v in c)
    if not (m > 0.0 and math.isfinite(m)):
        return None
    shift = max(0, min(15, CODEC_COEF_BITS - math.ceil(math.log2(m))))
    return [max(-32768, min(32767, int(round(v * (1 << shift))))) for v in c], shift


def _residual(xpad, s, n, q, shift):
    y = xpad[CODEC_MAX_ORDER + s: CODEC_MAX_ORDER + s + n]
    if not q:
        return y.copy()
    acc = np.zeros(n, np.int64)
    for j, c in enumerate(q):
        base = CODEC_MAX_ORDER + s - 1 - j
        acc += c * xpad[base: base + n]
    return y - (acc >> shift)


def _rice_fields(u):
    """One block's residuals (zig-zagged) -> (values, bit counts) of its
    partitions: 5-bit k, then each code as the integer (1 << k) | low k bits
    written in (u >> k) + 1 + k bits — that IS q zeros, a 1 and the k bits."""
    vals, bits = [], []
    for p in range(0, len(u), CODEC_PART):
        part = u[p:p + CODEC_PART]
        kmin = max(0, int(part.max()).bit_length() - 16)   # keeps u >> k below 65536
        mean = float(part.mean())
        guess = int(math.floor(math.log2(mean))) if mean >= 1.0 else 0
        best = None
        for k in sorted({min(CODEC_MAX_K, max(kmin, guess + d)) for d in (-1, 0, 1)}):
            cost = int(np.sum(part >> k)) + (k + 1) * len(part)
            if best is None or cost < best[1]:
                best = (k, cost)
        k = best[0]
        vals.append(np.array([k], np.uint64))
        bits.append(np.array([5], np.int64))
        vals.append(((part & ((1 << k) - 1)) | (1 << k)).astype(np.uint64))
        bits.append((part >> k) + 1 + k)
    return vals, bits


def encode_zone(q16):
    """int16 samples -> the zone's coded bytes (see ZONE CODEC)."""
    x = q16.astype(np.int64)
    xpad = np.concatenate([np.zeros(CODEC_MAX_ORDER, np.int64), x])
    vals, bits = [], []
    for s in range(0, len(x), CODEC_BLOCK):
        n = min(CODEC_BLOCK, len(x) - s)
        seg = xpad[CODEC_MAX_ORDER + s - max(CODEC_ORDERS): CODEC_MAX_ORDER + s + n]
        r = [float(int(np.dot(seg[:len(seg) - i], seg[i:]))) for i in range(max(CODEC_ORDERS) + 1)]
        coefs = _levinson(r, set(CODEC_ORDERS) - {0}) if r[0] > 0.0 else {}
        best = None
        for order in CODEC_ORDERS:
            if order == 0:
                qs = ([], 0)
            elif order in coefs:
                qs = _quantise(coefs[order])
                if qs is None:
                    continue
            else:
                continue
            e = _residual(xpad, s, n, qs[0], qs[1])
            if int(np.abs(e).max()) >= 1 << 38:
                continue
            l1 = int(np.abs(e).sum())
            if best is None or l1 < best[0]:
                best = (l1, qs, e)
        _, (q, shift), e = best
        vals.append(np.array([len(q)], np.uint64))
        bits.append(np.array([6], np.int64))
        if q:
            vals.append(np.array([shift] + [c & 0xFFFF for c in q], np.uint64))
            bits.append(np.array([4] + [16] * len(q), np.int64))
        u = np.where(e >= 0, 2 * e, -2 * e - 1)
        v, b = _rice_fields(u)
        vals += v
        bits += b
    return pack_bits(np.concatenate(vals), np.concatenate(bits))


def pack_bits(vals, nbits):
    """Writes each value in its bit count, MSB first, one stream padded to a byte."""
    ends = np.cumsum(nbits)
    stream = np.zeros(int(ends[-1]) if len(ends) else 0, np.uint8)
    for j in range(int(vals.max()).bit_length() if len(vals) else 0):
        on = ((vals >> np.uint64(j)) & np.uint64(1)).astype(bool)
        stream[ends[on] - 1 - j] = 1
    return np.packbits(stream).tobytes()


def codec_golden_signal(n=5000):
    """The codec's CROSS-LANGUAGE test signal, integer arithmetic only (so C++
    regenerates it bit for bit): tests/dsp/sample_voice_test.cpp
    (codecGoldenSignal) decodes `codec-golden`'s bytes and must get exactly this.
    Leaky-integrated LCG noise, a saw over the first half, a full-scale square
    at 4000..4099 (int16's both ends)."""
    out, s, acc = [], 12345, 0
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        acc = acc - (acc >> 5) + ((s >> 16) & 2047) - 1024
        v = acc + (((i * 37) & 1023) * 16 - 8192 if i < 2500 else 0)
        if 4000 <= i < 4100:
            v = 32767 if (i // 7) % 2 else -32768
        out.append(max(-32768, min(32767, v)))
    return np.array(out, np.int16)


def codec_golden(args):
    blob = encode_zone(codec_golden_signal())
    h = blob.hex()
    print(f"// {len(blob)} bytes, block orders {[blob[0] >> 2]} (first block)")
    for i in range(0, len(h), 96):
        print(f'    "{h[i:i + 96]}"')


PACKS = [("guitar", 0)] + [(name, 0) for name in STRAT_PACKS] + [("guitar-arts", 0), ("bass", 1)]


def pack_files(manifest, instrument):
    files = [f for f in manifest["files"] if f["instrument"] == instrument]
    if instrument in STRAT_PACKS:
        shared = {globals()[a] for a in STRAT_SHARED}
        files += [f for f in manifest["files"] if f["instrument"] == "guitar" and f["art"] in shared]
    return files


def build(args):
    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    os.makedirs(args.out, exist_ok=True)
    text = credits_text(manifest)
    only = set(args.only.split(",")) if args.only else None
    for instrument, code in PACKS:
        if only is not None and instrument not in only:
            continue
        files = pack_files(manifest, instrument)
        zones = []
        for i, f in enumerate(files):
            if "offset" in f:
                x = note_of(f, args.cache, manifest)
            else:
                x = decode_wav(fetch_member(f, args.cache, manifest))
            art = f["art"]
            pitched = art in (SUSTAIN, PALM, LEGATO, GHOST, PINCH, NATHARM, SLAPNOTE)
            seconds = 1.0 if art in (SUSTAIN, LEGATO, SLAPNOTE) else 0.3
            root = measure_f0(x, f["hint"], seconds) if pitched else f["hint"]
            if art == DEAD:
                root = f["hint"]
            y = trim(x, art)
            if "string" in f:
                string, fret = f["string"], f["fret"]
            else:
                string, fret = estimate_string_fret(root if pitched or art == DEAD else f["hint"], tuning_of(f))
            zones.append({"f": f, "art": art, "root": root, "y": y, "level": float(np.max(np.abs(y))),
                          "string": string, "fret": fret})
            if i % 100 == 0:
                print(f"  {instrument}: {i}/{len(files)}", flush=True)
        # Ghost notes are barely pitched: they take the pitch measured on the same
        # note's sustain recording. A short staccato reading far off its name is
        # the attack's pitch, not the note's: trust the name.
        sustain_root = {}
        for z in zones:
            if z["art"] == SUSTAIN and z["f"]["variant"] == 0:
                sustain_root.setdefault(round(z["f"]["hint"], 1), []).append(z["root"])
        for z in zones:
            if z["art"] == GHOST:
                roots = sustain_root.get(round(z["f"]["hint"], 1))
                z["root"] = float(np.median(roots)) if roots else z["f"]["hint"]
            elif z["art"] in (PALM, PINCH, NATHARM) and abs(1200 * math.log2(z["root"] / z["f"]["hint"])) > 30:
                z["root"] = z["f"]["hint"]
        # Level: per (art, variant), every pitch's loudest layer -> the same
        # median level (0.5); see below for the softer layers.
        groups = {}
        for z in zones:
            groups.setdefault((z["art"], z["f"]["variant"], round(math.log2(z["root"]) * 24)), []).append(z)
        # The recorded layers can lie 20+ dB apart (guitar p / mf / f); a DI that
        # drives an amp wants the TIMBRE of a soft pick more than its level, so
        # the relation is kept but compressed to the 0.35 power.
        for zs in groups.values():
            top = max(z["f"]["layer"] for z in zs)
            ref = max(float(np.median([z["level"] for z in zs if z["f"]["layer"] == top])), 1e-9)
            for layer in set(z["f"]["layer"] for z in zs):
                med = max(float(np.median([z["level"] for z in zs if z["f"]["layer"] == layer])), 1e-9)
                target = 0.5 * (min(med / ref, 1.0)) ** 0.35
                for z in zs:
                    if z["f"]["layer"] == layer:
                        z["norm"] = target / med
        blobs, records, start, coded = [], [], 0, 0
        for z in zones:
            y = z["y"] * z["norm"]
            peak = float(np.max(np.abs(y))) + 1e-12
            q = np.clip(np.round(y / peak * 32767.0), -32767, 32767).astype("<i2")
            gain = peak / 32767.0
            blob = encode_zone(q)
            records.append(struct.pack("<BBBBffIQIBBxxQ", z["art"], z["f"]["variant"], z["f"]["layer"], z["f"]["rr"],
                                       float(z["root"]), float(gain), len(q), start, len(blob),
                                       z["string"] + 1, z["fret"] + 1, coded))
            blobs.append(blob)
            start += len(q)
            coded += len(blob)
        zones_off = 64
        data_off = zones_off + 40 * len(records)
        data_off = (data_off + 15) & ~15
        text_bytes = text.encode("utf-8")
        text_off = data_off + coded
        header = struct.pack("<4sIIIfIQQQQII", b"DCVP", PACK_VERSION, code, len(records), float(FS), PACK_CODEC,
                             zones_off, data_off, start, text_off, len(text_bytes), 0)
        assert len(header) == 64
        path = os.path.join(args.out, f"{instrument}.dcvp")
        with open(path + ".partial", "wb") as out:
            out.write(header)
            for r in records:
                out.write(r)
            out.write(b"\0" * (data_off - zones_off - 40 * len(records)))
            for b in blobs:
                out.write(b)
            out.write(text_bytes)
        os.replace(path + ".partial", path)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        counts = {}
        for z in zones:
            counts[ART_NAMES[z["art"]]] = counts.get(ART_NAMES[z["art"]], 0) + 1
        print(f"{instrument}.dcvp: {len(records)} zones {counts}, {2 * start / 1e6:.1f} MB of audio "
              f"coded in {coded / 1e6:.1f} MB ({100.0 * coded / max(1, 2 * start):.1f} %), "
              f"{os.path.getsize(path) / 1e6:.1f} MB on disk, sha256 {digest[:16]}")
    with open(os.path.join(args.out, "VOICE-CREDITS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    if only is None:
        # What these packs were built FROM: a release (New-Release.ps1) rebuilds
        # them when the committed manifest or this tool changed since.
        with open(os.path.join(args.out, STAMP), "w", encoding="utf-8", newline="\n") as f:
            f.write(source_stamp())


STAMP = "voice-pack.stamp"


def source_stamp():
    """`manifest=<sha256>` and `builder=<sha256>` (lower-case hex) of the exact
    bytes of the committed manifest and of this file, one per line."""
    def sha(path):
        return hashlib.sha256(open(path, "rb").read()).hexdigest()
    return f"manifest={sha(MANIFEST)}\nbuilder={sha(os.path.abspath(__file__))}\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["build", "refresh", "codec-golden"])
    ap.add_argument("--out", help="output directory for build")
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "dotcharter-voice-sources"))
    ap.add_argument("--only", help="comma-separated pack names to build (default: all)")
    ap.add_argument("--only-source",
                    help="refresh: re-resolve this one source (an archive or a repository key), keep every other entry")
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)
    if args.command == "refresh":
        refresh(args)
    elif args.command == "codec-golden":
        codec_golden(args)
    else:
        if not args.out:
            ap.error("build needs --out")
        build(args)


if __name__ == "__main__":
    sys.exit(main())
