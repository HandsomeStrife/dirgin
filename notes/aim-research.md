# Aim, reticle and shooting: what the game does (worker 0077, 2026-10-27)

Research only; nothing under native/web/ was changed. Sources: analysis/decomp/*.c, the
recompiler output (port/recomp/generated_all), capstone disassembly of RAM for functions
Ghidra missed, new PCSX2 traces, and one new GS dump. Confidence: **D** decoded (code read,
meaning clear), **T** traced (measured in the real game, code not fully read), **L**
likely, **UNDECODED**. Units: 1 tick = 1/60 s; a PAL frame is 1.2 ticks. World is −Y up.

Names used below: **P** = player object (0x1339e0 in the z128 state), **C** = *(P+0xe8)
(body), **CAM** = *(DAT_0046e290+0x40), **HUD** = *(DAT_0046e290+0x44) (view object),
**RET** = DAT_00471380 (reticle sprite object), **W** = weapon stats (FUN_0055a378(P),
the 0x78-byte record in actions-research §5.3).

## 0. Summary (correcting earlier notes)

- There IS a weapon-ready mode, but it is a **toggle, not a held button**. The first R1
  press draws the gun (weapon state 0xd/0xf, ~6 ticks) and switches the camera to the
  weapon camera (type 0xd); it does not fire. Later R1 presses fire. The mode persists
  (no auto-holster seen in 220 idle frames) until ○ holsters. actions-research §5.2's
  "no aim mode" is wrong in that sense; it is right that there is no hold-to-aim button.
- While drawn, Vincent **always faces the reticle's hit point** and moves with the aimed
  clip sets (strafing). The reticle is **fixed at screen centre horizontally**; it moves
  **vertically** with camera pitch (formula §3.3, verified to 1e-4) and kicks up with each
  shot.
- **R3 = first-person view, not a zoom**, for the handgun: camera type 0xe puts the eye at
  head height and fades Vincent out; the FOV stays 73.5. Only scope weapons (W+4 bit 2)
  get camera type 0xf with FOV = W+0x34 (the 25° number in older notes).
- In this save the loadout's runtime W differs from the bgd defaults in actions-research
  §5.1: fire interval **25 ticks** (not 38), range field **31 → 310 units** (not 565),
  recoil ≈2°. Read W at runtime; don't hard-code §5.1.
- The save has **auto-lock on** (HUD+0x64 bit 0, CAM+0x1e8 bit 2): reticle kind 2 and the
  lock-on picker run. z128's start has no actors, so lock-on itself is code-only.

## 1. Traces and dump (all gitignored)

`TRACE_FIELDSET=aim` (new, opt-in, tools/trace/fields.py) swaps the cape fields for the
weapon state (P+0x290..0x2bf, P+0x118..0x12f, P+0x188), CAM+0x28/0x48/0x60..0xdf/0xf0..
0x10f/0x170..0x22f, HUD+0x60..0xaf, RET+0x1b0..0x20f and the camera-type objects at
0x4eebe0/0x4eec40/0x4eeca0. Default traces are unchanged. Runs (wrap in the lock):

    TRACE_FIELDSET=aim flock /srv/shared/dirge-port/trace.lock \
      python3 -I tools/trace/trace.py SCRIPT.json --out tools/trace/out/aim --scratch <unit scratch>

(`--scratch` must be the dir whose trace.env the pcsx2trace unit reads.) Output:
tools/trace/out/aim/*.jsonl. Scripts (frames, events; 2-frame presses unless noted):

| trace | input |
|---|---|
| aim_a_tap | R1 10 (draw); R1 40,46,52,58,64,70,100; idle to 280 |
| aim_b_hold | R1 held 10-200 |
| aim_c_walk | R1 10; ly −1 30-150 with R1 60,100; lx +1 160-240 with R1 190 |
| aim_d_turn | R1 10; rx +1 30-130 with R1 60,100; ry −1 150-200 with R1 180; R1 240 |
| aim_e_zoom | R1 10; R3 40; R1 90; rx +1 110-130; R3 160; ○ 220 |
| aim_f_r3cold | R3 10 (holstered); R3 100 |
| aim_g_air | R1 10; ✕ 40-44; R1 48, 60 (air); R1 120 |

GS dump: tools/gsdump/dumps/aim/reticle_f200.{gs.zst,bin,png}, frame ~200 of a script
"R1 at 10, then stand" (Shift+F8 sent with xdotool to the trace unit's Xvfb :97; the grab
helper was scratch-only: it is trace.run plus a PINE wait on CTRL+4 and `xdotool key
shift+F8`). The script area holds at most 1024 frames.

## 2. Input → weapon (FUN_003320c0, per frame) — D

Logical ids (FUN_00254810(id, mode): mode 0 held, 1 pressed, 2 released): 9 = R1,
0x1f = L1, 0x11 = R3, 0xc = ○, 0xa = R2 (reload).

1. **Not drawn** (P+0x2a8 bit 0 clear):
   - R3 pressed (and no R1/L1 press): FUN_003286d8(CAM) (reset lock ids, run the aim ray),
     HUD+0x74 = 0, face CAM+0xb0, FUN_0032a390 (camera type 0xe or 0xf), FUN_005766c0(P,1,1)
     (draw). Trace aim_f: one R3 draws and goes straight to first person.
   - R1 pressed: same but FUN_0032a420(CAM, 2) (weapon camera, type 0xd) instead of
     0032a390. Gate: on ground or P+0x2a8 bit 3 clear. Trace: draw only, no shot.
2. **Drawn**: ○ pressed → FUN_005766c0(P,0,1) (holster; trace aim_e f221: player state 3→2,
   camera type 0xd→0xc, reticle fades out). R3 pressed (FUN_005773c8: weapon state not
   10/11) → view mode 2 ? FUN_0032a390 (first person) : FUN_0032a420(CAM,2) (back).
3. Then, with a weapon: reload (R2 → FUN_00574f40, state 7/8), auto-reload when the
   magazine is empty (FUN_00316c18 + option FUN_00253f18(9)), and **fire**:
   `(R1 held && W+4 bit 0) || (R1 pressed && !(W+4 bit 0))`, L1 not held,
   FUN_00317a58 false (ammo), FUN_00577180 (weapon state 0 AND drawn AND !FUN_0055a410):
   FUN_00574800(P,0) (P+0x2a0 = 0); FUN_00574a80 (semi: state 1, timer = W+0x42) or
   FUN_00574b00 (auto: state 3, timer = W+0x42); face CAM+0xb0 (FUN_003296e0(CAM,·,0) →
   FUN_00562ef8); P+0x29c = 3.
   Releasing R1 on an auto weapon → FUN_00574b98: state 3 → 0, 4 → 2.
   Trace aim_b: holding R1 from the draw press never fires (semi-auto needs a new press).
   Trace aim_a: presses while the timer runs (f46, 52, 58) are ignored, not queued.

## 3. Weapon state machine FUN_00579570(P) — D (order of operations)

Runs per frame on P. `acc = P+0x294 += DAT_004eeb24 (step)`; `while acc >= 1: switch
(state = P+0x2a4) ...; acc -= 1`. So the body runs once per whole tick (1 or 2 times per
PAL frame; traced: timer 25,24,22,21,... ). T = P+0x290 (float timer). AIM = FUN_005784d8
(P,1): if drawn (P+0x2a8 bit 0) and CAM exists and P+0x2a0 == 0 → P+0x2b0 = CAM+0xa0 (aim
ray hit point, §4) and face it (FUN_00562ef8: P+0x120 = point; C+0x670 via FUN_003424c0;
P+0x118 = (P+0x118 & ~2) | 1). POSE(x) = FUN_00565ff0(P, x): P+0x188 bit 38 = x.

| state | body (in order) |
|---|---|
| 0 idle | if drawn: AIM; POSE(0) |
| 1 semi wind-up | POSE(0); if T > 0: T −= 1. else: **fire** FUN_00577480(P); state = 2; T = interval (below) |
| 2 semi recover | AIM; POSE(0); if T > 0: T −= 1 else state = 0 |
| 3 auto wait | if T > 0: T −= 1. else: fired = FUN_00577480(P); POSE(fired ? 1 : 0); state = 4; T = interval |
| 4 auto recover | AIM; if T > 0: T −= 1 else state = 3 |
| 5 materia shot | AIM; FUN_00577e68(P); POSE(0); state = 6; T = 60 |
| 6 materia recover | if T > 0: T −= 1; if T < 60: AIM. else state = 0 |
| 7 reload start | state = 9; T = W+0x46 (reload ticks); POSE(0) |
| 8 reload (other) | POSE(0); state = 9; T = W+0x44 |
| 9 reload | POSE(0); AIM; if T > 0: T −= 1 else state = 0 |
| 10 | POSE(0); state = 0xb |
| 11 | AIM; if T > 0: T −= 1 else state = 0 |
| 12 | state = 0xd; POSE(0) |
| 13 draw/holster | AIM; POSE(0); if T <= 0: state = 0 else T −= 1; then (fall through) state = 0xf; POSE(0) |
| 14 | state = 0xf; POSE(0) |
| 15 | AIM; if T <= 0: state = 0 else T −= 1; POSE(0) |

Interval after a shot (states 1 and 3): if FUN_00317b68(slot) (slot+0x64 bit 0x20 and slot
+0x20 top nibble == 2): half = W+0x44/2 (rounded toward 0); T = half < 4 ? 3 : half.
Else, state 1 only: if FUN_00316a20(slot) (true when FUN_00317b68, or the slot has an
ammo record per FUN_0031a1e8): T = W+0x44; otherwise T = min(W+0x44, 15) (W+0x44 < 16 ?
W+0x44 : 15). State 3 always uses W+0x44 (or the half). Traced: T = 25 after every shot
(aim_a f41/65/101, aim_g f49).

Draw: trace shows state 0xd T 6 → 0xf T 5..0 → 0 (f11-17). The code that sets 0xc/0xd
with T = 6 was not located (FUN_005766c0 → FUN_0055b8e0 chain). UNDECODED (setter only).

Fire (FUN_00577480; actions-research §4/§5 have the bullet): bullets go from the muzzle to
P+0x2b0. After a shot: FUN_0033a310(0x3e9, …, CAM) = camera message "shot" (§5.4 recoil);
FUN_0032a320(table 0x46eee0[class]) sets CAM+0x224 = 0.75 (class 1), CAM+0x228 = 0.01
(eye kick, decays 0.01·step per frame, traced 0.75, 0.738, 0.714...). The eye kick is only
applied when FUN_00326500() (DAT_0046b2fc != 0; it is 2 in this save) (FUN_0032ace0
l.2409: eye += CAM-vector × kick). The air-shot hop (FUN_00577d40) needs FUN_0055aa88(P)
etc.; traced: no hop in z128 (aim_g apex 13.09 as without a shot), P+0x270 → 1.

## 4. Aim point and fire direction — D (+T)

**FUN_00329778(CAM)** (once per frame, flag CAM+0x1e8 bit 0x400 guards re-entry):
1. far = max(1050, CAM+0x1b8).
2. M = CAM view matrix (CAM+0x60..) rotated by pitch (CAM+0x1cc + 1.0·CAM+0x1fc) and yaw
   (CAM+0x1d0 + 2.0·CAM+0x200) (FUN_00259928 / FUN_00259990; DAT_0046e420 = 1,
   DAT_0046e424 = 2). Row 3 = O (eye), row 2 = F (ray direction). CAM+0x1cc is the
   reticle's angle below the view centre (§5.3), so **the ray goes through the reticle**.
3. d = |O − CAM+0xf0| (eye to look-at point). S = O + F·(d + CAM+0x1d4) (CAM+0x1d4 = 10:
   the ray starts 10 past the player's depth, so it never hits Vincent). E = S + F·far.
   CAM+0xb0 = S + F·10000 (the "no hit" point).
4. Lock mode (CAM+0x1e8 bit 2, see below) → FUN_00328968. Otherwise
   FUN_005449f0(world, 1, hit, normal, S, E): no hit → CAM+0xa0 = CAM+0xb0, bit 0x200
   cleared, CAM+0xc0 = F; hit → CAM+0xa0 = hit point, CAM+0xc0 = normal, CAM+0x184 =
   |hit − S|, bit 0x200 set; if the hit object is an actor (FUN_0054ea60(obj, 0xc) false)
   within CAM+0x1b8: "target in range" → HUD flag 2 (§5), and a beep (0x4a05d8) when the
   target id changes; FUN_00573378(player, id).
5. HUD flag 0x10 cleared (FUN_0033c340(HUD,0)); flag 2 = in-range (FUN_0033c2b8).

**Readers.** FUN_003296e0(CAM, out, which): which = 1 → CAM+0xa0 (hit), 0 → CAM+0xb0
(far point); returns CAM+0x1e8 bit 0x200. The weapon's aim point **P+0x2b0 = CAM+0xa0**
(FUN_005784d8, every tick while drawn); the press handlers face CAM+0xb0. Traced: P+0x2b0
== P+0x120 == CAM+0xa0 every frame (aim_a). In z128 the ray hits walls (~100 units away);
the hit point is world geometry.

**Ranges** (FUN_00575510, on weapon/camera change): CAM+0x1b8 = W+0x18·10 (traced 310);
CAM+0x1b0 = W+0x38·10 (or W+0x18·10 when FUN_00326500()) (traced 310). With no weapon
both = 300. CAM+0x1b4 = 0.4 in this save (setter not found; lock cone, below).

**Lock-on (FUN_00328968)**, only when FUN_00321e20(W) (W+4 bit 0x20, or the auto-lock
option FUN_00253fb8() in 1..2 when FUN_00312698()): FUN_00328660(CAM,1,…) sets CAM+0x1e8
bit 2 (and 0x80000 for option 2). Each frame: ray S→S+F·far with probe 2 (actors), then
probe 1. Acquire: an actor within CAM+0x1b0 → CAM+0x1e4 = its id, beep (0x4a05d0 first,
0x4a05d8 switch), CAM+0x218 = 0.05. Keep: target within 1.1·CAM+0x1b0 and the angle
between (target − S) and F below FOV·(π/240)·CAM+0x1b4·0.875 (= 19.3° at FOV 73.5), and
its projected screen point inside x ∈ (−60, 572), y ∈ (−110, 558) (622/508 in the 0x1e
video mode). Otherwise drop: CAM+0xa0 = hit point or CAM+0xb0. While locked, FUN_00329778
sets **CAM+0xa0 = target position + 0.5·(target+0x410)** (its centre): auto-aim. HUD:
locked → flags 0x10 + 2; not locked but an actor under the reticle in range → flag 2.
D (code) / not traced (no actors at the z128 start).

**Undecoded:** the auto-lock option value and where FUN_00253fb8 reads it; whether a
non-lock weapon in this save would differ; CAM+0x1b4's setter.

## 5. The reticle (HUD) — D + T + GS dump

### 5.1 When shown
The camera type's update function sets the HUD reticle mode (FUN_0033ba88(HUD, m) →
HUD+0x60): type 0xc normal camera FUN_00330478 → **0 (hidden)**; type 0xd weapon camera
FUN_00330980 → **1**; types 0xe/0xf FUN_00330f08 → **2** (first person / scope). Vtables
0x4a07a8 / 0x4a0758 / 0x4a0708 / 0x4a06b8, slot +0x14. FUN_0033c060 maps the mode to a
reticle **kind** (RET+0x1b0): mode 0 → 0 (off); mode 1 or 2 → kind 1, or kind 2 when HUD
+0x64 bit 0 (lock-on enabled; this save). Mode 2 with HUD+0x64 bit 3 also shows the scope
overlay (FUN_003ae8d0; bit 3 set by FUN_0033c400, cleared on leaving type 0xf).
Traced (aim_a): reticle appears the frame after the draw press (HUD mode 1, kind 2).

### 5.2 Animation phases (FUN_003b0ac0, per kind k = 1..7)
RET+0x1b2+k = phase, RET+0x1bc+4k = end time, RET+0x1dc+2k = length (clock FUN_003e2450).
0 off; 1 fade in → 2 shown; 3 fade out → 0; 4 → 5 → 6 "on target" (RET+0x1b1 bit 0, from
HUD flag 2); 7 → 8 → 9 "locked" (bit 1, from HUD flag 0x10). Transitions: FUN_003b07b0 /
0840 / 08a0 / 08f8 via FUN_003b0490 / 0588 with lengths DAT_00472130[k] (normal) and
DAT_00472140[k] (variant). Draw: FUN_003b1048(RET, ctx, k, t, variant 0/1/2). Traced:
phase 1 for 4 frames, then 2 (f12-16); on holster phase 3 for 4 frames, then 0.

### 5.3 Position — D, verified
FUN_0033c868: x = HUD+0x7c·256 + 256 + HUD+0xa0, y = HUD+0x78·224 + 224 + HUD+0xa4, on a
512×448 HUD grid; written to every piece slot (RET+0x1ec/+0x1fc). HUD+0x7c = CAM+0x1d0 = 0
always (FUN_0032ace0 l.1697): **no horizontal movement, no target following** except the
lock piece (slot 4) which slides toward the target's screen point HUD+0x90/0x94 by HUD+0x70
(×1.1428572 + 11 and ×0.875 in video mode 0x1e). Vertical, FUN_0032ace0 l.1687-1696:

    a   = −0.4774648 · pitch            (pitch = CAM+0x174, rad; + after right stick up here)
    off = a · c(|a| + 0.3) − 2·CAM+0x198      c(x) = 0 (x<0), 1 (x>1), else 1 − cos(x·π/2)
    HUD+0x78 = off;  CAM+0x1cc = −atan(off · tan(73.5·0.875·½ in rad))   (ray offset, §4)

Traced (aim_d f181-240, pitch 50°): HUD+0x78 = −0.2373; formula −0.2372. So looking up
50° lifts the reticle 53 HUD px; at level pitch it sits ~0.5 px below centre.

### 5.4 Recoil on the reticle and camera — D + T
Camera message 0x3e9 (shot) or 0x3ee (materia) → FUN_0032ffb0 (camera types' slot +0x24):
`if W and !(W+4 bit 0x10): pitch(CAM+0x174) += CAM+0x198`; CAM+0x198 = 0; CAM+0x19c =
W+0x28° (1.0° with no weapon). Per frame (l.1747-1756): CAM+0x198 += (CAM+0x19c − CAM+0x198)
· 0.3 · step; CAM+0x19c −= 0.05°·step while > 0. The view pitch gets +0.8·CAM+0x198·(FOV/
73.5) and the reticle −2·CAM+0x198 (§5.3). So each shot bounces the reticle up and back;
**a second shot before it settles adds the leftover kick to the real pitch** (traced:
aim_a pitch −2.20° → −1.47° at the f64 shot, −1.43° at f100). Reticle y traced 224 → 213 →
224 over ~20 frames after a shot (aim_a f41-66). W+0x28 ≈ 2° for this loadout (from the
decay of CAM+0x19c in aim_e).

### 5.5 Drawing — GS dump (reticle_f200, kind 2, phase 2, variant 0)
21 GS draws (11200-11220) after the post passes, before the life gauge: tri-strip quads,
TEX0 TBP 10792, PSMT4, 128×128, CLUT CBP 10788; ABE (Cs−Cd)·As+Cd; vertex RGB (60,150,60);
vertex alpha 50 (the full-on level: FUN_003b1048 fades with 0x32·t/len), the centre dot 60.
Frame buffer 432×512 drawable; HUD → frame: x·432/512, y·512/448. Centre = HUD (256,224) =
frame (216,256). Pieces (HUD px, relative to the centre; each texture region is reused
rotated in 90° steps by vertex order):

| pieces | size | centre(s) | uv rect |
|---|---|---|---|
| dot | 2×2 | (0,0) | 94-96, 126-128 |
| ring corner arcs ×4 | 31×31 | (±25.5, ±25.5) | 97-128, 0-31 |
| ring straight segments ×4 | 34×13 | (0,±37.5), (±37.5,0) | 0-34, 20-33 |
| arrow spikes ×4 | 31×18 | (0,±42.5), (±42.5,0) | 97-128, 32-50 |
| diagonal ticks ×4 | 19×19 | (±28.5, ±28.5) | 41-60, 0-19 |
| drawn at alpha 0 ×4 | 21×28 at (±21.5,−16); 10×77 at (±5,−5.5) | | 0-21,34-62; 118-128,51-128 |

Screenshot: a green ring with four arrow spikes and four diagonal ticks, ~85 HUD px across.
The pieces come from data, not code: DAT_00472790[kind·3 + variant] → a list of 0x14-byte
records (+0 sprite id/frame → UV via FUN_003cb1f0/268; +4/+6 x/y; +8/+0xa w/h, −1 = sprite
size; +0xc RGBA; +0x10 flags: 1 fade in, 2 pulse 0..32, 4 fade out from 50, 8 late fade,
0x10/0x20/0x40 spin +90°/−90°/360° over the phase, 0x4000 mirror; sprite id 0x43 special in
video modes 2/3). Texture per kind: DAT_004727f0[2k]. The port should export these tables
from the user's ELF (don't copy them into the repo). The on-target / locked variants'
colours and pieces are in the same tables: not dumped (no actors in z128).

## 6. Camera with the weapon drawn, and R3 — D + T

Camera types are cached objects (FUN_00337ca0, DAT_004eed98[type]); FUN_00337338 switches
(CAM+0x48 = object, CAM+0x28 = type, CAM+0x2c = 0 = transition). FUN_0032a420(CAM, mode)
maps view mode (CAM+0x21c) to a type: 1 → 0xc, 2 → 0xd, 3 → 0xe, 4 → 0xf. Constructors
FUN_00330138 / 00330638 / 00330c10 / 003311a8 (FUN_003314e0); enter functions (slot +0xc)
FUN_00330240 / 00330748 / 00330d18 (disassembled) / 003312b0. Runtime values (z128 state;
object +0x10 pivot, +0x20 offset, +0x30 rates, +0x40, +0x50 FOV, +0x54, +0x58 fade target):

| type | pivot | offset | +0x40 | FOV | fade |
|---|---|---|---|---|---|
| 0xc normal | (0, −17, 0) | (0, 1.25, −38) | (0, 4.6, 0) | 73.5 | 128 |
| 0xd weapon | (5, −16.25, 0) (x = CAM+0x1a0) | (0, 0, −35) | (0, 4.05, 0) | 73.5 | 128 |
| 0xe first person | (0, −16.25, −0.1) | (0, 0, −0.1) | (0, 5.8, 0) | 73.5 (= DAT_0046e3f0) | 0 |
| 0xf scope | as 0xe, rates 0.01 | | | W+0x34 (FUN_0032a708) | 0 |

- **Look-at point** CAM+0xf0 = player position + yaw-rotated pivot: traced exactly
  (aim_a f279: pos + right·5 + (0,−16.25,0) = (−1392.07, −114.89, −1114.95), CAM+0xf0
  (−1392.0, −114.9, −1114.9)). So the weapon camera looks past Vincent's right shoulder
  5 units, 16.25 up; Vincent sits left of the reticle (dump screenshot).
- **Transition**: CAM+0x17c ramp += (1.02 − ramp)·0.02·step (0.1 when CAM+0x1e8 bit
  0x100000), clamped to 1; FUN_00260d44 eases it; eye/target lerp old → new; FOV lerps to
  type+0x50; at 1 → CAM+0x2c = 1. Traced every frame: 0.024, 0.048, 0.072, … reaching 1 at
  ~162 frames (aim_a: 0.2 at +9 frames, 0.516 at +29, 0.985 at +139).
- **Fade** CAM+0x1e0 → type+0x58: +4 per frame up, −8 down (0x80 = opaque), written to
  the character via FUN_00342318(fade/128) — traced 128 → 0 in 16 frames on R3, 0 → 128
  at once on leaving first person (the exit snaps).
- **Pitch limits**: +50° / −54° (DAT_0046e3f8 / 0x46e3fc as read from RAM in this state;
  older notes say ±54) unless CAM+0x1e8 bit 0x40000. Traced clamp at 50.0 (aim_d).
- **Right stick**: camera yaw accelerates ~0.135°/frame² to 2.21°/frame (1.84°/tick) at
  full deflection with the weapon drawn (aim_d f31-61). T; the code path (CAM+0x1fc/0x200,
  l.1700-1708, DAT_0046e368/36c) is not fully read.
- **Distance**: the traced eye sits ~30 units from the look-at point (type 0xd says 35,
  0xc says 38), ~17 at 50° pitch; CAM+0x1c0 = 30.1. Collision/pull-in and the pitch
  dependence are **UNDECODED** (FUN_0032ace0 l.2300-2400, CAM+0x1c0/0x1c4/0x1c8).

## 7. Body facing and clips while armed — T (+D for the request path)

- **Facing**: FUN_00562ef8 sets the face point (P+0x120, C+0x670), every tick while drawn
  (§3 AIM). Vincent turns toward it; measured ≈ half the remaining yaw per PAL frame
  (aim_a f11-15: err 42 → 21 → 11 → 5.7 → 3.1). **Decoded (§10)**: rotation mode 6,
  rate 0.5 toward C+0x2b0 − C+0x30. While the camera turns, his yaw leads the camera yaw by
  ~15° (aim point is to the right of the look-at line).
- **Pitch zone** (C+0x590 bits 25-26, FUN_00342be0): aim pitch C+0x680 + C+0x7b0 ≥ 0 → 1
  (level + "u" partner), < 0 → 2 (level + "d"), |·| > 90° → 3. FUN_00345818(C, level, up,
  down, flags) plays the level clip with the up or down clip as its partner
  (FUN_003456f0 → FUN_00344608). The partner weight formula is **UNDECODED**.
- **Clips** (traced): standing drawn h_st_a (+_ad/_au); a shot standing plays h_st_f
  (+_fu/_fd) via the stand-fire classes FUN_00596460 / FUN_00596678 (slots 0x57-0x59, then
  back to 0x40-0x42 h_st_a after the clip or 30 ticks; FUN_00597c60 is the CROUCH fire
  class, slots 0x78-0x7d = h_sq_*; corrected in §10) — h_st_f ran 24 frames (aim_d f61→85,
  f101→125). A second shot 23 frames after the first showed h_st_a, not a restarted h_st_f
  (aim_a f65): not explained (UNDECODED). Walking
  h_wk_a, running h_rn_a (60 u/s, same as unarmed), right strafe h_rrn_a, all keep facing
  the aim; **a shot while moving changes no clip** (aim_c f61/101/191); jumping armed plays
  h_jp0_a → h_jp1_a → h_jp3_a → h_jp4_a (the aimed jump set IS loaded in z128, correcting
  the HANDOFF note), and a shot in the air changes no clip.

## 8. Not decoded / not verified

UNDECODED: partner (up/down) blend weight; camera distance,
collision and eye easing (§6); camera right-stick acceleration code; who sets weapon
state 0xc/0xd and its T = 6; CAM+0x1b4's setter; the auto-lock option source; the
on-target/locked reticle variants (no actors); the eye-kick vector (CAM+0x224 × which
vector) and whether it is visible in story mode; FUN_005449f0's probe classes for the aim
ray (which geometry it hits).

## 9. Translation plan for shoot.js (in order, each with its check)

1. **Weapon config**: W from the loadout (actions-research §5.3) at runtime; check W+0x44
   = 25, W+0x18 = 31 against the trace (CAM+0x1b8 = 310).
2. **Input** FUN_003320c0 (§2): draw on first R1/R3, holster on ○, R3 toggles view mode
   2↔3, fire edge/held by W+4 bit 0. Check: aim_a/b/e/f event frames (state changes on
   the same frames).
3. **Weapon state machine** FUN_00579570 (§3) with the tick accumulator. Check: P+0x2a4 /
   P+0x290 per frame in aim_a, aim_g (25-tick interval, 1-2 decrements per frame).
4. **Camera types + transition** (§6): types 0xc/0xd/0xe values, ramp, look-at pivot,
   fade, pitch clamp. Check CAM+0xf0, CAM+0x17c, CAM+0x1e0 per frame (aim_a, aim_e).
   Distance stays a labelled GUESS until decoded.
5. **Aim ray** FUN_00329778 (§4) through the reticle angle, from S = eye + F·(d+10),
   far = max(1050, range); P+0x2b0 = hit. Check CAM+0xa0 / P+0x2b0 per frame (needs our
   collision ray to hit the same walls).
6. **Reticle**: kind/phase machine (§5.2), position formula (§5.3), recoil (§5.4), pieces
   from the exported DAT_00472790 tables and the TBP 10792 texture (§5.5). Check HUD+0x78
   per frame (aim_d), RET phases (aim_a), and the dump geometry (reticle_f200).
7. **Fire** FUN_00577480 + camera message 0x3e9 (§3, §5.4); bullets as in actions-research.
   Check CAM+0x198/0x19c and the pitch carry-over (aim_a f64).
8. **Clips**: request the whole-body sets by state (h_st_a / h_st_f via FUN_00596460, the
   _a movement sets, h_jp*_a), partner chosen by pitch zone. Facing + partner weight stay
   GUESS-labelled until decoded.
9. **Lock-on** FUN_00328968 once actors exist.

**Remove from shoot.js (no counterpart in the game):** the held RMB/Q aim mode and its
"exit on release"; the GUESS over-the-shoulder camera (dist 20 / height 16.5 / shoulder
6) and its exp easing (use types 0xd/0xe); the camera-look swap to ±54° (game: +50/−54,
all cameras); the 16 rad/s turn and accel 14; the direction-weighted strafe blend (cos/
stride, PHASE, backCap) — the game requests one aimed clip per move direction; the
additive upper-body fire overlay and the j02..j80 mask (moving shots play no fire clip);
"one shot per fire-clip length" (use the state-machine timer); the 45-tick aim hold;
aim/fire weight ramps; the FOV 65→25 zoom (handgun R3 is first person at 73.5, Vincent
faded out); FOV-scaled look speed; bullet speed 15 / range 1500 guesses (W gives 100
u/tick and 10·W+0x18 + 15); the muzzle-flash sprite and the 60-tick spark life (no source
found); the white SVG crosshair (the game's reticle is §5.5, green, kind 2).

## 10. Translation (worker 0077, part 2) — what was added and how it checks

Code (not committed): /srv/games/dirge-port-aim/web (aim_core.js pure logic, aim.js page glue,
player.js hooks, play.js wiring, test_aim.mjs), native/assets/models/export_web_aim.py,
tools/byo/build_web.sh step "weapon stats + reticle tables". Audit rows:
/srv/games/dirge-port-aim/AUDIT-ROWS.md.

New decodes:
- **Turn to aim**: FUN_00578748 with P+0x29c == 3 sets the move (FUN_0034af98) but no turn
  request, and rotation mode 6 (FUN_0034e960(C, 6)). FUN_0034de50 case 6: C+0x2c0 =
  normalize(C+0x2b0 − C+0x30), C+0x2cc = 0.5, then the shared lerp (FUN_00323318). C+0x2b0 =
  the face point (FUN_003424c0 → FUN_0034b5e0) plus C row 0 × 0.25..1.25 by C+0x664 (offset
  left out). Facing row 2 has y = 0 in every trace record (target flattened).
- **Aimed move class FUN_00595440**: angle FUN_00353458 = FUN_00260e74(C+0x20, C+0x90, C+0x10)
  → quadrant FUN_0036a638 (|a| < 30 → 1, ≥ 150 → 3, else > 0 → 2, ≤ 0 → 4) → table idle
  0x40 h_st_a, walk 0x60/0x66/0x63/0x69 (all h_wk_a), run 0x6c h_rn_a / 0x72 h_lrn_a / 0x6f
  h_brn_a / 0x75 h_rrn_a, partners via FUN_00345ca8 / FUN_00345d70, walk threshold 24
  (FUN_00345e38), roots FUN_00345e58 → FUN_00344340 (|Δroot| × 60 / n). Re-installed when the
  quadrant changes; speed 0 → class 6 (aim idle). Slot names: C+0x7fc table in the z128 state
  (0x40 h_st_a … 0x57 h_st_f … 0x78 h_sq_a …).
- **Weapon stats**: FUN_0031c3f8 translated (export_web_aim.py `compose`, PS2 round-toward-zero
  floats); the save's slot R = *(L+0x10) + idx·0xcc (FUN_0031a990, L = *(*(P+0x17c)+0x84)),
  parts at R+0x34 = bgd.bin section 5 #117, #141, #0. The composed W equals the live W byte for
  byte. Live values: 3 pellets, magazine 18, interval 25, range 31 (×10), bullet field 400,
  recoil 2.0°, scope FOV default 25.
- **Reticle tables** (ELF, static): piece record +0 sprite id, +2 frame, +3 copies (s8: n > 0 →
  n copies rotated 2π/n about the centre, < 0 → a mirrored pair), +4/+6 x/y (12-bit), +8/+0xa
  w/h (−1 = sprite size), +0xc RGBA, +0x10 flags; list ends at sprite id 0. Sprite frames
  DAT_0047bfc0[id] → (u, v, w, h). Lengths: show/hide DAT_00472110[k] = 100, normal
  DAT_00472130[k] = 100, variant DAT_00472140[k] = 2 (k = 1..3). Show FUN_003b0680 /
  hide FUN_003b0738 (reverse a running fade). The HUD clock DAT_004f58ec is taken as ms.
- Small ones: FUN_00260d44 = sin(x·π/2) clamped (jumps to FUN_0045b530); the reticle offset
  reads CAM+0x198 before the recoil update (l.1690 vs l.1751); a shot's camera message is
  handled in the same frame (pitch carry-over shows at once); leaving first person finishes
  the transition in one frame (traced; branch not located).

**Reticle texture: DECODED (worker 0100, 2026-10-28).** 'menu2' (PTR_DAT_0047d500[7]) is a
record in data/etc/menudata.bin (name 8 bytes, 'tm2', size, then a 'TIM4' file; identical in every
locale copy). Picture header: PSMT4 128×128, 16-colour CT32 CLUT, GsTex0 TBW 2. The image region
(imageSize 0x2080) and the CLUT region (0xc0) each START with a zeroed slot: the texels are the last
0x2000 bytes (0x22c0 in the file), the colours the last 0x40 bytes (0x4340). Worker 0077 read from
the region start (0x2240), 0x80 bytes early; that offset, not the swizzle, scrambled the image.
The texel bytes are the GS transfer image: GS dump reticle_f200 uploads them verbatim as PSMCT32
64×32 (DBW 1) to TBP 10792 and the CLUT as PSMCT32 8×2 to CBP 10788 at seq 53325, right before the
21 green draws (TBP 10792 is a streaming slot: a wall texture sits there earlier in the frame, which
is what the earlier attempt saw). Read back as PSMT4 TBW 2 = write CT32 / read T4 with the GS tables
(`dtex_unswizzle4` gives the same image). CLUT: RGB white, alpha 0x02..0x80 ramp. Draw state at the
reticle: TEX1_1 = 0 (nearest), CLAMP_1 = 0 (repeat), TEXA default, TEX0 TCC 1 TFX 0 (modulate:
Cv = Cf·Ct >> 7 → (60,150,60) gives (119,255,119); Av = Af·At >> 7), ABE (Cs−Cd)·As+Cd, TEST ZTE
GEQUAL, no alpha test. UVs (FUN_003b1048 l.185-190, via FUN_003cb1f0 / FUN_003cb268): u0 = frame
u·16, u1 = (u+w)·16, v likewise (12.4 FST); vertices (x0,y0,u0,v0) (x1,y0,u1,v0) (x0,y1,u0,v1)
(x1,y1,u1,v1), rotated or mirrored with their positions (dump: the mirrored pair keeps UVs per
vertex). Flag 0x4000 mirrors u1 −8; flags 0x2000 / 0x80000 recolour (0x4188c7 / 0x404040) and switch
texture (FUN_003dd4a8(9)); no kind 1-3 piece uses them. Method: the dump's freeze block holds the
4 MB GS memory at +0x1a9 (registers end at +0x13c; checked by decoding the display buffer against
the embedded screenshot); scripts in worker 0100's scratchpad (gsmem.py, rtex.py). The loader past
FUN_003d2b80 (FUN_00259018 / FUN_003d2dc0) is not decoded; the geometry comes from the file's
GsTex0 and the dump's BITBLTBUF / TRXREG / TEX0.

Checks:
- `node test_aim.mjs` (in the copy): all 7 traces, every frame: weapon state / timer /
  accumulator, drawn flag, camera type / phase / ramp / fade, HUD mode, HUD+0x78, CAM+0x198,
  reticle kind / phase — all match; the turn law within 0.27° per frame; the aimed run clip by
  quadrant matches every unblocked run frame. Mutations (interval +1, ramp rate 0.03, reticle
  constant, turn rate 0.4, no holster) each fail it.
- Reticle quads vs GS dump reticle_f200: 21/21, worst corner 0.078 HUD px, alpha and colour
  exact. Image: /srv/shared/dirge-port/review/aim-reticle-compare.png.
- Headless page (Chromium): loads with no errors or warnings; walk / run / turn and the ladder
  work; draw → camera 0xd, h_st_a, reticle; shot → h_st_f and the recoil; aimed walk / run /
  strafe clips; R3 first person (Vincent hidden) and back; holster.
