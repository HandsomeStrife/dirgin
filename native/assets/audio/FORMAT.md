# Dirge of Cerberus (PS2, SLPS-20200) — audio stream format

Recovered spec for the game's headerless ATRAC3-family voice/SE/BGM streams, the
sound database (`db.bin`), and the wave-dictionary banks (`*.wd`). Decoder:
`dirge_atdec.c` (links libavcodec's atrac3 / atrac3plus). Validation WAVs in
`/srv/shared/dirge-port/`.

## 1. The two stream codecs (DECISIVE EVIDENCE)

| ext  | files | codec       | ch  | rate   | frame (block_align) | samples/frame | status |
|------|-------|-------------|-----|--------|---------------------|---------------|--------|
| .at2 | 2726  | **ATRAC3**      | 1 (mono) | 44100 | **0x60 = 96 bytes** (voice/SE) | 1024 | **fully decoded, 0 errors** |
| .at3 | 382   | **ATRAC3plus**  | 1 (mono) | 44100 | **0x180 = 384 bytes**          | 2048 | format cracked; ffmpeg decodes first N frames only (see §5) |
| .at3 | 1     | ATRAC3      | 1 (mono) | 44100 | 0x0C0 = 192 bytes (`title.at3`, higher-bitrate BGM) | 1024 | fully decoded, 0 errors |

All streams are **raw, headerless** ATRAC3-family elementary streams: no RIFF/OMA/EA3
wrapper, no loop/rate header in the file. The game DMAs the raw frames over SIF to the
IOP, which runs Sony's `libatrac3`/`libatrac3plus` (the EE decompiled corpus contains
NO ATRAC constants or decode code — decode is entirely IOP-side; the params below were
recovered from the byte structure, `db.bin`, and empirical decode).

### How the codec/variant was proven
- **ATRAC3 vs ATRAC3plus by frame sync byte 0:**
  - ATRAC3 frames begin with the 6-bit *Sound Unit Id* `0x28`. All 2726 `.at2`
    (and `title.at3`) have `(byte[0] >> 2) == 0x28`.
  - ATRAC3plus frames begin with a `0` start bit. All 382 BGM `.at3` have
    `(byte[0] >> 7) == 0`. (`ffmpeg` confirms: the `atrac3`/`atrac3plus` decoders
    accept the respective streams and reject the other with "Sound Unit id != 0x28".)
- **ATRAC3 frame size = 96 bytes:** every 96-byte boundary of every `.at2` carries the
  `0x28` sync (0/… bad across the whole library). 192 happens to "work" too (192 = 2×96)
  but silently merges two frames and halves the audio; 96 is the true frame (a 192-byte
  read would put mid-frame bytes — not `0x28` — at odd boundaries, which never occurs).
  `title.at3` is the one ATRAC3 file whose only all-`0x28` block is 192 → 66 kbps BGM.
- **ATRAC3plus frame size = 384 bytes:** confirmed independently by the game's own DB —
  `db.bin` length field equals the 384-byte frame count (e.g. bgm_070: DB=2626 vs
  1009920/384 = 2630; bgm_044: DB=5662 vs 5666; bgm_136: DB=7512 vs 7516).
- **Mono:** every AT3+ frame's first channel-unit type is `MONO` (bits after the start
  bit = `00`); ffmpeg's stereo config is rejected ("Frame data doesn't match channel
  configuration"). ATRAC3 `.at2` decode as 1-channel with coherent speech (see §4).
- **Not joint-stereo / not byte-swapped:** 16-bit byteswap breaks the start bit;
  joint-stereo coding mode is rejected by the decoders.

## 2. `db.bin` — master sound database  (184344 bytes = 7681 × 24-byte records)

Record (24 bytes):
```
+0x00  char[8]  name   (e.g. "bgm_070\0", "on_v001\0", NUL-padded)
+0x08  u8       slot / sub-index
+0x09  u8       flags  (0xFE for the streamed-BGM table, 0x00 for resident, …)
+0x0A  u16      field  (per-table constant, e.g. 0x0520 in the resident section)
+0x0C  u32      f12    (per-type constant: 66 voice / 212 BGM — format/profile id, unconfirmed)
+0x10  u32      f16    loop start, in frames   (0 = no loop; voice = 0)
+0x14  u32      f20    length / loop end, in frames
```
`f20` (length in frames) is corroborated for every BGM against the real 384-byte frame
count (§1). `f16` loop-start < `f20` for BGM (e.g. bgm_070 loops frame 291 → 2626),
and is `0` for voice → **this is the loop-point source** the headerless streams lack.
The `f12`/`f16`/`f20` meaning for voice entries is less certain (voice `f20` ≠ its frame
count), so treat non-BGM loop fields as provisional.

`jsnddb.txt` is a human-readable name/label table (Shift-JIS BGM/SE titles), NOT a
parameter DB.

## 3. `*.wd` — wave-dictionary banks  (SPU2 SE pool, **SOLVED**)

Decoder: `dirge_wd.c` (self-contained C, no libav; `make dirge_wd`). The sample
codec is **standard PS2 SPU-ADPCM / VAG** — proven decisively from the byte
structure (see below). The `.wd` SPU-upload/parse code is **not** in the EE
decompiled corpus (`analysis/decomp/` has no `sceSd*`, no SPU2 registers
`0x1f90xxxx`, no ADSR/pitch constants, no `.wd` strings) — exactly like the ATRAC
path, the WD→SPU2 handling is **IOP-side** and out of the EE dump. The spec below
is therefore grounded in consistent bytes + canonical SPU-ADPCM, not EE code.

### Bank header + layout
```
+0x00  char[2]     "WD"
+0x02  u16         bank id   (0 for the 000x pair, 1000=0x3E8 / 1001=0x3E9 for 100x)
+0x04  u32         sample-data region size in bytes (region that follows the table)
+0x08  u32         entry count          (wave0000: 294, wave0001: 311, 1000:138, 1001:121)
+0x0C  u32         entry count (duplicate)
+0x10  u32[4]      reserved (≈zero; one bank has a stray 0x08 at +0x15)
+0x20  u32[count]  offset table — file offset of each 32-byte descriptor
                   (contiguous, 0x20 stride: entry i at 0x4C0..+i*0x20 in wave0000)
...                count × 32-byte descriptors
...                SPU-ADPCM sample-data region, begins at (max offset)+0x20
```
The region base = `(highest offset-table value) + 0x20` (end of the descriptor
table). The `+0x04` size field ≈ region length but carries up to ~0x30 trailing
padding in some banks, so the decoder derives the base from the table, not the
size field (for wave0000 they agree exactly: base 0x2980 + 0x1CD040 = file size).

### Descriptor (32 bytes)
```
+0x00  u32   reserved (always 0)
+0x04  u32   sample start offset within the sample-data region (ascending)
+0x08  u32   sample length in bytes  (see length rule below)
+0x0C  u32   SPU2 ADSR envelope word — constant 0x5FE800FF for every active voice
             (ADSR1=0x00FF, ADSR2=0x5FE8); 0 marks an EMPTY/unused slot
+0x10  u32   per-sample value (varies; purpose unresolved, NOT needed for PCM)
+0x14  f32   base pitch / tuning ratio — a few discrete per-bank values
             (2.851=0x40367F3C, 3.0, 3.25, 3.992=0x407F7F3C); NOT a per-sample Hz
+0x18  u32   reserved (0)
+0x1C  u32   reserved (0)
```
- **Empty slots:** a descriptor with ADSR(+0x0C)=0 and offset=length=0 is an unused
  index slot (banks reserve more indices than they fill — wave0000: 248 active of 294,
  wave1001: 92 of 121). Decoder skips these.
- **Length rule:** `+0x08` is the authoritative byte length for most samples (their
  ADPCM body is all flag-0/flag-2, no end marker). ~159/294 descriptors instead store
  `+0x08 == +0x04` (a sentinel); for those the sample fills its slot (up to the next
  active sample's start) and terminates on a VAG **end flag** (flag byte 1 or 7). The
  decoder stops at `min(declared length, next-start)` AND at any end-flag block, which
  handles both cases and never overruns into the next sample.
- **Pitch/rate (honest gap):** the descriptor carries NO per-sample sample rate. `+0x14`
  is a near-constant per-bank tuning float (only 1–3 distinct values per bank, spaced
  like musical intervals), i.e. these are sequencer/instrument samples whose final
  playback pitch is set at runtime by the note played. WAV output uses a chosen default
  rate (`-r`, default 44100 Hz); this affects playback speed only, not decode fidelity.

### Codec confirmation — PS2 SPU-ADPCM / VAG (decisive)
Across all 3.79M 16-byte blocks of wave0000:
- **shift nibble** (control byte low nibble) spans exactly **0–12**, never 13–15
  (the valid SPU-ADPCM shift range);
- **filter nibble** (control byte high nibble) spans exactly **0–4**, never 5–15
  (the 5 valid SPU predictor pairs);
- **flag byte** only ∈ {0,1,2,3,4,7} (the valid VAG flag set: 0 normal, 1/7 end,
  2 loop body, 3 loop end, 4 loop start).
Decoded with the canonical Sony filter table (×64 fixed point):
`pos = {0,60,115,98,122}`, `neg = {0,0,-52,-55,-60}`;
`out = clamp16( (nibble<<(12-shift)) + ((h1*pos[f] + h2*neg[f])>>6) )`, 28 samples/block.
- **Loops** are carried in the ADPCM stream flags (standard VAG), NOT in the
  descriptor: only ~11 samples per bank carry explicit loop-start (flag 4/6) / loop-end
  (flag 3) markers; most SE are one-shots. `dirge_wd` reports loop block indices in its
  listing; WAV output has no loop metadata.

## 4. Validation (see `/srv/shared/dirge-port/`)
- `voice_sample.wav`  on_v132.at2  — ATRAC3, 96-byte frames, 315 frames, 0 errors, 2.23s.
  Per-0.2s RMS `3910 6224 1932 6901 6927 9219 4121 224 55 12 3 0` = clear speech envelope
  (syllables then decay to silence).
- `se_sample.wav`      se_0444.at2  — ATRAC3, 96 frames, 0 errors, 7.31s.
- `bgm_title_theme_atrac3.wav` — title.at3 (ATRAC3 192-byte), 26410 frames, 0 errors;
  30s excerpt. Full track 613s with music dynamics (RMS varies, quiet bridge, fade-out).
- `bgm_atrac3plus_PARTIAL.wav`  bgm_136.at3 / `bgm_070_atrac3plus_FAILED_prefix.wav` —
  ATRAC3plus, 384-byte frames; ffmpeg emits output for the first N frames (27 / 5) then
  hard-errors. **This output is NOT coherent music** (re-characterized 2026-10-15, see §5):
  the envelope is intermittent full-scale-clipping bursts separated by dead silence
  (bgm_136 per-0.05s RMS `0 5445 1564 1485 16316 8321 0 24 55 0 7606 ...`, peaks railed
  at 32767) — decoder artifacts from a structurally invalid parse, not a BGM excerpt.
- **`.wd` SPU-ADPCM SE (`dirge_wd`)** — `sfx_sample_014/015/192.wav` from wave0000.wd.
  All four banks parse and decode (`make wdcheck`). Coherence across all 223 decodable
  active samples: mean lag-1 autocorrelation **0.698** (65% > 0.8, 77% > 0.5) — real
  waveforms, not noise (white noise ≈ 0). Exemplars: idx 14 (type-1, 25424 smp,
  autocorr 0.994, 0% saturated), idx 15 (type-1, autocorr 0.951), idx 192 (type-2
  end-flag, autocorr 0.995) — all with peaks near but not railed at full-scale. Lower-
  autocorr samples are genuine noise-like SFX (static/impacts). wave1000 shares samples
  with wave0000 (same idx-15 decode), confirming cross-bank decode.

## 5. Unresolved / future work
- **ATRAC3plus full decode — NOT a mid-stream codec desync; the per-frame bit layout
  does not match standard AT3+ (re-investigated 2026-10-15).** The stream-level facts
  (384-byte byte-aligned frames, 1 frame = 2048 samples, count = size/384 ≈ db.bin, mono)
  are solid. But decoding is blocked *before* any codec-internal path, at the frame header:

  - **ffmpeg's standard parse (frame @ byte 0, MSB-first, as canonical AT3+) is
    structurally impossible here.** Reading the channel-unit header by hand on EVERY frame
    of EVERY BGM track gives the *identical* result: start-bit 0, channel=MONO,
    `num_quant_units = 2`, `mute_flag = 1` — on all 2630 frames of bgm_070, all 7516 of
    bgm_136, etc. This is because **`byte[0] ≡ 0x01` on literally every frame of every
    track** (verified across the library). Real music cannot have a constant 2-quant-unit,
    residual-muted header on every frame; `mute_flag=1` zeroes the residual spectrum, so
    the "decoded" output is only misread tonal/gain data → the clipping/silence artifacts
    in §4. ffmpeg hard-errors once that garbage trips `num_coded_vals > num_quant_units`
    (=2): *"Invalid number of transmitted units"* (atrac3plus.c:134). N≈5–27 is just how
    many frames the invalid parse stays in-bounds before that check fires — not a desync.

  - **The real frame layout is a DoC/Sony-PS2-specific header or bit-packing, not
    recoverable by re-framing.** `byte[0]` is a constant `0x01` marker; `byte[1]` takes
    only ~9–14 values; `byte[2..3]` are low-entropy/structured; `byte[4..383]` are high
    entropy (payload). An exhaustive scan over {MSB, LSB/bit-reversed, 16-bit byteswap,
    32-bit byteswap} × byte-offset 0–8 finds **no** alternative where a valid start-bit(0)
    + MONO + a *varying* `num_quant_units` holds across all frames — only the standard
    offset satisfies start+mono, and only because `0x01` is constant. Whole-file byteswaps
    decode *worse* (sw16: 0 ok; sw32: 8 ok). So the bytes are genuinely not in any trivial
    reordering of the canonical AT3+ bitstream.

  - **A container wrapper cannot fix it (ruled out empirically).** libavcodec's AT3+
    decoder ignores `extradata` entirely — it takes channels + `block_align` only, both
    already correct in `dirge_atdec`. Feeding a reconstructed **OMA/EA3** wrapper through
    ffmpeg's own demuxer→decoder (`bgm070.oma`) gives a **byte-identical failure**: same
    "Invalid number of transmitted units", same 5 frames / 0.232 s, in both ffmpeg 4.4.2
    and the static 7.0.2 build. RIFF-WAVE-ATRAC3PLUS would be the same (same params).

  - **No AT3+ decode tooling exists on this box** (`atracdenc`/`at9tool`/`at3tool`/
    `vgmstream-cli` all absent; `atracdenc` only *encodes* AT3 anyway). ffmpeg is the only
    AT3+ decoder present, and its RE'd decoder assumes the canonical layout DoC doesn't use.

  **To finish:** recover DoC's exact AT3+ frame header / bit-packing — which realistically
  needs Sony's `libatrac3plus` or the IOP-side decode routine (NOT in the EE decompiled
  corpus — confirmed: no ATRAC constants/code EE-side), then feed correctly-framed data to
  a complete AT3+ decoder. Header-only reverse-engineering from the files is insufficient.
  ATRAC3 (.at2 voice/SE + title.at3 BGM) is complete and unaffected.
- **Loop points** recovered for BGM from `db.bin` f16/f20 (frames); voice loop fields
  unconfirmed. Not yet wired into the decoder output (WAV has no loop metadata).
- **`.wd` bank** — SOLVED (§3): SPU-ADPCM/VAG confirmed, descriptor layout + length
  rule cracked, `dirge_wd.c` decodes all four banks to coherent PCM. Remaining gaps:
  (a) per-sample playback rate is not in the descriptor (runtime/sequencer-controlled;
  WAV uses a default rate), and (b) the bank→`db.bin`/sound-id indexing and the `+0x10`
  per-sample word are not yet mapped.
- A from-scratch ATRAC3 decoder (we currently link libavcodec's) is future work; the
  current path proves the format is cracked and produces standard PCM WAV.
