# Chart voice — sample credits and provenance

The built-in guitar and bass voice plays real DI recordings. Most are by
**Karoryfer Samples** (Karoryfer Lecolds), every one of those released under
**CC0 1.0 Universal** — a public-domain dedication: no attribution is required
and any use, commercial use and derivatives included, is allowed. We credit
them anyway, with thanks. Licences verified 2026-10-04 at each repository's
`LICENSE` file at the pinned commit. The Strat packs and the guitar
articulations are CC BY 4.0 (below).

## The sample packs (`guitar.dcvp`, `bass.dcvp`, `strat-*.dcvp`, `guitar-arts.dcvp` — NOT in git)

Built by `tools/voice/build_voice_pack.py` from the files listed, with URL and
SHA-256, in `tools/voice/voice_pack_manifest.json` (1,992 files, measured
2026-10-07): trimmed, mono 16-bit 44.1 kHz, level-normalised.
The app ships them beside `juce_sharp.dll` in `voices/` together with
`VOICE-CREDITS.txt`, which the builder writes from the same list.

| Library (pinned commit) | Used for |
|---|---|
| **Black And Green Guitars** — green Gretsch Anniversary, by Brian Wood — <https://github.com/sfzinstruments/karoryfer.black-and-green-guitars> @ `b3b3249d37dc` | guitar: every note E2–D6 chromatically, p / mf / f layers x 2 round robins; staccato (palm mute); hammer-on; release noises |
| **Shinyguitar** — archtop, magnetic pickup, by D. Smolken — <https://github.com/sfzinstruments/karoryfer.shinyguitar> @ `57243cca8527` | guitar dead notes: each string muted, 3 round robins |
| **Black And Blue Basses** — "darkblack" — <https://github.com/sfzinstruments/karoryfer.black-and-blue-basses> @ `6e7d674cdb41` | bass fingerstyle: every note B0–D4, mf / f x 2 round robins; ghost notes; staccato; release noises |
| **Growlybass** — Squier Jazz Bass — <https://github.com/sfzinstruments/karoryfer.growlybass> @ `4f483268fc66` | bass with a pick: f / ff x 2 round robins every 3 semitones; staccato |
| **Swagbass** — Ibanez BTB, flatwounds, by D. Smolken — <https://github.com/sfzinstruments/karoryfer.swagbass> @ `9d10fcae71af` | the thumb of a slap (muted slap, 5 round robins) — played by the slap MODEL (the pack has no recorded slaps) |

### EGFxSet — the Stratocaster packs (CC BY 4.0, attribution REQUIRED)

**EGFxSet: Electric guitar tones processed through real effects of distortion,
modulation, delay and reverb** — Hegel Pedroza, Gerardo Meza, Iran R. Roman
(2022), version 1.0, <https://doi.org/10.5281/zenodo.7044411>, licensed under
**Creative Commons Attribution 4.0 International**
(<https://creativecommons.org/licenses/by/4.0/>; licence verified 2026-10-04 at
the Zenodo record's metadata). We use the `Clean.zip` archive only (pinned by the
publisher's MD5 and our SHA-256): every string x frets 0..22 of an American Fender
Stratocaster in each of its five pickup positions, plucked with a 2 mm pick into
an Audient iD14. **Changes:** trimmed, resampled 48 → 44.1 kHz, 16-bit,
level-normalised, one pack per pickup position (`strat-neck`, `strat-neck-middle`,
`strat-middle`, `strat-middle-bridge`, `strat-bridge`; each also carries the
Gretsch set's release noises and Shinyguitar's dead notes). The voice ALSO
FILTERS them to simulate other pickups and guitars (Telecaster, Les Paul, the HSS
bridge humbucker, coil splits); those sounds are derived from, not recordings of,
those instruments. The same attribution is written into `VOICE-CREDITS.txt` and
into every pack.

### Guitar-TECHS — the guitar articulation pack (CC BY 4.0, attribution REQUIRED)

**Guitar-TECHS: An Electric Guitar Dataset Covering Techniques, Musical
Excerpts, Chords and Scales Using a Diverse Array of Hardware** — Hegel Pedroza,
Termeh Taheri, Wallace Abreu, Ryan Corey, Iran R. Roman (ICASSP 2025),
<https://doi.org/10.5281/zenodo.14963133>, licensed under **Creative Commons
Attribution 4.0 International** (<https://creativecommons.org/licenses/by/4.0/>;
licence verified 2026-10-04 at the Zenodo record's metadata and the dataset's
site). We use **player 1's** `P1_techniques.zip` only (pinned by the publisher's
MD5 and our SHA-256): the direct-input recordings of an Ibanez PF-300 Les Paul
type (flatwound strings, bridge humbucker) playing palm mutes on every string and
fret, pinch harmonics and natural harmonics. **Changes:** cut into single notes
at onsets found in the audio and labelled from the dataset's per-string MIDI
(natural harmonics by their measured pitch), trimmed, resampled 48 → 44.1 kHz,
16-bit, level-normalised, packed as `guitar-arts.dcvp` (DotCharter #52). The voice
FILTERS them to the selected guitar model's pickup like every recording. The same
attribution is written into `VOICE-CREDITS.txt` and into every pack.

**Player 2 is deliberately NOT packed**: its recordings (and other held-out
material — Karoryfer Emilyguitar's muted strings and round robins, Swagbass's
and Fashionbass's notes and muted plucks, CC0 slap-bass phrases from Freesound)
are the references `tools/voice/realism_eval.py` measures the voice against;
they never ship.

## The waveguide fallback's excitations (`dcvoice_excitation_data.h` — in git)

When no pack is installed the voice falls back to a string model excited by
268 short residuals (12-80 ms, band-limited, 12-bit; ~0.6 MB of source) that `tools/voice/extract_excitations.py`
inverse-filtered out of **Emilyguitar** (<https://github.com/sfzinstruments/karoryfer.emilyguitar>),
**Growlybass** and **Black And Blue Basses** — CC0 1.0 as above.

## Not used, on purpose

No pop recording exists to us, and no slap recording under a FREE licence
(searched again 2026-10-07: Freesound's CC0 / CC BY slap uploads are phrases, two
of them synthesised); the slap and the pop are MODELLED on the finger recordings (see
`docs/agents/chart-voice.md`). Natural and pinch harmonics and palm mutes are real
recordings since #52 (Guitar-TECHS, above); without `guitar-arts.dcvp` the voice
approximates them as before.
We do **not** use IDMT-SMT-Guitar/Bass (non-commercial, no derivatives), RAVE
models (non-commercial), GOAT (research only), EGDB (no licence) or any
commercial library (Ample and similar). No shippable Telecaster, Les Paul or
P-bass recording was found (searched 2026-10-04: Karoryfer's catalogue — its
other basses are a Bass VI, a Killer KB in fifths tuning and a hollow AB-95 — and the usual
free-sample listings, none under a licence we can ship); those models are
simulated from the Strat and the two bass sets.
