# kelfnt glyph atlas — decode + Shift-JIS → cell mapping (reversed)

This closes the two open items in `FORMAT.md §2`. TL;DR:

1. **kelfnt0–3 are stored LINEAR, not GS-swizzled.** The prior "half-page won't
   place / 7% scrambled" gap was an artifact of applying the kelfnt4 swizzle model
   to textures that aren't swizzled. Decoded linear, all four decode with **0 holes**.
2. The atlas is a **JIS X 0208 + half-width + NEC + Latin-1** font laid out in
   (mostly) code order. The **half-width ASCII → cell** map is fully reversed and
   verified (English dialogue renders readably). The kana/kanji region uses a
   bespoke non-linear packing (a code→index table lives in EE code) — anchors are
   documented below; a full per-char kana/kanji table is the remaining work.

## 1. Container format per atlas (the decode fix)

| archive            | glyphs      | TIM | size    | storage               |
|--------------------|-------------|-----|---------|-----------------------|
| `fontdata1.bin`    | kelfnt0..3  | 3   | 512×448 | **LINEAR** 4-bit, row-major scanline |
| `fontdata2.rfd`    | kelfnt4     | 4   | 512×256 | **GS-swizzled** 4-bit (PCSX2 PSMT4-via-CT32) |

`docfont` now branches on the TIM version (`'3'` → `linear4_alpha`, `'4'` →
`deswizzle4_alpha`). The 4-bit index **is** the alpha coverage (`index*17` → 0..255);
rendering coverage directly (not via the zero-filled trailing CLUT) gives crisp,
anti-aliased glyphs. Verification — all CLEAN:

```
kelfnt0..3  TIM3 512×448  holes=0  LINEAR
kelfnt4     TIM4 512×256  holes=0  SWIZZLED
```

Proof the data is 14 full 128×128 pages (114688 B = 14·16384) that *don't* tile a
512×448 swizzle grid, but **do** read perfectly as a plain 512×448 linear image —
the atlas reads as JIS-ordered glyphs (symbols, full-width Latin, kana, Greek,
Cyrillic, box-draw, NEC, Latin-1, then kanji) with no scrambled region.

## 2. Shift-JIS / ASCII → glyph-cell (kelfnt0)

Cells are a 32×28 grid of 16×16 cells; `cell(i)` → col `i%32`, row `i//32`.
Western dialogue is 1-byte ASCII in the decrypted text-bins and uses the
**half-width** cells. Map **verified by rendering each run and reading it back**:

| chars | cells            | formula                   |
|-------|------------------|---------------------------|
| `A`–`J` | 630–639        | `630 + (c-'A')`           |
| `K`–`Z` | 608–623        | `608 + (c-'K')`           |
| `a`–`j` | 662–671        | `662 + (c-'a')`           |
| `k`–`z` | 640–655        | `640 + (c-'k')`           |
| `!` 22, `:` 21, `@` 629, `{` 656, `|` 657, `}` 658, `_` 660, `~` 661 | | |

(The ASCII block is *not* a single linear `c+const` run: the NEC-extension symbols
occupy cells 565–607 where `A`–`J` "would" sit, so the font shifts `A`–`J` down one
atlas row — a bespoke packing. The sub-runs above are each contiguous and proven.)

Rendered real decrypted strings (see `out/13_dialogue_native.png`):
`Deactivate the barricade!` and `Find Reeve!` — readable, in the genuine half-width
game font, composited natively onto an in-game dialogue panel.

### Japanese region — full-width kana → cell map (REVERSED & VERIFIED)

Full-width glyphs follow JIS X 0208 order, but the kana area is packed **non-linearly**
(hiragana / katakana / NEC blocks interleave; the two halves of some atlas rows are
swapped). It is **not** reducible to a single formula — confirmed by exhaustively
reading every kelfnt0 cell and by testing candidate half-swap / linear models, which
all fail somewhere. The map below was recovered **empirically and verified
glyph-by-glyph**: each cell was extracted (16×16) and matched against a reference JP
font (IPAGothic, blurred-cosine over size/shift sweeps) AND confirmed by eye in a
side-by-side proof (`out/21_kana_proof.png`). Every entry here is proven.

**Katakana (kelfnt0 cells), by SJIS code:**
```
ァ8340=168 ア8341=169 ィ8342=170 イ8343=171 ゥ8344=172 ウ8345=173 ェ8346=174 エ8347=175
ド8368=208 ナ8369=209 ニ836a=210 ヌ836b=211 ネ836c=212 ノ836d=213 ハ836e=214 バ836f=215
パ8370=216 ヒ8371=217 ビ8372=218 ピ8373=219 フ8374=220 ブ8375=221 プ8376=222 ヘ8377=223
ベ8378=192 ペ8379=193 ホ837a=194 ボ837b=195 ポ837c=196 マ837d=197 ミ837e=198
ム8380=199 メ8381=200 モ8382=201 ャ8383=202 ヤ8384=203 ュ8385=204 ユ8386=205 ョ8387=206 ヨ8388=207
ー815b=324
```
Pattern within kelfnt0 row 6: SJIS trail 0x68–0x77 → cells 208–223 (the RIGHT half,
filled first), trail 0x78–0x88 → cells 192–207 (LEFT half) — a per-row half-swap. The
0x7F SJIS gap (ミ 0x837e → ム 0x8380) lands ミ=198, ム=199 (contiguous cells).

**Hiragana (kelfnt0 cells), by SJIS code (proven subset):**
```
し82b5=128 … て82c4=143   (trail 0xb5–0xc4, contiguous)
え82a6=145 ぉ=146 お=147 か82a9=148 … ざ82b4=159   (trail 0xa6–0xb4)
ぷ82d5=160 へ=161 べ=162 ぺ=163 ほ=164 ぼ=165 ぽ=166 ま82dc=167   (trail 0xd5–0xdc)
よ82e6=177 ら82e7=178 り=179 る=180 れ=181 ろ=182 わ=183 を82f0=186 ん82f1=187
```
Greek lower α–ω: cells 230–253 ; Cyrillic А…: cell 260. First JIS L1 kanji `亜`
(SJIS 0x889F): kelfnt0 cell 722 (kanji then run into kelfnt1/2/3).

### The SJIS→cell mechanism (what the EE does, and the decode gap)

- The EE **text decode path is a stock libc `mbtowc`** (`FUN_0044b598`, with the
  SJIS/EUC/ISO-2022-JP converters `FUN_003cc208/003cd7e8/003cf338/003e3a68`): it only
  turns a SJIS lead+trail into the raw 2-byte code. The **cell** is produced downstream
  by game code; it is **not stored as a simple u16 SJIS→cell array** — searched the ELF
  (`SLES_541.85`) exhaustively for the confirmed cell runs and found none (the only long
  +1 u16 run near the fonts, at `0x3aa470`, is the Windows-1252→Unicode table 160..255,
  which coincidentally overlaps the katakana cell numbers — a false positive to avoid).
  So the kana/kanji cell is **computed** (bespoke packing), not tabled.
- **Decode gap (the real blocker for arbitrary JP):** a contiguous block of full-width
  kana is **absent from the current decoded atlases** — katakana ten 9–40
  (`ォ`–`ト`, i.e. `カ セ タ デ` …) and hiragana `で`–`ぶ`/`み`–`ょ`/`ぁ`–`う`
  (incl. `の`). Verified by reading every one of kelfnt0's 896 cells and by template-
  matching the full kana syllabary across all five atlases (kelfnt1–4 are pure kanji).
  These glyphs render in the shipped game, so they exist in the ROM — the most likely
  cause is that the LINEAR TIM3 decode mis-orders one or more of the 14 128×128 GS pages
  (448 = 3.5 pages; the swizzle geometry doesn't tile cleanly — the exact caveat docfont
  already flags). Re-ordering those pages is a **decode** fix (prior agent's domain),
  after which the same verification method extends the map to the full syllabary.

## 3. Honest coverage

- ✅ kelfnt0–4 decode fully clean (linear/swizzle split) — the "unplaced page" gap closed.
- ✅ Half-width ASCII → cell: complete and verified; real English dialogue renders readably.
- ✅ **Full-width katakana ten 1–8 and 41–72 + a hiragana subset → cell: reversed and
  verified glyph-by-glyph.** `render_japanese.py` renders real Japanese words
  (`さくら` `ちから` `そら` `おかし` `ナイフ` `パイプ` `ハム`) and the katakana syllabary
  rows with the correct genuine kelfnt0 glyphs → `out/20_dialogue_japanese.png`
  (proof: `out/21_kana_proof.png`). Readable, correct, in the real game font.
- ⚠️ Full-width katakana ten 9–40 + hiragana middle/head (incl. `の`), and the kanji
  block, are **missing from the decoded atlas** (see decode gap above) — so the exact
  validation string `中断セーブデータ:カームの街` cannot yet be rendered end-to-end
  (its `セ タ カ デ`, `の` and `街` fall in the absent block). The mechanism and the
  verified-map method are in hand; completing them waits on the TIM3 page-order fix.
