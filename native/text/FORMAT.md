# Dirge of Cerberus (FFVII) — text & font formats (native port notes)

Native, dependency-light C for the dialogue/UI text subsystem. Three tools:

| tool        | does                                                              |
|-------------|------------------------------------------------------------------|
| `doctext`   | decrypt an encrypted **text-bin** + parse its string table       |
| `docfont`   | decode the **kelfnt** font atlases (RFD → TIM → PSMT4) to bitmaps |
| `docrender` | composite **real kelfnt glyphs** into an in-game-style text box   |

`render_sample.py` assembles the hero image (`/srv/shared/dirge-port/text_sample.png`).
Build: `make` (needs zlib). Demo: `make demo-text demo-font demo-render`.

---

## 1. text-bin encryption + string table

~1739 encrypted text-bin files (magic `45 90 AF F9 B7 47 F6 94`), mostly under
`extract/_KEL/noPath/` by file-code name.

### Cipher
Transcribed from the community algorithm (Shademp, as implemented in **DoCCryptTool**,
`CryptoClasses/*`) and **cross-checked against the game's own decompiled routine
`FUN_0041bbf8`** in `analysis/decomp/` — which carries the identical magic constant
`0xA1652347`, the keyblock index `counter & 0xF8`, the block-counter expansion
`c | c<<30 | c<<20 | c<<10`, and the 64-bit half-swap. That is the decisive
confirmation the transcription matches the shipped code.

Two 256-byte keyblock tables are generated from 8-byte seeds — header seed
`{88 56 31 95 F1 A3 89 57}`, body seed `{01 00 …}` (the `seedA=1,seedB=0` header) — by:
rotate halves (A<<<8, B<<<16), prepend `+0x45`, a per-byte `+0xD4 ^ (<<2) ^ 0x45`
diffusion (block 0), then 31 more blocks each `= prev*5` (64-bit). Decryption works on
8-byte blocks:
1. per-byte XOR chain `d1 = (blockId^0x45)^c0`, `dk = c[k-2]^c[k-1]`;
2. each `dk` passed 8× through `b = ((b+120) - keyblock[off+i]) & 0xFF`
   (DoCCryptTool `Integers[x] = (x+120)&0xFF`, `LoopAByte`);
3. reassemble as two u32 halves, subtract keyblock (with borrow), XOR a special key
   `Eval+0xA1652347 / Fval+carry` (Eval/Fval = the block-counter expansion), XOR
   keyblock back. Output halves written big-half-first.

File layout (decrypted): `[0x00 header 32B][0x20 body …][tail: plaintext snippet]`.
Header @0x00: `u32 seedA=1, u32 seedB=0, u16 lineCount, u8 _, u8 compFlag,
u32 txtSnippetSize, u32 decompBodySize, u32 fileSize, u32 hdrSize=24, u32 hdrChecksum`.
Body size = `fileLen − txtSnippetSize − 32`; header uses the header key, body the body
key (block counter restarts per section). Checksum = sum of every 4th byte.

### Body / string table
Body is **zlib-compressed** when `compFlag==1` (inflate after decrypt; last 8 bytes are
a size+checksum footer). Decompressed body = a table of `lineCount` entries, 12 bytes
each: `u32 id, u32 lineIdPos (absolute), u32 linePos (absolute)`, then NUL-terminated
line strings, then NUL-terminated line-id strings. **Strings may overlap** — entries
legitimately point partway into a longer string to share suffixes (the "mixed snippet"
quirk the wiki warns about; e.g. several entries pointing at `Data:Kalm` inside
`0:Tempsave Data:Kalm`). `doctext` reads each stored pointer verbatim.

### Encoding — **Shift-JIS**
Japanese build (SLPS-20200). JP strings are Shift-JIS; Western locale files
(`data/etc/locale/{us,uk,it,sp,fr,gr}/…`) are Latin-1/ASCII. Verified samples:

- `中断セーブデータ:カームの街` ("Suspend save data: Kalm town") — `06800020.bin`
- `攻撃で吹き飛ばされた時、地面に落ちる…転倒を回避できます` — `0c7ee1c8.bin`
- `This ability is useful for moving in tight areas. Through training…` — `0c7ee100.bin`
- objectives: `Find Reeve!`, `Deactivate the barricade!` — `06010020.bin`

In-game control codes embedded in lines: `$r` newline, `$c07` colour, `$x170` tab-stop,
`$iba$`/`$ima$`/`$isi$`/`$il3$` button-icon inserts.

---

## 2. kelfnt font format

Fonts are **RFD "fontdatarfd" archives** (not literal `kelfnt0..3` files):

- `data/etc/fontdata1.bin` → `kelfnt0..kelfnt3`, each **TIM3 512×448 PSMT4** 16-col CLUT
- `data/etc/fontdata2.rfd` → `kelfnt4`, **TIM4 512×256 PSMT4** 16-col CLUT

(Per-locale `fontdata1.bin` copies are byte-identical for several languages — the atlas
is the shared Japanese font; Latin text uses its half-width cells.)

Each kelfnt is a PS2 TIM2-family texture whose 4-bit image is an **alpha-coverage mask**
(CLUT index drives alpha; RGB ≈ white). Glyphs sit on a fixed **16×16 cell grid** (32
cols). The 4-bit payload is **GS-swizzled**: the linear source of a GS host→local
`PSMCT32` BITBLTBUF write into a width-`w/2` buffer, sampled back as PSMT4 (TBW=w).
`docfont` inverts that composition using canonical PCSX2 block/column tables (the same
model proven on the port's RST textures).

**Status (RESOLVED — see `GLYPHMAP.md`):** The two atlas families use **different
storage**, which the earlier single-swizzle model missed:
- `kelfnt4` (`fontdata2.rfd`, TIM4) is **GS-swizzled** → deswizzles CLEAN (~320 kanji).
- `kelfnt0..3` (`fontdata1.bin`, TIM3) are stored **LINEAR** (plain row-major 4-bit),
  **not** swizzled. Decoded linear they are CLEAN with **0 holes** — the "half-page /
  ~7% scrambled" gap was an artifact of applying kelfnt4's swizzle to linear data.
  `docfont` now branches on the TIM version (`'3'`→linear, `'4'`→swizzle).

The decoded kelfnt0 reads as a **JIS X 0208 + half-width + NEC + Latin-1** font in code
order. The **half-width ASCII → glyph-cell** map is fully reversed and verified (English
dialogue renders readably). The **full-width katakana (ten 1–8, 41–72) and a hiragana
subset → cell** map is now also reversed and verified glyph-by-glyph — real Japanese
words render with correct glyphs (`render_japanese.py` → `out/20_dialogue_japanese.png`).
The remaining kana (katakana ten 9–40, hiragana middle incl. `の`) and the kanji block
are **absent from the current decoded atlas** (a TIM3 GS-page-order decode gap, not a
mapping gap). Full detail, the cell table, and the evidence are in `GLYPHMAP.md`.

---

## 3. render

`docrender` composites **real** kelfnt4 glyph cells (by cell index) onto an in-game
translucent panel, alpha-blending the coverage mask exactly as the HUD does — a pure-C
render of genuine in-game font bitmaps.

`render_dialogue.py` now renders **real decrypted dialogue** with the **correct glyphs**:
it decodes kelfnt0 (linear), maps each ASCII code to its half-width cell via the reversed
table in `GLYPHMAP.md`, and composites the strings `Deactivate the barricade!` and
`Find Reeve!` (plus the speaker tag and a confirmed-katakana strip) onto an in-game
dialogue panel — readable in-game text in the genuine game font, rendered natively.
Result: `out/13_dialogue_native.png` (copied to `/srv/shared/dirge-port/`). The older
`render_sample.py` fallback-face image (`text_sample.png`) is superseded for Latin text.
