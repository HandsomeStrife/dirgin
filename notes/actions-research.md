# Actions research: jump, ladder, shooting (worker 0057, 2026-10-26)

Research only, no code changed. Sources: analysis/decomp/*.c and elf/SLES_541.85 (PAL
build). Data reads used a scratch reader over the ELF's PT_LOAD segments. Confidence:
**D** = decoded (read in code, value read from the ELF/data), **L** = likely (code read,
meaning inferred), **G** = guess. World is −Y up, so "up" means negative y.

## 0. Time base (answers "60 Hz or 30 Hz?")

| item | value | where | conf |
|---|---|---|---|
| logic unit | 1 tick = 1/60 s; variable step | FUN_00322f88: `DAT_004eeb24` = step in ticks (cap 5.0), `DAT_004eeb28` = step × 0.016666668 s | D |
| step per vblank | 1.0 tick (NTSC), **1.2 ticks (PAL, video mode 2)** | FUN_0040ce28: step = vblanks since last flip × scale; FUN_0040cb78: scale × 1.1999999 when mode 2 (PAL: GS CRT mode 3, 512 lines, FUN_0040c078) | D |
| PAL frame rate | not fixed: 50 fps → 1.2 ticks per frame, 25 fps → 2.4 | as above | D (mechanism) / unmeasured |
| clip playback | 1 clip frame per tick | ladder code turns clip length into seconds as `(frames − 2) / 60` (FUN_00599f10); the h_jp timing fits the jump arc (§1) | L |
| velocities | stored per tick; inputs given per second and × 1/60 | jump: `speed × 0.016666668` (FUN_00575768) | D |

So "1 clip frame per tick" holds if the port runs 60 ticks/s. Our SLES build at 50 Hz
takes 1.2-tick steps; it is not a 50 Hz rule set. play.js's fixed 60 Hz tick matches the
game's units. The motion-player advance (`frame += rate × byte/128 × step`, FUN_005aa2a8)
looked like this, but that function may serve another animation type (texanm): G.

## 1. Jump

| item | value | where | conf |
|---|---|---|---|
| exists? | yes; player state 8 (0x20 = a variant chosen by an angle test FUN_0036b058 when motion slot 0xdb exists). Pressing again while in states 8/9/0x20 gives a **second, mid-air jump**: state 0x1f/0x21, char+0x79c bit 0x20, motion c_wjp0–2, gated by FUN_00575698 | FUN_00575768, FUN_00598f50 | D (code) / L (that it is a double jump) |
| button | logical id 0xd = **✕** in non-Japanese builds (○ in Japan; ids 0xc/0xd swap by region, FUN_00255f08). Same button as "use ladder"; ladder wins when a ladder trigger is in front | FUN_00333150 (id 0xd → ladder check, else FUN_00575768(player,0)) | D (code) / L (which region code our PAL disc reports) |
| take-off speed | **80 units/s up** = 1.333 u/tick (`DAT_0046ee38`); variant 1: 112 u/s (`DAT_0046ee3c`, used by FUN_00575768(p,1): high jump) | FUN_00575768 | D |
| horizontal at take-off | current ground velocity is kept: (char+0x90 move dir in world) × char+0x1d8 (ground speed, u/s); combined with the vertical part, normalised, magnitude × 1/60 | FUN_00575768 → FUN_0034b2c8 | D |
| gravity | **0.07 u/tick²** downward (+y): vector `(0, 0.07, 0)` at 0x46f490, added once per tick sub-step | FUN_003543b8 (movement mode 0xb, airborne) | D |
| terminal speed | none found in mode 0xb | FUN_003543b8 | L |
| air control | none: the airborne step reads no input; horizontal speed is fixed at take-off | FUN_003543b8 | L |
| apex | 1.333/0.07 = 19 ticks, height 12.7 units (high jump: 26.7 ticks, 24.9 u) | derived | D |
| animation | h_jp0 (3 f) → after clip end or 10 ticks, h_jp1 (10) + h_jp2 (10) queued → h_jp3 (16, loops) until the on-ground bit → h_jp4 (15, land). Aiming variants h_jp*_a/_au/_ad; wall/back variants c_wjp*, c_bj*, c_bwj* | FUN_00598f50 (state class vtable 0x5a5b78), FUN_00599820/599aa0 | D |
| landing | per sub-step a vertical probe (object probe index 5) along this step's dy; a hit nearer than the step sets char+0x1c0 bit 0x200 (on ground) and switches movement mode | FUN_003543b8 | D |
| air wall hit | horizontal probe index 4, length 7 + 1 = 8 (`DAT_0046f428` = 7.0 = body radius) at the offsets below; on a hit the horizontal velocity is scaled by −0.25 | FUN_003543b8 | L |
| fall damage | not found (did not search beyond the jump code) | — | — |
| other path | when script flag `DAT_0046e298` bit 13 is set, the jump instead uses a physics body: gravity = cfg(+0xcc) × 196 u/s² (cfg+0xcc forced to 1.2 → 235 u/s² = 0.065 u/tick²) and a jump HEIGHT cfg(+0xc4) + 1 (cfg = `*(DAT_0046dd68+0x14)`, runtime) | FUN_00575768, FUN_00356358, FUN_003590c8 | D (code) / G (when the flag is on; assume off) |

Animation cross-check: h_jp0 + h_jp1 = 13 frames of rising, h_jp2 covers frames 13–23, and
the physics apex falls at tick 19, so clip frames and ticks agree at 1:1.

**play.js now:** gravity 0.05 u/tick², cap 6, not from the game. Game: **0.07, no cap
found**, take-off −1.333 u/tick (y), no air steering.

## 2. Ground movement, steps and slopes (0055's open question)

Normal walking is movement mode 10, FUN_003539b0 (mode set via FUN_0034db80(char,10)).

| item | value | where | conf |
|---|---|---|---|
| ground probe | vertical segment from 17 above the feet (`(0,−17,0)` at 0x46f430) to 5 below (`(0,5,0)` at 0x46f460), object probe index 5 | FUN_003539b0 | D |
| step up | ground found > **7.5** above the feet (`DAT_0046f42c`) is refused | FUN_003539b0 l.427 | D |
| follow | Δy smaller than this tick's move: snap; otherwise lerp **0.2** toward the hit | FUN_003539b0 | D |
| step down / ledge | no hit within 5 below → on-ground bit 0x200 cleared → falls (mode 0xb) | FUN_003539b0 | D |
| slope limit | **no angle test in mode 10**: any surface the probe hits is ground. The 40° rule (0.69813174) is only in FUN_0035a440, the physics body used for riding objects/primitives and by the bit-13 path | FUN_0035a440, FUN_003539b0 | D |
| wall probe | horizontal, index 4, length (speed + 7.0) at heights from the 0x46f430.. vectors (−17; −18/−19 also in that table) | FUN_003539b0, FUN_003543b8 | D (exists) / L (heights) |
| probe flags | object probes go through the object list's vtable +0x17c (FUN_0054fc38); indices 4/5 use **0x18e (class 0x01)**, or **0x18d (class 0x02) when `DAT_0046e298` bit 0 is set** | FUN_0054fc38 | D |
| physics-body step (FUN_0035a440 / FUN_003590c8) | within 3.5 of the ground: follow at most 0.3 per call; re-check ground 8.5 below | FUN_0035a440 end, FUN_003590c8 | D |

So the 47–53° ramps are walkable in the game's walk mode as long as each step rises
< 7.5. Item 4 of the brief: there IS a horizontal wall probe in the walk and air modes
(index 4 via +0x17c). Against what: in z128 only **10 sub-tree-1 triangles** have class bit
0x01, and they are the ladder faces (§3), so z128 must run with script flag bit 0 set,
making walls/floors class **0x02** (113,161 tris). G: that flag is set for z128 (not
traced to the script).

## 3. Ladders

Data: **06800001.bin** (header "bzd048", the z128 stage file loaded at
`*(DAT_0046dd68+0x34)`), section table at +0x24 (16 B each: +0 offset, +4 count, +8 stride);
section **8** = gimmick records (219 × 0xC0 at 0xF8260). Record: +0x00 4×4 matrix (row
vectors, row 3 = world origin), +0x80 local box min, +0x90 local box max, +0xa4 flags,
+0xa8 id, **+0xaa type (1 = ladder bottom trigger, 2 = ladder top trigger)**, 8 = other,
10 = 45 records. Reader: FUN_00310b00/FUN_003110b8; actors built by FUN_00547f50.

Each record holds a trigger floor point (the origin), a box rising in −y, and an orientation whose row 2 (local +z) points to the ladder side. The ladder next to the start deck is the bottom/top pair 35/34, about 290 units east of the start. Its ladder face (class 0x01) is the plane x ≈ −1106, rising about 57 units (≈12.6 rungs). z128 has three more ladder pairs in section 8, and two class-0x01 faces with no section-8 record. `native/assets/models/export_web_ladders.py` reads all of them from the game file at export time, so the per-record values are not kept in these notes.

| item | value | where | conf |
|---|---|---|---|
| trigger | ✕ (id 0xd); 7-unit ray along the facing hits a gimmick box (FUN_00544200); type 1 → mount from bottom, type 2 → from top. Needs player state 2–5 | FUN_00333150, FUN_00576c88/576de8 | D |
| snap (bottom) | segment in ladder-local space (0, min.y − 7, max.z − 5) → (0, min.y − 7, max.z + 5), cast with 0x18e (class 0x01). Hit → snap = hit point, n = hit normal. Miss → snap = local (0, 0, max.z), n = −row 2 | FUN_00551e80 | D |
| snap (top) | same at (0, max.y + 7, max.z ± 5) | FUN_00552088 | D |
| mount pose | facing = −n; target = snap + n̂ × **5.6** + (0, −0.1, 0) (Vincent; other characters use 5.3/−0.2 or 3.0/+0.4, table at 0x46ef94..efbc keyed by char+0x664) | FUN_0059a5d0 | D (values) / L (which row is Vincent: the default) |
| mount motion | c_ld_u (24 f); position lerps to the target over half the clip; then idle c_ld_lid (150 f) | FUN_0059a5d0 | D |
| climb | **rung steps of 4.5 units**: y = base − 4.5 × rung (base char+0x720, rung char+0x740); each step plays c_ld_umr/uml (up) or c_ld_dml/dmr (down), alternating hands, moving over (16 − 2)/60 s = 14 ticks → **0.32 u/tick**. A step chains into the next when its clip ends with the stick still held; otherwise it settles after 32 ticks | FUN_00599f10 | D |
| input | stick y < −0.3 = up, > 0.3 = down, in between = idle (state 10 = on ladder) | FUN_00333150 → FUN_00576f08/576f98/577038 | D |
| top exit | before stepping up: segment at feet − 20.25, from −5 to +5 along the facing, class 0x01; no hit → state 0xd: c_ld_uel/uer (20 f) | FUN_00346fc0, FUN_0059b208 | D |
| bottom exit | before stepping down: segment from 5.5 behind, down 8.25, class 0x01; hit → state 0xe: c_ld_der/del (12 f) | FUN_003470d8, FUN_0059b648 | D |
| jump off | ✕ while on the ladder → FUN_00562d30, c_ld_jp (5 f) then c_ld_fa0 | FUN_00333150 | L |

Caveat: the snap uses the hit normal from a two-sided face, so its sign is not settled.
Use the side the bottom trigger box is on (rec 35: west of x = −1106, facing +x).

## 4. Shooting (partial: stopped after two attempts on the fire/projectile chain)

| item | value | where | conf |
|---|---|---|---|
| buttons (logical ids) | 9 = R1, 0x1f = L1 (tested in complementary pairs: aim/fire, swapped by control type), 0xa = R2, 0x10 = □, 0xc = ○ (melee, state 0x80 + combo, c_atk_*), 0x11 = R3 | pad table 0x46bae0 (6 B: u16 mask in swapped libpad order, s8 modifier, u8, u8 id), FUN_003320c0 | D (table) / G (which of R1/L1 aims) |
| shots | moving projectile actors (factory type 6, vtable 0x58d458), not hitscan: each tick sweeps previous → current position | FUN_00556870 → FUN_005442d8 | D |
| hit test | object probe index 0 = flags 0x18d → **class 0x02** (the visible-geometry set), plus actors | FUN_005442d8 → +0x17c | D |
| camera pitch limits | **±54°** (`DAT_0046e3f8` 54, `0x46e3fc` −54) in the normal camera; a per-camera set (+0x204..0x210) applies when camera flag 0x40000 is set (aim camera?, not traced) | FUN_0032ace0 | D / G for aim |
| FOV | FOV × 0.875 / × 1.0 (normal) and × 1.1429 (flag 0x1e) from the table at 0x46e360..; base 65 at 0x46e3f0 | FUN_0032ace0 | G |
| fire rate, spread, range, damage, reload, muzzle flash | not found. Likely a weapon-parts data table (Cerberus barrels), not code literals | — | — |
| aim/reload motions | h_st_a / _au / _ad (60 f; up/down blend), h_st_f/_fu/_fd (15 f, fire), h_st_rd (40 f, reload) | 0770000b | D |
| gun attach | marker 15 → bone 56 (see HANDOFF "Cerberus gun") | FUN_00316270 / FUN_005ab8f0 | D (earlier work) |

## Leads not followed
- Who sets `DAT_0046e298` bits 0 and 13 (script natives around FUN_00326660); decides
  class 0x01 vs 0x02 for the walk probes and the jump path.
- Weapon stats: the fire path from FUN_00574d30 / FUN_00574f40 / FUN_0056b530 to the type-6
  spawn; the parts data.
- FUN_00562d30 (ladder jump-off) speeds; c_wjp / c_bj triggers.

## 5. Weapon data (worker 0064, 2026-10-26)

Research only, no code changed. Sources: analysis/decomp/*.c, the ELF (PT_LOAD reader),
`data/bgd/bgd.bin` (KEL id 0x03100031, header "bgd020") and the z128 stage file
06800001.bin. Units: 1 tick = 1/60 s (§0). Confidence D / L / G as above.

### 5.1 Cerberus default handgun (the values to use)

| item | value | where | conf |
|---|---|---|---|
| fire mode | **semi-auto**: one shot per R1 press (W+4 bit 0 clear; when set, holding fires) | FUN_003320c0 (id 9 held vs pressed by W+4 & 1) | D |
| first-shot delay | 0 ticks (W+0x42) | FUN_00574a80 sets timer = W+0x42 | D |
| fire interval | **38 ticks** (0.63 s) between shots (W+0x44 = 30 + 8) | FUN_00579570 state 1→2, timer = W+0x44 | D |
| bullet speed | **100 u/tick** (6000 u/s) = W+0x2c / 6 (W+0x2c = 600) | FUN_00552dd8 (+0x12c), FUN_00556068 moves `start + dir·age·v`; the lead-aim maths in FUN_00577480 uses W+0x2c×10 u/s | D |
| range | **565 u** = W+0x18 × 10 + 15 (W+0x18 = 55); bullet stops at that distance. +20 instead of +15 when FUN_00321e20 is true (a weapon-flag/difficulty test) | FUN_00552dd8 (+0x15c, constants 0x46ed6c/70 = 15/20), FUN_00556068 | D / G for which constant |
| spread | **none in the shot**: each bullet flies from the muzzle to the aim point (char+0x2b0); no rand call in the fire path. W+0x1c/0x20/0x24/0x28 (composed 1.0 / 0.5 / 0.1 / 0.7) are spread-like but no reader was found | FUN_00577480, FUN_00552dd8 | D (no random spread) / G (those fields) |
| pellets | 1 (W+0x16) | FUN_00577480 loop | D |
| damage | 85 (W+0x40); W+0x4c is a % bonus (0 here) | FUN_00552dd8 → bullet+0x134; FUN_0031c3f8 | D (value) / L (meaning) |
| magazine | **9** (W+0x48); loaded rounds sit in the slot record R+0x30, so the cap is R+0x94 = W+0x48 | FUN_0031a1f8, FUN_00319f80 | D |
| reload | **80 ticks** (1.33 s) (W+0x46); refill = min(reserve, 9); allowed when loaded < 9 and reserve > 0. Button id 0xa (R2) → FUN_00574f40 → state 7 | FUN_00579570 state 7, FUN_00319f80 | D (timer) / L (R2 by 0057's pad table) |
| scope FOV | **25°** (W+0x34; parts give the minimum, default 25 when no part sets one; the rifle scope part gives 6). Base FOV 65 | FUN_0031c3f8; FUN_0032a708 compares camera+0x50 (FOV) with W+0x34 in camera mode 4 | L |
| gunshot sound | `bsj_051` for class 1 (class 2: bsj_053, class 3: bsj_052; W+4 bit 3: bsj_061), played at the muzzle | FUN_0033d118 → FUN_0033cc48; dry-fire click is bsj_015/017 through the same call, so these are sound cues | D |
| muzzle point | marker 15 (gun, bone 56); fallback local (0, 16, 10) | FUN_00341ec8 | D |
| muzzle flash | **not found** (two attempts). FUN_00335338 after the shot looks like a noise/alert broadcast, not an effect | — | — |
| camera kick | class 1: FUN_0032a320(0.75, 0.01) per shot (table at 0x46eee0, pairs per class) | FUN_00577480 | D (call) / G (meaning: amplitude, time) |
| air shots | firing in mid-air gives a small upward hop: height 5 × (W+0x42 + W+0x44 + 2)/60 = 3.3 u (cap 12), at most **3 per jump** for class 1 (per-class caps at 0x46ee90: 0, 3, 12, 2, 0, 3) | FUN_00577d40 → FUN_00357168 (−sqrt(2gh) take-off), counter char+0x270 | L |

### 5.2 Buttons (logical ids; physical keys from 0057's pad table)
- **Fire = id 9 (R1)**. There is no hold-to-aim button: the shot always goes to the
  reticle's aim point. **Materia (magic) shot = id 0x1f (L1)** (FUN_00574c10 → state 5,
  FUN_00577e68). Pressing R1 and L1 together → FUN_005728d8 (beast/limit transform, "bsj_094"). D (code) / L (physical keys; the
  control type swaps 9 and 0x1f).
- **R3 (id 0x11) toggles the zoom camera**: FUN_0032a420(cam, 2) on, FUN_0032a390(cam, 0)
  off; camera mode 4 takes its FOV from W+0x34. Offset of the zoom camera not traced. L.
- For the port: RMB/Q "aim" ≈ the R3 zoom (FOV 65 → 25); LMB = R1.

### 5.3 Where the numbers live (layout; no raw tables copied)
- **Weapon stats W** (0x78 B) = the sum of up to 6 installed part records, built by
  FUN_0031c3f8 into the runtime weapon-slot record R (0xcc B, list via FUN_0031a990) at
  R+0x4c. Offsets used: +4 flags (bit 0 auto, bit 3 alt sound, 0x10000/0x20000 special),
  +0x10 top nibble = materia type, +0x14 bits 0–1 materia level / bits 2–4 **weapon class**
  (1 handgun, 2 automatic, 3 rifle; set by the slot-0 part) / bits 5–6 (set by the slot-1 part),
  +0x16 pellets, +0x18 range/10, +0x2c speed/10, +0x34 FOV (min), +0x3c homing deg,
  +0x40 damage, +0x42 first delay, +0x44 interval, +0x46 reload, +0x48 magazine,
  +0x4a hit value (→ bullet+0x153), +0x4c damage %, +0x4e interval-reduction %,
  +0x50..+0x62 % modifiers for the floats. Floor rules: pellets ≥ 1, magazine ≥ 1, range field ≤ 1000.
- **Part records**: bgd.bin section 5 (250 × 0x78, same layout as W). bgd.bin has a section
  table at +0x24 (16 B: offset, count, stride), as in the stage files. Items: section 0
  (0x18 B: +0 item id, +4 section-5 index, +0xc kind; 3 = weapon part), found through
  section 3 (category = item id high 16 bits, start, count). Part slot = section-5 record
  +0x12 & 0xf; item category 0x6f3N = slot N.
- **Default loadout**: bgd section 13 (5 × 0x34; first 3 used = the 3 weapon slots), 6
  entries of 8 B, +4 = index into section 12 (8 B, +0 item id); entry 5 is the ammo item.
  Read by FUN_0031b690. **Handgun slot = parts #20 (item 0x6f300000) + #52 (0x6f310000), ammo
  0x62300000.** Sections 6/7 are a second set (parts 0x7030…), probably another mode: G.
- **Enemy weapons** are separate: stage file section 24 (z128: 28 × 0x104; the first 0x78 B
  are copied into W by FUN_0031a2f0, chosen by R+0x32).
- **Materia shots** use bgd section 11 (23 × 0x6c bullet records), picked by (materia
  type 1–6, level 1–3) through bytes at section 4 (+0x313 / +0x343). Cost byte +6 comes off
  weapon+0x24 (MP). Not needed for the handgun.

### 5.4 Leads not followed
- Muzzle flash effect: try the fire motion h_st_f's event track, or FUN_003359d8(…, 0x42c00).
- How the aim point char+0x2b0 is built (any reticle bloom/spread; likely reads W+0x1c..0x28).
- Zoom-camera offset (camera modes 2 and 4 in FUN_0032ace0).
