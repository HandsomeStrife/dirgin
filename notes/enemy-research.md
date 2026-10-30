# Enemy research map (worker 0114, 2026-10-29)

Research only. No code changed. Sources: analysis/decomp/*.c, elf/SLES_541.85 (vtables read
from the ELF), the z128 stage file `extract/_KEL/noPath/06800001.bin` ("bzd048"), the
sea-of-flames save state (EE RAM read offline with tools/trace/p2s.py, no emulator run) and
the three z128 GS dumps. Confidence: **D** decoded from code/data, **L** likely, **G** guess.
No tables are copied here; exporters should read the game files at export time.

## 0. Summary

- z128's enemies are **placed by the stage file**, section 1 (221 records). Each record
  names a parameter set in section 0 (model name, HP, weapon). The common soldier is
  **e027**, then e048 and e030. All 221 actor objects exist from stage load, but only
  actors that have been *activated* get a body and an AI (FUN_00559748).
- At the save point, **7 enemies are active**: six e027 soldiers 350–560 units east of the
  start deck (x −1039..−901, z −1100..−839) and one at (−1427, −121, −743). All are in
  AI state 0 playing their idle clips `r_gst` / `r_gst_lk`. The three soldiers placed on the
  start deck (records 41–43) are already dead (HP 0).
- Enemy models use the same container family as Vincent (skeleton + smd + rst texture),
  but **the current decoder can't read e027's draw packets** (1 triangle of 1394 vertices).
  This is the first blocker for anything visible.
- e027's texture is uploaded in all three z128 GS dumps and drawn in the hero frame, so
  the dumps can be the oracle for the e027 decode.
- AI = an AI object per actor (actor+0x184) with three swappable behaviour layers, run by a
  tick scheduler. The per-type state machines are vtable classes picked by a mode number.
- Damage: bullet → FUN_005442d8 → target vtable +0x1b4 (FUN_0055db50) → damage formula
  FUN_0055d2c0 → vtable +0x1ec FUN_0055c7f8 (HP −= dmg) → hit reaction +0x324, death +0x334.

## 1. Spawning (stage file 06800001.bin)

Section table at +0x24 (16 B: offset, count, stride). Accessors: FUN_00310b00(n) returns
the descriptor; one getter per section (FUN_00310c80 = section 1, FUN_00311600 = 24, …, see
the `FUN_00310b00(0xNN)` calls in 003108c0–00311a18). The live copy is `*(DAT_0046dd68+0x34)`.
Section 0 is also copied to `*(DAT_0046dd68+0x38)` (FUN_00310bb8).

| sec | count × stride | what | reader | conf |
|---|---|---|---|---|
| 0 | 57 × 0x20c | **parameter sets**: +0x20 4-char model name (e001, e023, e024, e027, e030, e036, e048, n0xx NPCs, p000, p001, g001), +0x30 u16 **max HP**, +0x34/+0x38 floats (×10 into status +0x74/+0x70), +0x44 weapon name (w003/w004), +0x68 low nibble → status +0x26, +0x84 → section 19 index, +0x92 bit 2 → +0xa0/+0xa4 floats | FUN_003153d0, FUN_00315050, FUN_00317140 | D (fields used) / G (meaning of the floats) |
| 1 | 221 × 0x74 | **actor placements**: +0 section-0 index, +4 u16 group id, +8 **AI mode** (FUN_0035f300), +0xc pos xyz, +0x1c facing xyz, +0x2c..0x34 vec, +0x40 flags, +0x44 tag string ("e_Sol_11", "e_DG_111", "e_ExH_10"…), +0x4c bytes → AI +0x98/+0x99, +0x50 HP override (when FUN_00315738(…,0x14)), +0x58..+0x60 item id + count (drops?) | FUN_00318f48 (loop at stage start) → FUN_00318d48 → FUN_00314ad8 → FUN_00315050 | D (loop, +0/+8/+0xc/+0x50) / G (+4 group, drops) |
| 4 | 1 × 0x60 | first u16 → FUN_00318c50 after the loop (player slot?) | FUN_00318f48 | G |
| 13 | 35 × 0x10 | model name list (e001, e023, e024, …) | — | L |
| 14 | 54 × 0x48 | per-model records starting with a name (e056 …) | — | G |
| 19/20 | 29 × 4 / 29 × 0x620 | indexed by section-0 +0x84 (status +0xa8 via FUN_00310e40) | FUN_003153d0 | D (link) / G (content: AI parameters?) |
| 21 | 1 × 0x108 | resident list: (name, max count) pairs, e.g. e027 ×3, e030 ×3, e048 ×2, p000 ×15, w003/w004/w007 | — | L |
| 24 | 28 × 0x104 | enemy weapons (first 0x78 B = weapon stats W) | FUN_00311600 / FUN_0031a2f0 (actions-research §5.3) | D |

z128 section 1 holds 126 e027, 27 e048, 23 e030, 2 e023 (HP 22800/30000: bosses or
set-pieces), 1 each of e001, e024, e036, and NPCs (n0xx). Records 41–43 (e_Sol_11..13) stand
on the start deck; records 69–71 (e_Sol_a1..a3) stand 35–63 units away, one level down (y 0).

**Activation (worker 0116, 2026-10-29).** Every placement gets an actor object at load
(object table `DAT_0046e880`+0x1164, class vtable **0x58e1f8**, constructor FUN_00558790;
status in the 256-slot table `DAT_0046e200`+0xc). Body (+0xe8) and AI (+0x184) appear only
after **vtable +0x144 = FUN_00559748** (param_2 = on/off). Story-mode callers:

| path | when | rule | conf |
|---|---|---|---|
| FUN_00324e70 → FUN_00319190 → FUN_003159b8 | stage load | for each status whose placement flags (status+0xc = section-1 **+0x40**) have **bit 3** (FUN_00315738(s,3)) and whose model resource is found (FUN_00376938): load the model, set status+0x60 bits 0x1 and **0x40** | D |
| FUN_00324e70 → FUN_005453b0 → vtable +0x36c = FUN_00558f68 | right after, stage start | for each of 256 slots with a status and (actor+0x28 bit 0x40 or status+0x60 bit 0): if status+0x60 bit 0x40 and actor not active (+0x28 bit 0) → **+0x144(1,1)**, clear 0x40 | D |
| FUN_005453b0 → FUN_005478d0 → vtable +0x254 = FUN_0056a930 | same pass | re-activate actors marked "was active" (actor+0x40 bit 0x200, set by FUN_0054b610 from +0x24c = FUN_0056a838 when an active actor is suspended) | D |
| object script command handler FUN_0054cd70 (enemy +0xc FUN_005661e8 falls through to it via FUN_00588b78) | event script | cmd **0x108** = activate (+0x144(1,0)), **0x109 / 0x130** = deactivate | D |
| FUN_005453b0 | stage start | no status and actor+0x118 bit 0x400 → deactivate | D |
| FUN_00541c98 (activate list +0x1128 if byte +0x25 < 0 or its anchor is active) | `KerberosNetLobby.onRoundInit` (FUN_0039d758, name table 0x470720, names XOR 0xab) | online mode only | D |

z128: 59 of 221 placements have +0x40 bit 3 ("appear at stage load"), incl. Vincent (rec 0)
and the start-deck soldiers 41–43. The save's 7 live e027s (records 44–49, 66) do **not**
have bit 3, so they were activated mid-stage by an event script (cmd 0x108). Which script
event and its trigger condition (area, flag, group id +4) live in the section-3 bytecode run
by FUN_00545b20 (~100 opcodes): **not decoded**. Other +0x144 callers (FUN_0053c140,
0053d610, 00540b90, 00540c88, 00541278, 005405b0, 00541688, 00541890, 00518838, 0055ff80)
create/activate other object types (FUN_0053db98 type 5/6/7/0xd/0x12/0x15) or spawn by
name; not checked against enemies.

## 2. Assets

Character files are named by FUN_00249cb8: id = `0x07000000 | letter<<16 | number<<6 | k`
("e027" → 0x076506c0). Same family as Vincent (0770 + k):

| k | e027 file | content |
|---|---|---|
| 0 / 0xa | 076506c0 / 076506ca | m000 (melee? `m_*` clips) |
| 1 | **076506c1** | models.rfd: skeleton + smd (`e027_L01`, `e027_S01`, …) |
| 2 | **076506c2** | texture.txd (shared by the soldier family: same bytes as 07651182 etc.) |
| 3 | 076506c3 | unknown (21 KB) |
| 0xb | **076506cb** | m001 = rifle clips `r_*`, incl. the idle `r_gst` / `r_gst_lk*` |
| 0xd..0x11 | 076506cd..d1 | m003..m007 (r_para* layers, c_jz*, …) |

e030 = 07650780.., e048 = 07650c00.., e023 = 076505c0.., e036 = 07650900..
(`data/chr/e/0/X/Y` in extract/_KEL/file.txt; several enemies share texture/motion files of
identical size, so check the save state's loaded resources before trusting a size match,
as was needed for Vincent's 077032xx vs 077000xx).

**Exporters:**
- `smd_material.py --render 076506c1.bin 076506c2.bin OUT e027` (dump-free materials)
  runs, but `render_skinned.assemble` builds **1 triangle**; 11 groups decode 0 triangles.
  The vertices (1394, bbox ±6.9 × −6.4..11.7) are fine, the index/strip association is not.
  notes/session-2026-10-15-geometry.md §9–10 already describes this layout ("headers
  separated from the V1-8 index unpacks"; greedy num==count association, 279/316 containers
  exact), but that code (scratchpad robust5/6.py) was never committed and is gone.
- export_web_vincent.py is Vincent-specific (clip sets, cape, dump binding); the clip
  decoder (anim_rot.py) and smd_material.py should apply as is once the mesh decodes.
- **GS dumps:** e027's texture is uploaded in hero 163235 (TBP 0x3a04 and 0x2c48),
  walk 185126 and 185138. In the hero dump 840 strip draws on TBP 0x2c48 (CLUT 0x2c44) go
  to the scene framebuffer (FBP 0xe0), screen rows ~167–229 of 512: soldiers in the
  distance. Usable as a check for the mesh decode (UV triples, as for Vincent). Not yet
  confirmed which enemy model those draws are (the texture is shared).

## 3. Logic

**Objects.** Actor (vtable 0x58e1f8, player 0x58f128) → +0xe8 body C (same layout as the
player's: C+0x00 matrix, C+0x30 position, C+0x1c0 flags, C+0x570 motion name), +0x17c status
(+0x04 max HP, +0x20 HP, +0x9c section-0 record, +0xa0 section-1 record), +0x184 AI,
+0x19c hit-zone record, +0x118 flags. 42 vtable slots differ from the player's; listed by
diffing the two vtables (script in the worker 0114 scratchpad).

**AI object** (0x110 B, FUN_0035e5d0, attached by FUN_00365c50 from FUN_00559748):
+0x14 flags, +0x18 next think time, +0x20 sub-state, +0x24 state, +0x34/+0x38/+0x3c/+0x40
floats (distances/angles to the target; read by the brains), +0xd0 mode,
+0xe0 brain, +0xe4 move layer, +0xe8 third layer, +0xf0 owner actor.

| part | function | conf |
|---|---|---|
| scheduler | FUN_00365008 (called from FUN_00323128): accumulates step ticks; runs FUN_00365110 for every AI whose due time (+0x18) has passed, re-queues it (+0x20 interval) | D |
| per-AI tick | FUN_00365110: brain vtable +0x1c, then +0xe4 and +0xe8 layers' +0x1c; brain flag bit 0 → FUN_0035f268 switches mode to brain+0x14 | D |
| brain by mode | FUN_0035f300: 0/other → FUN_003613b0 (vt 0x4a2430), 1 → 362868, 2 → 362e58, 3 → 3615b8 (vt 0x4a2510, **update FUN_00361690**), 4 → 3641c0, 5 → 3638f8, 6 → 3644b8, 7 → 3632b0, 8 → 363720, 9 → 518a58 (**FUN_00518b30** family, gated by FUN_00326500), 0x100–0x1ff → script-defined (DAT_0046e590) | D |
| move layer by mode | FUN_0035f590: 1..7 → FUN_00366160 + vtables 0x4a2a28..0x4a2938 (mode 3 = **FUN_003667b0**) | D |
| default brain update | vtable 0x4a2430 +0x1c = **FUN_003614b0 = `jr ra; nop`: empty.** Function created in ghidra-proj by worker 0117 (analysis/decomp/003614b0_FUN_003614b0.c). Mode 0 has no brain logic at all (§7) | D |
| brain FUN_00361690 | states in AI+0x24 (0 → 2 → …): distance gates 10/50/80/250 units, target via FUN_0035edc0, motion requests FUN_0055b8e0(actor, n), reads C+0x1c0 0x100000 and C+0x808 | D (structure) / not translated |
| motion requests | FUN_0055b8e0 (the same entry the player's state changes use: commit b9eba85) | D |
| activate | FUN_00559748: body, status, AI mode = section-1 +8 (8 → 0 then FUN_0035f2e0), AI +0x98/+0x99 from record +0x4c/+0x4d | D |
| damage receive | vtable +0x1b4 FUN_0055db50 (bullet), +0x1cc FUN_0055e498, +0x1e4 FUN_0055eac8 (other hit kinds) | D (+0x1b4 from FUN_005442d8) / L (others) |
| damage amount | FUN_0055d2c0: FUN_00318120(status, bullet+0x134, FUN_00554738(bullet)); × hit-zone byte × 0.05 if actor+0x19c; cap 9999 | D |
| apply | vtable +0x1ec FUN_0055c7f8: HP −= dmg (FUN_003168c8, floor 0), then +0x324 hit reaction (enemy FUN_0055c978) and, when dead (FUN_00316cf8), +0x334 death (enemy FUN_0055cac8) | D |
| stagger | FUN_0055d950(actor, bullet+0x153) — the weapon's "hit value" W+0x4a (actions-research §5.3) | D (link) |
| shooter callbacks | shooter vtable +0x314 (hit dealt), +0x31c (kill) | D |

Perception (how state 0 notices the player) lives in the brain updates, mostly in the
undecompiled 0x3614b0 for mode 0, which is the mode every active z128 enemy uses.

## 4. Verification (tracer)

The save state can be read offline: `p2s.read_p2s(...)['eeMemory.bin']`, then the object
table `u32(0x46e880)+0x1164` (index 66–71 and 88 are the active e027s). All pointers are
fixed while those enemies live, so the existing hook (a fixed list of (address, words) pairs,
tools/trace/fields.py) can log them **with no hook changes**: add an `ENEMY` base list to
`resolve_bases` and a `TRACE_FIELDSET=enemy` set. Not trial-run (offline read was enough
to prove the addresses; the emulator run costs a lot of time). Enemies activated *after* the
load get bodies that don't exist yet: those need a pointer-chasing hook (not supported now).

Fields worth tracing per active enemy (7 × ~0x90 B; the log holds ~290 records today, so
expect fewer with these added):

| field | address | why |
|---|---|---|
| matrix / pos | C+0x00 (mat4) | position, facing |
| move state | C+0x1c0, C+0x1c8, C+0xc0, C+0x1d8 | as for the player |
| motion | C+0x570 (name8), C+0x580 | which clip, to check the AI's motion requests |
| HP | status+0x20 | damage |
| AI | AI+0x14, +0x18, +0x20, +0x24, +0x34..+0x40, +0xd0, +0xe0 | state machine, think time, target distances |
| actor flags | actor+0x118, actor+0x28 | alive/active bits |

## 5. Proposed jobs (smallest visible win first)

1. **Decode e027's mesh from the game files.** Fix the index association for the
   "separated" layout in render_skinned.py (session-2026-10-15-geometry §10 has the rule),
   then check vertex UV triples against the hero dump's TBP 0x2c48 draws. Files:
   native/assets/models/render_skinned.py, smd_material.py, a new export_web_enemy.py.
   Done when e027 renders whole with its own textures and matches the dump's UVs.
2. **One e027 standing in z128, idle.** Read section 1 at export time (section-1
   records 44–49 and 66 are the save state's active ones (= object-table entries 66–71 and
   88), or 69–71 near the start), skeleton from 076506c1, clip `r_gst` from
   076506cb via anim_rot.py. Files: export_web_enemy.py, native/web (enemy loader, draw),
   tools/byo/disc.py (inputs). Grade pose/placement against a GS dump that shows soldiers.
3. **Decode the activation path.** Who calls vtable +0x144 (FUN_00559748), what the
   section-1 group id (+4) means, and the script commands in FUN_005661e8. Research +
   a placement list exporter. Needed before more than the 7 save-state enemies appear.
4. **Enemy trace set.** fields.py `enemy` set (§4), one run standing still and one walking
   toward the e027s at x ≈ −950. Gives the AI ground truth. Files: tools/trace/fields.py,
   a script in tools/trace/scripts/.
5. **Default brain (mode 0).** Create the function at 0x3614b0 in Ghidra (manager's call),
   decode and translate it and its scheduler (FUN_00365008/FUN_00365110). Check it
   against the job 4 trace.
6. **Damage and death.** Bullet → FUN_0055db50 → FUN_0055d2c0 (+FUN_00318120 defence,
   hit zones at actor+0x19c) → FUN_0055c7f8 → hit reaction/death clips. The page already
   fires (aim-research.md); hook its bullets to enemy hitboxes.
7. Later: other brains (mode 3 FUN_00361690, mode 9 FUN_00518b30, move layer
   FUN_003667b0), e030/e048 models, enemy weapons (section 24 + FUN_0031a2f0).

## 6. Enemy trace (worker 0116, 2026-10-29)

`TRACE_FIELDSET=enemy` (tools/trace/fields.py): player step/pad/state/matrix/motion, then per
active enemy at load (up to 7, `find_enemies`: vtable 0x58e1f8 with body, status, AI and
+0x28 bit 0): actor +0x28, +0x118/+0x11c (target: FUN_00563200 = id at +0x11c when +0x118
bit 1), body C+0x20..0x3f (facing, position), C+0x1c0/+0x1c8/+0x1d8, motion C+0x570, HP
status+0x20, AI +0x14..+0x43, AI +0xd0. 948 B/record, at most 427 records.

Runs (tools/trace/out/, not committed):
- `k_enemy_engage`: run east (lx 0.6, ly −1). Vincent stops at **x ≈ −1186** in `h_wk` and
  pad input stops registering (pad_held stays 0, R1 ignored); g_ladder_route2 stops at the
  same spot. Looks like an event line taking control, not a wall (not decoded).
- `k_enemy_engage2`: stop at x −1231 (~190 units from #66), R1 ×3 (fired: h_st_f).
  **No enemy reacted**: no motion, target, HP, AI mode or state change in 420 frames.
- ~~Only e1 is scheduled~~ **Wrong (worker 0117):** in k_enemy_engage2 all seven AIs re-queue
  every 15 ticks (AI +0x18 = due, +0x1c = last run). In k_enemy_engage all seven stop together
  at the x −1186 stop, because that stop pauses the scheduler (§7).
- **No trace of an enemy noticing Vincent yet.** Leads: get past the x −1186 event (wait it
  out with a longer script, or find what it plays), or find what wakes the six sleepers.

## 7. Why the save's enemies sleep (worker 0117, 2026-10-29)

**Scheduler (D).** `DAT_0046f4ec` (FUN_00365dd8 / FUN_00364c48): +0 clock (ticks), +8 fraction,
+0x12 per-tick run counter, +0x1c slot table (64), +0x20/+0x24 queue head/tail sorted by due
time AI+0x18, +0x28 count, +0x2c.. a second "wake" list (FUN_00365308/FUN_003656a0, empty here).
FUN_00365008 (from FUN_00323128) returns at once if game-state bit 6 is set (FUN_00325b40 =
FUN_00326068(6)) or object-table +0x2b0 bit 12; else clock += int(step), then FUN_00365110 on the
head while due. FUN_00365110: perception FUN_0035ec10; the brain runs only if owner (AI+0xf0),
brain (AI+0xe0), AI+0x14 bit 0x10 clear and owner vtable +0x26c = FUN_0056bdb8 (`return
actor+0x180`) ≠ 0; then the move/third layers; due = clock + interval (short AI+0x20) + run
counter; re-insert (FUN_00365ad0). Save state: all 7 e027 AIs are queued, interval 15, actor+0x180
≠ 0 for all. Nothing in the scheduler singles out enemy 67.

**Mode 0 is "no AI" (D).** All seven have AI mode 0, whose brain update FUN_003614b0 is empty, and
FUN_0035ec10 only searches for a target when mode ≠ 0 (FUN_0035fd78). A bullet hit only records
AI+0x70 (hits, short), +0x4c (time), +0x90 (shooter id) (FUN_0055db50 → FUN_00360870 →
FUN_003608d8) and changes no mode. So being near them or shooting them can't wake them.

**What sets a mode (D).** FUN_0035f268(ai, mode) callers for e027:
- FUN_0055ac00(actor, on): toggles actor+0x28 bit 0x20000. On: mode = section-0 record +0x82
  (0 → 3, or 5 when body C+0x664 is 0x1e–0x1f; 8 → FUN_0035f2e0 with +0x83). Off: mode 0.
  Called only by spawners FUN_0053d610 (status+0x14 bit 0x40) / FUN_0053d888, and by event 9/10
  of a different class (vtable 0x58e600, handler FUN_0056c710; no such object in z128).
  The save's seven have actor+0x28 = 0x208403: bit 0x20000 clear, never switched on.
- **Event-script command 0x143** in the e027 command handler FUN_005661e8: mode = arg +0x10
  (8 → FUN_0035f2e0(arg +0x14)); 0x193 sets the target to the player (FUN_0035fc98).
  So these soldiers wake when the stage's event script sends 0x143 (section-3 bytecode, FUN_00545b20;
  which event and trigger: not decoded).

**The x −1186 stop is a message box (D for the bits, L for "message").** Running east sets
game-state bits 1, 6, 10, 11 (word *(*DAT_0046e294+4); bit 6 = AI paused via FUN_00325b10,
called from the event VM FUN_0037def0 / FUN_0037cf30) and the AI clock stops at 1821.
Waiting 800 frames does nothing (k_enemy_engage3). **Two ✕ presses close it** (k_enemy_engage4:
bits clear at frame 455, AI clock resumes). START opens/closes the pause menu (bit 1).
Event flags (DAT_0047dbb0 +0x38, XOR DAT_0047dbf0; FUN_0037e3c0 = "wait until flag N == v"):
flag 2 = 7, flag 46 = 1 throughout; no change while the message closed.

**After the message (k_enemy_engage5/6).** Running on reaches the ladder bottom (−1110, −91,
−1109), ~90 units below e027 #66 (−1039, −146, −1100). ✕ there climbs (c_ld_u, c_ld_uml/umr;
y −91 → −119 in 170 frames). R1 ×4 from the ladder bottom fired (h_st_f) but hit nothing (HP,
hits unchanged). **No e027 changed mode, clip, target or HP in any run.** Next: climb to the
top (hold the stick longer, ~400 frames from 830) and see whether the script sends 0x143 there;
or decode section-3 for the 0x143 sender. Trace fields added: gstate, ai_now, evt_obj,
evt_flags/evt_key, eN_ai_hit, eN_ai_hits. Scripts are capped at 1024 frames (hook script area).

## 8. What wakes z128's enemies (worker 0119, 2026-10-29)

Reader: `python3 -I tools/stage_events.py extract/_KEL/noPath/06800001.bin [--dis N | --actions N
[--obj 44,45] | --triggers | --cmds 0x143,0x108]` (reads the stage file at run time; no tables kept).

**Correction to §1/§7:** FUN_00545b20 is not the event VM. It is the +0xc *command handler* of the
object manager (class vtable 0x58ba60, = DAT_0046e880), i.e. the receiver of commands < 0x100.

**Script format (D).** Section 2 = u32 offsets of 298 scripts in section 3 (FUN_00310d20). Each object
has a script index at object +0x34 (actors with none fall back to section-0 +0x7c, FUN_00568f98). Script:
+0 size, +4/+8 globals offset/size, +0xc size of a table of (event id, fn index) pairs; fn header at
table end + index×8 (+4 data, +8 locals, +0xc code; FUN_0036fdc8). Vtable **+0x2f4** on every object
(FUN_0054e4a0) runs "event N" of its script (FUN_003715e0). VM = FUN_00371318: 8-byte instructions
(op, imm), 53 handlers at 0x4a2f60, stack of 8-byte (value, type) cells. Ops used here: 4 push int,
26 pop → register r(imm) (registers VM+0x3c.. = the event's 4 args), 24 push reg, 25/27 local, 52 clear
regs, 18–23 compare, 29/30 jz/jnz, 14 jmp, 17 ret, **38 send cmd imm** (FUN_00370b40): pops target id
(−2 = own id; −1 = host); args r0..r3 → command record +0x10..; cmd < 0x100 → manager (FUN_00545b20),
else the object's +0xc (e027 FUN_005661e8 → FUN_0054cd70). Full op names in the reader.

**Commands seen (D):** manager 0xc(N) = event flag N (FUN_003ff738), 0x3a(k) = object id of section-1
record k (manager +0x3b2; k 44–49 → ids 66–71, k 66 → id 88), 0x30(n) = manager short n. Object
0x104(i,v) / 0x105(i) = write/read actor var i (actor +0xa0, 8 shorts), 0x101(obj, n) = queue message
n to obj (FUN_0053e9e0), 0x108 activate, 0x130/0x109 deactivate, 0x143 AI mode, 0x193 target player.

**Sleepers' own scripts (D)** (#66 → 108, #67 → 109, #68 → 127, #69 → 128, #70/#71 → 126, id 88 → 92).
All the same shape: event 3 (activation, FUN_00559748) → if var7 == 1: 0x193 + 0x143 (mode 2, or 1/5),
else 0x141. Event 8 (hit reaction FUN_0055c978, fired when damage lands) → 0x143 mode 1/2 and
0x101 to the neighbours. Event 2 (message, arg r0) and 0x13 → var7 = 1, 0x143 mode 2. Event 0x17
(death) → 0x101 to the neighbours. Script 0 (stage controller) only activates/deactivates them
(event 0x22 by flag 0; no 0x143 to them).

**Trigger volumes = stage section 8 (D)** (219 × 0xc0; class vtable 0x58cdf8, record at object +0xd4):
+0x00 matrix (+0x30 position), +0x40 inverse, +0x80/+0x90 local box min/max, **+0xa0 script index**,
+0xa4 flags, +0xaa shape. FUN_005517a8 (vtable +0x160, old/new position): outside → inside fires event
**0xd** (flag bit 0), inside → outside fires 0xe (bit 1). Flag 0x20 volumes run event 0x12 instead
(from FUN_005511e8, vtable +0x1b0; its condition is **not decoded**).

| trigger | position | box (local) | script | sends |
|---|---|---|---|---|
| 95 | (−1073.9, −146, −1093.8) | x ±2.6, z ±18.3, 40 tall | 27 (once: own var0) | ev 0xd: 0x101(n 0) → rec 44 (#66), 45 (#67) |
| 75 | (−998.2, −142.2, −1081.5) | x ±39.3, z ±21.8 | 98 | ev 0x12: 0x101(n 1) → rec 44, 45 |
| 89 | (−788, −160, −965.9) | x ±27.6, z ±5.5 | 110 | ev 0xd: 0x101 → rec 46–49 |
| 77, 204 | (−908, −140, −857) / (−871, −153, −816) | | 100 | ev 0x12: 0x101 → rec 46–49 |
| 78 | (−1426.4, −120.9, −740.8) | x ±5.7, z ±43.5 | 101 | ev 0x12: 0x101 → rec 62, 63, 66 (id 88) |
| 96 | (−1511.6, 0, −1094) | x ±59.6, z ±3 | 23 | ev 0xd: 0x108 activate + 0x101 → rec 69–71, 113 |

`--triggers` prints all 219. Which script event a 0x101 message n lands as is **not decoded** (the
queue FUN_0053e9e0 → its consumer); the trace fits event 2 (mode 2 + target player, no hit).

**Recorded (D, trace `out/k_enemy_engage7.jsonl`, script k_enemy_engage7):** close the message at
240/280, run to the ladder, climb (c_ld_u…, top at frame 752: y −146), run east. At **frame 784**
Vincent crosses x −1074 (trigger 95); the same frame #66 and #67 go AI mode 0 → **2** with target =
player (+0x118 = 0x3, +0x11c = 0x16), AI interval 15 → 5, AI state +0x24 0 → 3 → 4. #66: r_gst_lk →
**r_st_a** (785) → **r_st_f** (786, fires), then alternates r_st_a/r_st_f; #67: r_gst → r_st_a (796) →
r_st_f (875). No hit on either (HP, hits unchanged). #68–#71 and id 88 stay in mode 0 (their triggers
are further east / north). The R1 shots at 890–990 (Vincent standing at −1034) hit nothing.
Frames are within one 1020-frame run, so no save-state chaining was needed.

## 9. Awake soldier combat AI, mode 2 (worker 0121, 2026-10-29)

Translation plan for e027 in AI mode 2 (set by script cmd 0x143, §8). D = decoded from the code;
T = matches trace `out/k_enemy_engage7/8.jsonl`; L = likely. Addresses are the game's.

### 9.1 Objects
- **AI** (actor +0x184): +0x14 flags (bit 0 = no run-counter add, 0x2 = target visible,
  0x4 = target in range, 0x10 = AI off, 0x40 / 0x200 brain options, 0x100 = brain started),
  +0x18 next due, +0x1c last run, +0x20 interval (short), +0x24 brain state, +0x26 brain-state
  time, +0x2a action-layer time, +0x2c combat-layer state, +0x2e combat-layer time, +0x34 target
  distance (3D, aim point to own position), +0x38 = +0x34 − both radii (actor +0x2c), +0x3c planar
  distance − radii, +0x40 dy, +0x44/+0x48 angles (FUN_0054a978), +0x64 / +0x68 time target seen /
  lost, +0x6c last perception, +0x7c rounds left (short), +0x80 last seen point, +0xd0 brain mode,
  +0xd4 action-layer mode, +0xd8 combat-layer mode, +0xe0 brain, +0xe4 action layer, +0xe8 combat
  layer, +0xec pre-brain hook, +0xf0 owner actor, +0xf4 owner status, +0xf8 owner body C,
  **+0x100 weapon W** (= status +0xac, stage section 24 copy, FUN_0031a2f0), +0xfc fire-timer entry. D
- **Brain object** (0x1c B, FUN_00361210 / FUN_00361248): [0] AI, [1] actor, [2] status, [3] C.
- **Clock** `*DAT_0046f4e4` = scheduler +0 (ticks); +0x10 rays this tick, +0x11 shots this tick,
  +0x12 AI runs this tick, +0x13 total (FUN_00360378, FUN_003605d8). D
- **e027 W (save, 0x10d83f8)**: +0x16 pellets 1, +0x18 range 50 (→ 500 / 515), +0x2c speed 300,
  +0x40 damage 45, +0x42 first delay 0, **+0x44 interval 180**, **+0x46 reload 100**,
  **+0x48 magazine 6**, +0x4a hit value 12 (layout: actions-research §5.3). D (values read from RAM)

### 9.2 Per tick, in this order (FUN_00365008, AI clock += int(step))
1. Due AIs (head of queue, `due − now < 1`) → **FUN_00365110(ai)**; if none ran this tick
   (+0x12 == 0) the head runs anyway. D
2. **FUN_003656a0**: the fire-timer list (scheduler +0x2c..; max 2 entries per tick). D

**FUN_00365110(ai)**: perception FUN_0035ec10 → [+0xec hook] → brain vt+0x1c → if brain +0x10
bit 0: FUN_0035f268(mode = brain +0x14) → action layer +0xe4 vt+0x1c → combat layer +0xe8 vt+0x1c →
due = now + interval (+ run counter unless AI+0x14 bit 0) → re-queue (FUN_00365ad0). The brain and
layers run only if owner, brain, AI+0x14 bit 0x10 clear and owner vt+0x26c (actor+0x180) ≠ 0. D

### 9.3 Perception FUN_0035ec10 (every AI run)
- Skip (only distances refreshed) if rays this tick (+0x10) ≠ 0 and (rays > 2 or now − AI+0x6c < 61)
  and now − AI+0x6c < 121. D
- No target and mode ≠ 0: FUN_0035fd78 → candidate FUN_005608e8(actor,0,0); keep it if
  FUN_003600e0 (line of sight) → FUN_0035fc98 sets target, bit 0x2, +0x64 = now. D
- Target: bit 0x2 = FUN_00360290 → FUN_003600e0(ai, target, 3): eye = FUN_005634e0(actor, 3)
  (height actor+0x30 − 2), target point = same on the target; FUN_00360378 = ray
  FUN_005449f0(mgr, probe 3) only if length ≤ 1000; clear = visible. Bit 0x4 = FUN_003602e8(ai,3,0):
  AI+0x34 < W+0x18 × 10 (500 here). +0x64 / +0x68 on a bit-2 change. D
- FUN_0035e830: AI+0x34..+0x48 from aim point FUN_00360558 (= FUN_00563438, actor +0x120 =
  target position via FUN_00562ff8) and own position. D. T: AI+0x14 = 0x106 from frame 785 on.

### 9.4 Brain mode 2 = class vt 0x4a2608 (FUN_0035f300 case 2 → FUN_00362e58)
Start vt+0x14 **FUN_00362ee8**: AI+0x20 (interval) = **5**. D, T (15 → 5 at f784).
Update vt+0x1c **FUN_00362ef8**, state AI+0x24:
- pre: state ≠ 1 and FUN_0055ab08 (C+0x565 bit 7, busy) → state 1. FUN_0055bb40 (actor +0x104 in
  0x80..0xbf, an attack) → return. No target (FUN_0035eda0) → state 5.
- **0**: FUN_0055b8e0(actor, **3**, 0, 0) (actor state 3 = gun up → r_st_a), FUN_0055a828 (stop,
  FUN_003533e0), FUN_0035f540(ai, 0) (no action layer), FUN_0034e960(C, 6) (C+0x214 = 6: stand,
  aim point C+0x670). No target → state 2 (FUN_0035eb50). **C+0x808 bit 0** (e027: 0x872b) → **3**,
  else 7. T: f785 st 3, actor state 3, r_st_a.
- **1**: wait until not busy → 0.  **2**: wait 31 ticks → 0.
- **3**: FUN_0035f9a0(ai, **2**) (combat layer 2) → state **4**. T: f786.
- **4**: nothing (the combat layer does the work). T: stays 4 to the end of both traces.
- **5** (lost target): actor state 3, or with AI+0x14 bit 0x40: 0x1b if motion 0xb6 exists, 6 if
  motion 2 exists, else 3; C+0x214 = 6 → **6**: wait for a target → 0.
- **7/8** (non-shooter branch, C+0x808 bit 0 clear; not e027): every ≥ 0x18 (FUN_00310c40) and after
  60 ticks with 1/11 chance (or AI+0x14 bit 0x200 and > 60 ticks): attack index FUN_0035ff98 →
  FUN_0055baa8(actor, 0x80 + i) → 8; 8 waits for the attack to end → AI+0x20 = 6, state 7. D (not traced)

### 9.5 Combat layer 2 = vt 0x4a2370 (FUN_0035fa10 case 2; enter FUN_00360e30 = stop fire)
Update **FUN_00360e50**, state AI+0x2c (skipped while busy):
- **0**: magazine empty (FUN_003604f0: W and AI+0x7c == 0) → **FUN_00360718**: reload =
  FUN_005612e8(actor, …) (C motion state 0x1c, or 0x1d by actor +0x100 bits 40–41) unless
  C+0x564 bit 0x4000 or actor +0x118 bit 0x10000; refill FUN_00360418(ai, 0): AI+0x7c = W+0x48.
  No target → stop fire (FUN_00360820). Target alive: **visible AND in range** (FUN_003601d8, bits
  0x2 & 0x4) → if no timer: **FUN_003607a8(ai, −1)** (start fire timer); else stop fire and, after
  301 ticks without a shot window (FUN_0035eba0 vs +0x2e), → 1. Target dead → 1, or 2 when
  FUN_00316d08 (HP 0 and status +0x64 bits).
- **1**: after 61 ticks: FUN_0035fe08 = re-target FUN_005608e8 → 0.  **2**: target revived → 3;
  no target → 1.  **3**: after 31 ticks → 0. D. T: state 0 throughout (target stays visible).
- **FUN_003607a8**: FUN_00365308(sched, ai, period = W+0x44, count −1) adds entry {+8 due = 0 (now),
  +0xc AI slot, +0xd shots left (−1 = no limit), +0xe period}; if period < 15, aim pose on
  (FUN_00565ff0(actor,1): actor +0x188 bit 38). **FUN_00360820**: remove entry (FUN_00365478), pose off.

### 9.6 Fire timer FUN_003656a0 → shot FUN_003605d8
- Entry due: magazine empty → next due = now + W+0x46 (100) + i; else if AI+0x14 bit 0x10 clear →
  **FUN_003605d8(ai)**. Shots-left −1 (if > 0; 0 → delete). Next due = now + period + i, and when
  FUN_00326500() (FUN_002424a0 ≠ 0; true in this save): period > 12 → **+ int(period × (rand − 0.5))**
  (rand = FUN_0040b5c0, 0..1); period ≤ 12 → r = rand × 24: r < 1 → +2·period, 1 ≤ r < 2 → +period. D.
  T: #66 shots at clock 2360 → 2626 (266 = 180 + 86).
- **FUN_003605d8**: return if AI+0x7c == 0 or target dead. Aim point FUN_00360558. Actor +0x118 bit
  0x800 clear and **C+0x564 bit 0x4000** (motion state busy): shoot only if bit 0x200000
  (FUN_00569340 directly), else no shot. **Standing (C+0x1d8 == 0)**: C+0x214 = 6, then
  **FUN_00341b78(C, aim)**: FUN_003424c0 stores the aim point at C+0x670 (+ a side offset by stance
  C+0x664 when C+0x564 bit 0), then motion state = (current state +0x74) or **0xe** (FUN_00341780).
  Moving: FUN_00569340 at once. Then shots-this-tick +1 and **AI+0x7c −1 whether or not a shot
  went out**. D. T: #67's first timer call (f799) took a round (7 → 6) with no r_st_f (C+0x564 had
  0x4000 while r_st_a blended in); its first real shot is f875.
- Initial rounds: FUN_00360418(ai, 1) = W+0x48 + int((W+0x48/2 + 1) × rand) (L: T #66 6, #67 7).

### 9.7 Motion state 0xe (standing shot; factory C+0x7d8 vt 0x5a6c28 → FUN_00592918 → vt 0x5a6400)
Update vt+0x24 **FUN_00596460**, phase +4, timer +0xc (+= step):
- 0: (C+0x300 == 0 and not FUN_00342308 → FUN_003533e0), FUN_00345530(C, 1) → 1.
- 1: muzzle FUN_00341ec8 (marker 15), **FUN_00341988(C, 1, {muzzle, C+0x670})** → slot
  FUN_0056a038 (jump table 0x58de60[1] = 0x56a11c) → **FUN_00569340(actor, aim)**; motions
  **0x57 / 0x58 / 0x59** (fire + up/down partners, flags 0x82; = r_st_f) → 2.
- 2: when the motion ends (FUN_003460d8) or timer > 30: motions **0x40 / 0x41 / 0x42** (flags 0x83;
  = r_st_a) and done bit (+8 bit 0) → returns 6. D. T: r_st_f for 2 frames then r_st_a (f786–788).

### 9.8 The bullet and damage to Vincent
- **FUN_00569340** → actor +0x14c = now; W = status +0xac; W+0x78 bit 0x800 clear →
  **FUN_005694f8**: muzzle (FUN_00341ec8, or W+0xb4/b5 markers), **FUN_005409a8(mgr, muzzle, aim,
  W, status)** spawns the bullet (as Vincent's, FUN_00552dd8: damage W+0x40 → bullet +0x134, range
  W+0x18 × 10 + 15, speed W+0x2c). No spread in the path (as for the player). D (calls) / L (range,
  speed as for the player).
- Hit: **FUN_0055db50(victim, bullet, …)** → damage **FUN_0055d2c0** = FUN_00318120(status, bullet
  +0x134, FUN_00554738(bullet)) (defence), × hit-zone byte (actor +0x19c [0]) × 0.05 unless bullet
  +0x120 bit 0x800, cap 9999 → victim vt+0x1ec = **FUN_0055c7f8** (both classes): status HP −=
  (FUN_003168c8); then vt+0x324 (alive: e027 FUN_0055c978, player FUN_00576ac8) or, on death, +0x324
  then vt+0x334 (e027 FUN_0055cac8, player FUN_00576b48). Reaction state: FUN_00565480 (by bullet
  flags +0x148 | hit zone FUN_0055d950; → FUN_00561270 / FUN_005618d0, which sets actor state
  0xf / 0x10). AI side: FUN_00360870 (AI +0x70 hits, +0x4c, +0x90). D (chain) / not decoded: the
  defence formula inside FUN_00318120 and the reaction branches of FUN_00565480.
- T: Vincent HP **780 → 736 (−44)** at f789, 3 frames after #66's f786 shot, and 736 → 692 at f1011
  after the f1008 shot (45 damage − 1 from FUN_00318120: L). Correction to §8: #66's first shot does
  hit Vincent; §8's traces had no player HP field.
- e027 hit reaction FUN_0055c978 (alive): sound cues 0x44 / 0x4c (FUN_00341330 / FUN_003412d0, C+0x7ec
  cue table, 15-tick throttle; NOT motions), script event 8 (§8: 0x143 mode 1/2 + 0x101 to
  neighbours). vt+0x344 is empty for e027. Death FUN_0055cac8: cue 0x4e (or 0x4c), script event
  0x17, FUN_005435a8, AI off (FUN_0035f728: modes 0), item drop (FUN_00316d70 / FUN_00541688,
  lifetime 3600 ticks), vt+0x374(actor, 2, now). D

### 9.9 Kill trace `k_enemy_engage8` (TRACE_FIELDSET=enemy; new fields p_hp, p_aim, cam_lock, eN_ai_ammo, eN_ai_timer, eN_astate, eN_cflags)
Route as k_enemy_engage7 to the trigger (f784), stick released 787, R1 795 (draw), rx −1 805–813 and
ry +1 815–825 (aim onto #66; without them the aim ray pointed ~14° right of #66 and ~17° up, 194 u
out, and no shot touched it), R1 830/860/890/920/950. Per frame, mapped onto §9.2–9.8:

| frame | game | decoded step |
|---|---|---|
| 784 | #66/#67 mode 0 → 2, actor +0x28 gains 0x4000 | 0x143 → FUN_0035f268; brain start FUN_00362ee8 (interval 5) |
| 785 | #66 brain 3, actor state 3, r_st_a, AI+0x14 0x106 | brain case 0 (C+0x808 bit 0) |
| 786 | #66 brain 4, timer set, **ammo 6 → 5**, r_st_f | case 3 → combat layer 2 state 0 → FUN_003607a8 → FUN_003656a0 → FUN_003605d8 → FUN_00341b78 → class 0xe |
| 788 | #66 r_st_a | class 0xe phase 2 (fire motion done) |
| 789 | **Vincent HP 780 → 736** | bullet hit → FUN_0055c7f8 on the player |
| 796–799 | #67 brain 3 → 4, ammo 7 → 6 with no r_st_f (C+0x564 0x4000) | FUN_003605d8 busy branch: round used, no shot |
| 831… | Vincent's shots: #66 AI hits +3 per shot (833/834/835, 863–865, …; 15 after five shots) | FUN_00360870 per capsule near-miss via FUN_0055df90 (§9.10), not a hit |
| 875 | #67 r_st_f, ammo 6 → 5; Vincent HP unchanged | shot (missed / blocked: not decoded) |
| 1008 / 1011 | #66 r_st_f, ammo 5 → 4; Vincent 736 → 692 | timer: 2360 → 2626 ticks (180 + jitter) |

**Resolved in §9.10 (worker 0123): the hits were capsule near-misses, not damage.** Was open: #66 takes hits but its HP stays 1 — no hit reaction, no death,
no script event 8 (mode stays 2, brain 4). Not explained by the save's values: status +0x64 = 0
(FUN_0055f0e0 bits 0/1 clear), actor +0x118 = 0x203 (no 0x20 / 0x40 / bit 31), actor +0x19c = 0 (no
zone scaling). Candidates to trace next: damage FUN_0055d2c0's result (defence FUN_00318120 against
the player's bullet kind FUN_00554738), actor +0x19c and status +0x64 at the hit frame, and whether
the +3 per shot is three bullets or one bullet counted three times. Death (FUN_0055cac8) therefore
isn't in a trace yet; a translator should take §9.8 from the code and not wait for it.

### 9.10 Why #66 never died, and the kill (worker 0123, traces `k_enemy_engage9`, `10`, `11a`)
- **HP 1 is real and by design.** Status +0x04 (max) and +0x20 are both 1 for #66 at load. The
  max HP comes from FUN_00314ea8: if status +0x0c override bit 0x14 is set (FUN_00315738), max HP =
  **section-1 record +0x50**, else section-0 record +0x30. #66's record (e_Sol_21d) has bit 0x14
  set and +0x50 = 1; #67 (e_Sol_22d) has the bit clear and gets 100. Other override bits #66 has:
  5 (FUN_0055a0d8: activation only when actor +0x28 bit 1), 0x12 (item drop, FUN_00316d70). Neither
  touches damage. Same class and vtable as #67 (0x58e1f8). D
- **engage8's 15 "hits" were near misses.** AI +0x70 counts a bullet inside the actor's capsule,
  not damage. Bullet path (FUN_005442d8): capsule test vt+0x19c **FUN_0055c5f8** (radius actor
  +0x2c) → vt+0x3c / +0x184 → part test vt+0x17c **FUN_0055bda8**: part = FUN_005ad118 (ray against
  the model's per-part volumes, chunks 'mkd'/'skd'; −1 = none). If there's no part, or the part is
  masked (C+0x77c bit), then **FUN_0055df90 → FUN_003608b8 → FUN_00360870(ai, 0, 0)**: AI +0x70 +1,
  no damage, AI +0x90 unchanged. Otherwise actor +0x142 = part and actor +0x19c = zone record
  (status +0xa8 → +0x30 + part × 16) → FUN_0055db50. D. T (engage9): 3 per shot (the bullet is inside
  the capsule for 3 frames), AI +0x90 stays −1 and +0x19c stays 0, so FUN_0055db50 never ran.
  The aim was ~8° right of #66 (bearing 89° vs 80.6°, 31.5 u away).
- e027 zones (13, 16 B, byte 0 × 0.05 = multiplier): 0 = 0x50 (×4, head L), 1 = 0x18, 2 = 0x14,
  3–8, 10, 11 = 0x10, 9 and 12 = 0x0e. None are 0. D (RAM values)
- 1-frame rx taps don't move the aim (engage10: bearing stays 89.1°). R1 during h_st_f is ignored, so
  the shots land every 40 frames. T

**Kill, `k_enemy_engage11a`** (engage8, rx −1 805–817, ry +1 819–829, R1 every 40 frames from 840;
aim bearing 81.2°):

| frame | #66 | decoded step |
|---|---|---|
| 840 | R1 | Vincent fires |
| 843 | HP 1 → **0**; AI +0x70 = 1, **AI +0x90 = 0x160016** (shooter = player id 0x16); actor +0x19c = zone **6** (×0.8); AI mode 2 → **0**, combat state 0, fire timer +0xfc → 0; actor state 3 → **7**; C+0x1c8 → **12**; C+0x564 0x40000400 → 0x4000c000; actor +0x118 0x203 → 0x003, +0x28 loses 0x400; clip **r_stlaf0** | FUN_0055bda8 → FUN_0055db50 → FUN_0055d2c0 → vt+0x1ec FUN_0055c7f8 (HP 0) → +0x324 FUN_0055c978(dmg, 1) → +0x334 FUN_0055cac8 (FUN_0035f728: AI modes 0) → FUN_00565480(…, dead) → **FUN_00560fc8**: knock-back FUN_00353340(12.0, 16.0, C, hit dir), FUN_0034edf0(C, 0xb), then FUN_0055b8e0(actor, **7**) unless actor +0x118 has 0x8000 / 0x10000. D (chain) / L (which FUN_00565480 branch) |
| 843–857 | slides +9.7 u in x (away from Vincent), C+0x1c0 0x30301 → 0xf0701 → 0x30701 → 0x30301 by 860 | the FUN_00353340 knock-back. T |
| 860–1020 | r_stlaf0 held, HP 0, mode 0 | corpse. T |
| 896 | C+0x564 +0x20, actor +0x118 +0x100 | not decoded |

There's no hit-reaction clip (HP 1 means the first part hit kills), and #67 doesn't react (mode 2,
keeps firing). A non-lethal hit (FUN_005618d0 → actor state 0xf / 0x10) still needs a trace on #67
(HP 100). Its bearing (79.9°, 118 u) is close to #66's, so a run that kills #66 and keeps firing
should reach it.

### 9.11 Enemy weapon record, bullet flight, Vincent's defence (worker 0125, 2026-10-29)
- **W source (D):** status +0xac = stage section 24 record (FUN_00318610 / FUN_00311600, 0x104 B,
  first 0x78 B = W). Index FUN_00318718: status +0x0c (= section-1 +0x40, FUN_00315050) bit 0x16 ?
  section-1 +0x54 : parameter set (section 0) +0x88. Records 44 / 45 -> 2 = the RAM values in §9.1.
- **Bullet (D):** FUN_005409a8(…, 0, 0) -> FUN_00554568 -> FUN_00552dd8(b, 0): speed W+0x2c / 6, end =
  start + normalize(aim − muzzle) × (W+0x18 × 10 + 15) (aim only with W+4 bit 0x400000), **+0x142 =
  arg + 2 = 2 frames of countdown** (FUN_005549f0) before the first move. Explains the 3-frame
  shot -> HP gap (§9.8, T: f786 -> f789, f1008 -> f1011 at 31-34 u, 60 u per frame).
- **Defence (D, values from the save):** Vincent level 4, +0xc0 0, W+0x68 0, G+0x60 20 → level term
  int(0.33·4 + 0.4 + 0.5) = 2; +0x60 0x8233 → difficulty cut index FUN_00242c28 − 1 = 0 → table
  +0xaa = 0. 45 → 44 (the "−1" of §9.8). FUN_00554738 kind 0 for e027 bullets.
- **f875 miss (open, two attempts):** #67's bullet passes 2.6 u from #66's axis at chest height; #66
  shows no hit / near miss and FUN_0055db50 has no friendly-fire exit → the object filter vt+0x1ac
  skips soldiers (L). Why it misses Vincent (his part test vt+0x17c on his per-part volumes, or the
  room probe 0 between the decks) is not decoded.
- Manager cmd 0x2f(n, v) = manager short n = v (FUN_00545b20). Object cmd 0x113 = object +0x36 = r0
  plus a delayed message to itself via FUN_0053e9e0 (delivery not decoded).
- GameRandom twist checked against the save's MT words (index 231, word 230). T

## 10. Non-lethal hit, hit reaction and the kill of #67 (worker 0124, trace `k_enemy_engage12`)

Trace: k_enemy_engage11a to the #66 kill (f843), then rx −1 845–851 and ry −1 852–861. Camera
forward was 80.2° / −17.4° vs #67 at 78.0° / −0.7° from the eye (new fields `cam_fwd` CAM+0x80,
`cam_eye` CAM+0x90). The aim ray crosses #67, lock-on FUN_00328968 takes it (cam_lock 0x43 by
f864) and auto-aim holds it. R1 at 866 / 906 / 946 / 986 / 1018. #67 = e1 (EI1 0x43).
TRACE_FIELDSET=enemy, 1024 frames. Records show the previous frame's result, so record N is the
logic of frame N−1.

### 10.1 What happened (T)
| record | #67 | step |
|---|---|---|
| 866 | R1 (one bullet) | Vincent fires |
| 870 | **HP 100 → 75**, part 10, zone rec 10 (×0.8), AI +0x70 = 1, +0x4c = now, **+0x90 = 0x160016**; actor state **stays 3**; clip **r_stlld0**; C+0x1c8 0 → **12**; C+0x564 0x40000400 → 0x4001c000; moved 2.4 u away from Vincent | non-lethal hit: §10.2 → §10.3 live branch → FUN_00561570 |
| 871 | **HP 75 → 0**, part **0** (head ×4), AI +0x70 = 2; AI mode 2 → **0**, timer 0; actor state **7**; clip **r_stbf0**; C+0x1c0 0x10301 → 0x30301; actor +0x28 loses 0x400 | the same bullet, one frame on (still inside the capsule), hits the head → FUN_00560fc8 |
| 871–950 | slides ~11 u, then stops | death knock-back FUN_00353340(12, 16) |
| 988 | C+0x564 +0x20, actor +0x118 +0x100 | §10.5 |

So the "multi-hit kill" is two part hits from one bullet: 25 (limb) + 126 (head, capped at HP 75).
#66's death (f843) is the same as in §9.10. **No other soldier reacts**: e2–e6 stay in mode 0
(r_gst / r_gst_lk) through f1023, after the hit and both deaths. #66 didn't react to #67's hit
either (it was already dead). T

### 10.2 Damage per hit (FUN_0055db50 → FUN_0055d2c0 → FUN_00318120)
1. **FUN_0055db50(victim, bullet, …)**: returns early if status bits 0/1 = no damage (FUN_0055f0e0).
   `wasDead = FUN_00316cf8(status)`. dmg = FUN_0055d2c0; 0 if actor +0x118 has 0x20 or 0x40.
   Applied = min(dmg, HP), used for the shooter callback (vt+0x314). D
2. **FUN_0055d2c0**: dmg = FUN_00318120(status, bullet +0x134, kind FUN_00554738(bullet)); if
   actor +0x19c (zone record) ≠ 0 and bullet +0x120 bit 0x800 is clear:
   **dmg = int((dmg + 0.5) × zone[0] × 0.05)**; cap 9999. D
3. **FUN_00318120** (defence): kind 3 → raw. L = armour (status +0xc0 table, or equip +0x68) +
   level term FUN_0031ce90(status +0x11) = int(k0·lvl + k1 + 0.5) (k from G+0x14 +0x270 = 0.33 /
   0.4 when FUN_00310c60 = G+0x60 ≥ 7, else 0 / 50), capped at 99. d = int((raw + 0.5)(100 − L)
   × 0.01). Status +0x60 bit 2 (and not 0x100): a second, difficulty-table cut. Then FUN_0031d388
   element factor {1, 0, 0.25, 0.5, 2}[status+0x9c→+0x9c bits], ×0.2 / ×0.33 for status +0x27
   nibble 1 / FUN_00317b68, and min 1 if raw > 0. D (code). For #67 in this save: lvl 0, +0xc0 0,
   +0x60 0, element 0 → **d = raw**. D (RAM values)
4. Bullet raw = W+0x40 (FUN_00552dd8: bullet +0x134 = (short)W+0x40; hit value +0x153 = W+0x4a).
   In this save, Vincent's slot records (*(PS+0x84), FUN_0031a990, R+0x4c = W) give damage **30**,
   hit value **4**, interval 25, reload 80, magazine 18. (actions-research §5.1's 85 / 38 / 9 is a
   different set of parts.)
- **Open (two attempts):** 30 gives int(30.5 × 0.8) = 24, but the trace shows **25**. That needs a
  defence output of 31 (int(31.5 × 0.8) = 25, and the head check int(31.5 × 4) = 126 also fits).
  Where the +1 comes from isn't decoded: the live W may differ from the slot record, or a bonus
  could apply. Next step: log the bullet's +0x134 (bullet list of mgr DAT_0046e938) on the hit frame.
- Zone records (status +0xa8 → header, **+0x30 = pointer** to 16-byte records, so the zone record
  is `*(hdr+0x30) + part × 16`, as FUN_0055bda8 writes): byte 0 = scale (0 = 0x50 head, 1 = 0x18,
  2 = 0x14, 3–8, 10, 11 = 0x10, 9, 12 = 0x0e), byte 1 high nibble = hit-effect class (FUN_003361b8),
  ushort +2 bit 1 = no reaction flags, bit 2 = skip stagger, bit 3 = bullet +0x120 |= 0x60000 (head
  record has bit 3). D (code) / D (RAM)

### 10.3 Reaction vs death: FUN_00565480(actor, flags, hitdir, hitpos, wasAlive)
flags = bullet +0x148 | **stagger FUN_0055d950(actor, hit value)** (unless zone bit 2), zeroed by
zone bit 1. Called only if actor +0x118 has no 0x10000. Live and lethal hits go through the same
function; "dead" is FUN_00316cf8 after the damage. D

**FUN_0055d950 (stagger)**: if now − actor +0x148 < 60 ticks, +0x150 += hv and +0x152 += hv; else
both = hv and phase (bits 40–43 of +0x150) = 0. +0x148 = now. Thresholds (FUN_00310c40 = G+0x64
≥ 27: status +0x9c → +0x94 / +0x96 / +0x98; else 1 / 30 / 60; this save 1 / 30 / 60):
+0x152 ≥ t1 → **0x42**, +0x152 = 0, phase 0 → 1; phase 1 and +0x150 ≥ t2 → **0x44**, phase 2;
phase 2 and +0x150 ≥ t3 → **0x48**, both = 0, phase 0. D. With hv 4: every hit gives 0x42; the 8th
hit inside 60-tick gaps gives 0x44; the 15th gives 0x48.

FUN_00565480, in order (D):
1. Body C+0x1c8 ≠ 12 (or C+0x4c0 / +0x530 clear) → FUN_00356740(C+0x3b0, 0, FUN_002419d8(0)).
   Enemy (actor +0x28 bit 0x20, not 0x20000) or C+0x45c bit 17: flag 8 → 4. Actor +0x118 bit 0x800:
   8 (with bit 4) or 4 (with bit 0x10) → flags = 2.
2. `wasAlive` = 0 (already dead): only motion class 0x36 → FUN_005612a0. Otherwise:
3. Actor +0x110 nibbles (bits 8–11 when alive → FUN_00562670; bits 12–15 when dead →
   FUN_00562778). #67: both 0.
4. Busy states FUN_0055aa88 / attack FUN_0055bb40 / FUN_0055bb68 or in the air (bVar1) with stance
   (actor +0x100 bits 46–47) ≠ 0: flags 0xc → dead FUN_00561270 / alive FUN_005618d0(…, 2); else
   dead → FUN_00561270. FUN_0055a410: state 10 and dead → FUN_00562d68.
5. **flags & 8** (knock-down) and no +0x118 bit 4: dead → FUN_00561270(…, 0 or 2); alive → (state
   0xf → none) FUN_005618d0(…, 0 if C+0x1c0 bit 0x200 and no flag 0x40, else 2).
6. **flags & 4** (big) and no +0x118 bit 0x10: state 0xf → none; no C+0x1c0 0x200: dead
   FUN_00561270(…, 2) / alive FUN_005618d0(…, 2); with 0x200 and motion class ≠ 0x36: dead
   FUN_00561138 / alive FUN_00562898 (gated by flag 0x40 when +0x118 bit 0x100000).
7. **flags & 2** (small; **this trace**, 0x42) and no +0x118 bit 8: state 0xf → none; no C+0x1c0
   0x200: dead FUN_00561270 / alive FUN_005618d0(…, 0); **with 0x200** (#67: C+0x1c0 0x10301) and
   class ≠ 0x36: **dead → FUN_00560fc8** (§9.10, now D); **alive → FUN_00561570(actor,
   normalize(hitdir + 0.15 × normalize(own pos − hitpos)))** (gated by flag 0x40 when +0x118 bit
   0x100000). Unless +0x118 bit 8 (alive: none).
8. No flag 2/4/8 and class ≠ 0x36: dead → FUN_00560fc8. Class 0x36 → FUN_005612a0.
T: f870 = step 7 alive, f871 = step 7 dead.

### 10.4 The two outcomes
- **FUN_00561570 (flinch, alive)**: actor +0x118 bit 8 → none. Unless C+0x300 is 1 or 0xe (motion
  state; 0xe = the standing shot §9.7) or +0x118 has 0x800 / 0x1000: FUN_0034b638(0.15, C, (0,0,0,1)) (L:
  the operand is the VU zero/w=1 register), and for an enemy (+0x28 bit 0x20 without 0x20000)
  **FUN_003531e0(4.0, 4.0, C, dir)** (push-back; C+0x7e4 +0x40 / +0x44 override when its +0x10 bit
  0x10000) and FUN_0034edf0(C, 9); then FUN_0034e960(C, 4). Then **FUN_00561718**: throttle: if now
  − actor +0x144 < 12 ticks (50 % chance: < 6, rand FUN_0040b5c0), no clip and +0x144 = now − 6.
  Else, by stance actor +0x100 bits 40–41: 0 → **motion state 0x1e** (FUN_00341780(C, 0x1e, 0)),
  1 → 0x1f, 2 → actor state 0x11; FUN_00565bd0(actor, stance); +0x144 = now. (Class 0x37 first
  re-picks the stance from C+0x590.) In the C+0x300 = 1 / 0xe case, C+0x1c0 |= 0x20 if a clip was
  started. **Actor state is not changed** (T: stays 3). D (code), T (clip r_stlld0 = class 0x1e
  for stance 0; C+0x1c8 → 12 and the 2.4 u move in one frame = the push-back). Not decoded:
  which r_st*ld/lf/… clip class 0x1e picks (by part / direction); r_stlld0 here.
- **FUN_00560fc8 (death)**: FUN_003533e0 (stop); FUN_00353340(12.0, 16.0, C, dir) (enemy:
  DAT_0046edac instead of 12 when +0x28 has 4 and 0x20; C+0x7e4 +0x50 / +0x54 override);
  FUN_0034edf0(C, 0xb); status bit 6 → FUN_0056b620 / event FUN_0053e9e0(…, −0xcd); then
  **FUN_0055b8e0(actor, 7)** unless +0x118 has 0x8000 / 0x10000. D. T: state 7 on both kills; clips
  r_stlaf0 (#66, hit from 31 u) and r_stbf0 (#67, head). The death motion (class 0x36, as
  FUN_00565480 tests via FUN_003419c0) picks the clip; that choice isn't decoded.
- Before FUN_00565480, FUN_0055db50 calls vt+0x1ec = FUN_0055c7f8 (HP −= dmg; alive →
  FUN_0055c978 cues + script event 8; dead → FUN_0055cac8 AI off, §9.8). After it:
  FUN_00360870(ai, shooter, dmg) (AI +0x70 / +0x4c / +0x90), FUN_003361b8 (hit effect by zone
  nibble), FUN_00567f10 (enemy: dmg). D

### 10.5 The frame-896 flags (§9.10), decoded
**FUN_00343160** (body motion update): when the motion-state object C+0x7d0 has its done bit (+8
bit 0) and C+0x564 bit 0x20 is clear → C+0x564 |= 0x20 and FUN_00341af0 sends the motion-done
event to the actor. The e027 handler **FUN_00539570**, event 0x36 (death motion) and the actor dead
(FUN_00316cf8) → **actor +0x118 |= 0x100** (corpse settled). D (writers) / T (#66 53 frames after
its death, #67 117: the end of each death motion, which differs by clip). L (that FUN_00539570
is the e027 vtable entry; it is the only writer found).

### 10.6 Translation plan (in order, per bullet part hit)
FUN_0055bda8 (part, zone) → FUN_0055db50: [FUN_0055f0e0 gate] → wasDead → FUN_0055d2c0
(FUN_00318120 then × zone[0] × 0.05) → FUN_0055d950 stagger flags → vt+0x1ec FUN_0055c7f8
(HP, FUN_0055c978 / FUN_0055cac8) → FUN_00565480 (§10.3) → FUN_00561570 + FUN_00561718
(alive) or FUN_00560fc8 (dead) → FUN_00360870 → FUN_003361b8 → FUN_00567f10. Later:
FUN_00343160 motion-done → FUN_00539570 → +0x118 |= 0x100.
Fields: actor +0x100 (stance), +0x104 state, +0x110, +0x118, +0x142 part, +0x144 last flinch,
+0x148 / +0x150 / +0x152 stagger, +0x19c zone; status +0x11, +0x20, +0x27, +0x60, +0x64, +0x9c,
+0xa8, +0xc0; C+0x1c0, +0x1c8, +0x300, +0x564, +0x7d0, +0x7e4; bullet +0x120, +0x134, +0x148, +0x153.
Constants: zone scale × 0.05, cap 9999, stagger 60-tick window and 1 / 30 / 60, flags 0x42 / 0x44 /
0x48, push-back (4, 4), death knock-back (12, 16), flinch throttle 12 / 6 ticks, aim mix 0.15.
Still open: the 30 → 31 damage gap (§10.2); class 0x1e and 0x36 clip choice; FUN_005618d0 /
FUN_00562898 (big / knock-down reactions, not reached: they need 8 / 15 hits in a chain).

## 11. How an enemy bullet hits or misses Vincent (worker 0127, traces `k_bullet1` / `k_bullet2`)

Translation plan to replace the page's `BULLET_R` stand-in. D = decoded from the code, T = matches the
call log (below), L = likely. Same game objects as §9.8 / §9.11.

### 11.0 The f875 "miss" explained
**It's the fire-time spread, and the RNG stream it draws from.** Each e027 shot tilts its direction by
up to ±12.2° (§11.2), using two draws from the shared GameRandom (FUN_0040b5c0). Vincent's own
shots draw from the same stream (2 per pellet, 3 pellets per shot in this save, spread 2.918°).
- k_enemy_engage7 had no `p_hp` field, so it never showed a miss. Re-run with the engage7 inputs
  (`k_bullet1`): #67's f875 bullet draws r1 = 0.4294 → 1.7° tilt → **hits** at f879 (HP 736 → 692,
  part 0), the same frame as the page's sim. T
- k_enemy_engage8 (`k_bullet2`): Vincent fired at f831 and f861 (6 pellets, 12 draws), so #67's f875
  draws are r1 = 0.0966, r2 = 0.7922 → **9.84° tilt**. Measured angle between (aim − muzzle) and the
  bullet's direction: 9.8°. The bullet passes ~19 u wide of his capsule (radius 5): no capsule
  hit, no part test, no damage. T
- #66 standing in the path is skipped by the object filter (§11.3 step 2: same side). T: filter
  FUN_0055d100 returns 0 for #66 and #67, 1 for Vincent, every frame.

### 11.1 Timeline of one shot (T, k_bullet1 / k_bullet2; frames = record frames)
| frame | what | function |
|---|---|---|
| F (r_st_f) | bullet spawned, spread applied, countdown +0x142 = 2 | FUN_005694f8 → FUN_005409a8 → FUN_005405b0 → FUN_00552dd8 |
| F+1 | countdown 2 → 1, no move | FUN_005549f0 |
| F+2 | countdown 1 → 0: **room probe** (one ray, §11.4), no move | FUN_00554970 → FUN_00553808 |
| F+3… | move 60 u, then hit-test the frame's segment | FUN_00556068, FUN_00556870 → FUN_005442d8 |
T: #66's f786 shot → hit test and HP 780 → 736 at 789 (31 u away, first segment); #67's f875 shot →
first segment 878 (0–60 u, misses), second 879 (hits in k_bullet1).

### 11.2 Spawn and spread (once per pellet, W+0x16 = 1 pellet for e027)
1. **Muzzle**: W+0xb4 = 0 (0..3) → FUN_003464b0(C, out, W+0xb4, W+0xb5 = −1) (FUN_005694f8). D
2. **Aim** (FUN_003605d8 → FUN_00360558 → FUN_00563438): shooter +0x118 bits 1 and 2 set (#66/#67
   0x10000203) → FUN_00562ff8: target = object id shooter +0x11c (0x16 = Vincent), aim = target
   vt+0x5c = **FUN_00563720**: C+0x765 ≠ −1 (Vincent: **3**) → world position of bone C+0x765
   (FUN_0054a768, matrix row 3); else FUN_005634e0(actor, out, 3). Without bits 1/2: own position +
   facing × 1000. T: aim point is inside Vincent's capsule, 13 u above his feet. D
3. **Spread FUN_005405b0** (taken when W+4 has neither 0x100000 nor 0x200000; e027 W+4 = 0x100). D
   - d = |aim − muzzle|; dir = normalize(aim − muzzle).
   - s = W+0x1c if d < T+0xd0 (**120**), W+0x20 if d < T+0xd4 (**380**), else W+0x24. e027: **25 / 25 / 25**.
     (T = *(DAT_0046dd68 + 0x14).)
   - s /= FUN_0031cf08(1.0, status+0x10) = (k0·lvl + k1)·0.01 + 1, k = T+0x268 (0.15, 2.5) when
     G+0x60 ≥ 7 (here 20), else DAT_0046e220 (0, 50). e027 lvl 0 → ÷ 1.025.
   - s = FUN_00318518(s, status): status+0xc0 ≠ 0 → × (100 − short T[0x44c + ((eq+0x10 >> 19) & 0x1e)])
     × 0.01. e027: +0xc0 = 0, unchanged.
   - FUN_00317b68(status) (status+0x64 bit 0x20 and +0x20 top nibble 2) → s × 0.5. e027: no.
   - FUN_00326500() and status+0x60 bit 2 and FUN_003251f8() → s × {1, 0.33, 0.75, 0.1}[FUN_00317cf0]
     (DAT_0058b550). e027: +0x60 = 0, no.
   - **e027: s = 24.390244°.** T (FUN_0025a058's float argument, every e027 shot).
4. **FUN_0025a058(s, dir)**: r1 = rand; e = unit axis: if |dy| ≤ |dx| or |dz| ≤ |dx| then (z if |dz| ≤
   |dy| else y) else x; p = normalize(dir × e); dir1 = rotate(dir, about p, (r1 − 0.5)·s·π/180);
   r2 = rand; dir2 = rotate(dir1, about dir, 2π·r2). Rotations are right-handed (Rodrigues) with these
   signs. T: reproduces all three logged shots to 5e-5 (test vectors below). D (code) / T (signs).
5. aim' = muzzle + dir2 × W+0x18 × 10. Then FUN_00552dd8: +0x80 = dir2, +0xc0 = +0xe0 = +0x90 =
   muzzle, +0xd0 = muzzle + dir2 × +0x15c, +0x15c = W+0x18×10 + 15 (DAT_0046ed6c; 20 = DAT_0046ed70
   when FUN_00321e20(W)), +0x130 = |+0xd0 − muzzle|, +0x12c = W+0x2c × 0.16666667 = **50 u/tick**,
   +0x124 = 0, +0x128 = owner id (short +0x0a of FUN_0055a370(actor): #66 0x2c, #67 0x2d), +0x134 = W+0x40, countdown 2.
   Flags for e027 bullets: **0x402001** (0x2000 = room probe at spawn, no 0x800 = thin bullet, no
   0x8000, no 0x200, no 0x40). D, T
- Test vectors (muzzle, aim, r1, r2 → dir2): k_bullet2 f875 (−963.7263, −173.8069, −1084.9316),
  (−1069.9387, −159.2386, −1105.2223), 0.09658, 0.79215 → (−0.9340, 0.0872, −0.3464); k_bullet1 f875
  (−963.8456, −173.0982, −1085.4814), (−1034.6635, −157.4682, −1103.1165), 0.429386, 0.731939 →
  (−0.9405, 0.2127, −0.2650); f786 (−1046.2216, −155.2970, −1098.2831), (−1074.4689, −158.2524,
  −1105.6091), 0.549953, 0.86268 → (−0.9682, −0.0870, −0.2344).
- **RNG order matters**: the same stream feeds the fire timer jitter (§9.6), initial rounds and
  Vincent's pellets. The page must share one GameRandom in the game's call order (§9.11: the MT state
  is checked against the save).

### 11.3 Per frame (FUN_005549f0, after the countdown)
1. +0x124 += step (1.2). **Move FUN_00556068**: +0xe0 = +0x90; new = +0xc0 + +0x80 × (+0x124 ×
   +0x12c); dist = |new − +0xc0|. dist > +0x130 → +0x90 = +0xd0, done (bit 0x800000); else dist >
   +0x15c → done; else +0x90 = new. Also done when +0x124 > 1800 or dist > 3000. D, T (60 u/frame).
2. **Hit test FUN_00556870 → FUN_005442d8(mgr, A = +0xe0, B = +0x90, bullet)**. A hit clears bit 1 and
   ends the bullet. No hit: e027 bullets (bit 0x2000) return 0 here, with **no per-frame wall test**
   (walls were handled by §11.4). D, T
   FUN_005442d8, in order:
   - Walk the object grid cells along A→B (FUN_0036d400(it, mgr+0x1170, 4, A, B), FUN_0036df00;
     cell list FUN_00589940). Each object needs +0x38 bit 0 and +0x28 bit 0.
   - **Filter vt+0x1ac = FUN_0055d100** (player and e027): false if bullet +0x120 bit 0x4000; obj +0x28
     bit 0x100000; target dead (FUN_00316cf8) unless the +0x28 bit 0x20 / vt+0x37c < 8-tick case;
     obj +0x188 bit 39; FUN_00552ad0(bullet). Owner +0x128 ≠ −1: owner == obj +0x100 id → false; if
     mgr+0x2b0 ≥ 0: FUN_0031d1a8(team(owner), team(obj)) = 0 (same side) → bullet bit 0x200
     (friendly fire; clear for e027), else → bullet bit 0x8000 clear. T: 0 for #66/#67, 1 for Vincent.
   - Thin bullet (bit 0x800 clear): mgr+0x1294 = bullet; **part test vt+0x17c = FUN_0055bda8(obj, 0,
     hit, nrm, A, B)**; mgr+0x1294 = 0. (Bit 0x800 bullets use the sphere path vt+0x19c / +0x184 /
     +0x3c instead; not e027.)
   - Hit distance |A − hit|. Objects with +0x29 bit 0 (or bit 0x40 bullets on actors) are pierced
     (≤ 32 kept); otherwise the nearest is kept. After the walk: pierced ones nearer than the nearest
     get vt+0x1b4; the nearest: mgr+0x10c0/0x10d0 = hit/nrm (FUN_00541b08), **r = vt+0x1b4(obj,
     bullet, hit, nrm)**, bullet +0xf0 = hit, +0x100 = nrm, FUN_00552ac8(bullet, actor id). Returns r.
3. **FUN_0055bda8, mode 0** (Vincent and e027 alike):
   - Capsule FUN_0055bc90 → FUN_0054b2b0: P0 = actor position, P1 = P0 + (0, −18, 0); radius actor
     +0x2c = **5** (+0x160 when +0x118 bit 0x1000000). T (both actors). L: the 18 comes from
     FUN_0054b2b0 (not read).
   - FUN_0025c0f8(r, hit, nrm, A, B, P0, P1) (segment vs capsule; hit = entry point); if none,
     FUN_0025c770(r, B, P0, P1) (B inside). Neither → **return 0 (miss)**. T: k_bullet2 f878/879.
   - Capsule hit, model actor+0x180: player only (+0x28 bit 4, mgr+0x1294 set): vt+0x18c =
     **FUN_00573720** deflect test → return 1 (see 4). Then **FUN_005ad118(model, A, B)** = part index
     (per-part volumes). −1, or part bit set in C+0x77c → FUN_0055df90 (near miss, no damage),
     **return 0**. Else actor +0x142 = part, +0x19c = zone record → return 1. T: f789 part 6, f879
     part 0.
4. **Vincent's handler vt+0x1b4 = FUN_005720c8**: vt+0x18c (FUN_00573720) true → deflect (FUN_005694a0
   (…, 3), cue 0x48800, FUN_0054b158; no damage). Else +0x118 bit 0x20 → no damage; +0x28 bit 0x20 →
   FUN_00571938; else **FUN_0055db50** = damage §10.2 (45 → 44, T). Then FUN_00567c08 (hit effect)
   unless bullet +0x140 bit 32. e027's handler is FUN_0055db50 directly.
   - FUN_00573720 is true when status+0x61 bit 0 is clear, now − actor +0x27c < **8** ticks
     (DAT_0046ee30), bullet bit 0x800 clear and the bullet's origin is within ±**75°** (1.309 rad,
     DAT_0046ee34) of his facing. Its setter (+0x27c: FUN_0056de80 / 0056ec08 / 00571ee8 / 00573360)
     is **not decoded**; it was false in both traces. Leave it out.

### 11.4 Room probe (once, at countdown end; FUN_00554970 → FUN_00553808, bullet bit 0x2000)
Unless bullet bit 0x20: ray from +0xc0 to +0xd0 + dir × 0.1 against the stage object
(FUN_0053bb58: object index mgr+0x1c → vt+0x17c = **FUN_0054fc38**, probe index 0 → class mask
**0x18d** (DAT_0058c990[0]) → FUN_0026a320(A, B, mask)). Hit → +0xd0 = +0xf0 = +0x110 = hit point,
+0x100 = normal, +0x130 = |hit − origin|; the move then stops there. D. T: k_bullet2 f875 bullet
stops at 362 u (x −1302), short of its 515 u range. The mesh ray is the one the walk probes use
(notes/actions-research.md §2); FUN_0026a320 itself isn't read here.

### 11.5 Translation order for the page (replaces BULLET_R)
1. Spawn (11.2): muzzle, aim = Vincent's bone 3, spread with the shared GameRandom (2 draws), +0xd0
   end, countdown 2.
2. Countdown end: stage ray mask 0x18d → shorten +0xd0 / +0x130.
3. Per frame: move (11.3.1), then for each object along the segment: filter FUN_0055d100, capsule
   (P0, P0 + (0,−18,0), r 5), part ray FUN_005ad118 on the model's part volumes; nearest hit →
   FUN_005720c8 → FUN_0055db50.
4. Not decoded, keep as labelled stand-ins: FUN_005ad118's per-part volumes ('mkd'/'skd', §9.10)
   (a capsule-only hit would hit more often than the game; the near misses in §9.10 show parts
   matter); FUN_0026a320 (use the page's existing walk-mesh ray if it is a translation); the deflect
   window setter.

Call log: `TRACE_CALLLOG=bullet` / `spread` (tools/trace/calllog.py; frames via
`TRACE_CALLLOG_FRAMES=780:900`), scripts `k_bullet1` (= engage7 inputs, to f900) and `k_bullet2`
(= engage8 inputs). Out: tools/trace/out/k_bullet*.calls.jsonl and out/spread/.

## 12. Facing: the awake soldier's body turn (worker 0128, 2026-10-30)

Dan's report (20261030T100134Z-191d): soldiers in the wrong place, not aiming, no rifles. **Position
was right**: page vs k_enemy_engage7 body C+0x30 for #66 (-1038.978, -146.000, -1100.470) and #67
(-953.915, -160.370, -1084.805): 0.000 both (matrix also exact vs the save, enemy-placement.md);
#67 is on the walkway, 12.3 u from his Vincent. The page never turned them (drawn yaw stayed at the
placement: #67 154.37 deg while the game turns it to about -102).

**Turn (D, T).** setTarget FUN_00562f50 -> FUN_00342608(C, target): C+0x6c2 = target id, C+0x564
|= 0x40000000 (trace cflags 0x40000400 from the wake). Every frame FUN_00343160 (motion update) with
that bit -> FUN_00342650: target vt+0x5c point -> FUN_003424c0(C, p): C+0x670 = p and, while
C+0x214 = 6, C+0x2b0 = p (+ row 0 x stance offset only with C+0x564 bit 0) through FUN_0034b5e0
(inverse parent frame; static room = world). FUN_0034ea18 -> FUN_0034de50 case 6: C+0x2c0 =
unit(C+0x2b0 - C+0x30) (3D), C+0x2cc = 0.5; LAB_0034e2f0: face . dir <= -0.97 -> dir = row 0
(C+0x1c0 bit 0x40 clear), face += (dir - face) x FUN_00323318(0.5) (= 0.5 at step 1.2), unit; then
FUN_0034a5a8(C, 1) rebuilds the rows from up (0,-1,0) (face loses its y). C+0x45c bit 21 would take
the rate-limited FUN_0025a560 branch instead (not taken: the lerp fits). Open loop vs the traces:
#66 median 0.30 (engage8), #67 0.08; with a (0.4, -0.1) offset of Vincent's point in his frame #66
engage7 0.93 -> 0.06 deg, i.e. the residual is the vt+0x5c GUESS (PLAYER_AIM_H). enemy_ai.js
`turnBody` / `setAim`; test_enemy_ai 'facing' (closed loop) and fixed-input turnBody checks;
mutants: no turn, no per-frame look, rate 0.3, no flatten, setTarget without bit 30 all fail;
'Vincent's post-move point' only moves the engage7 #66 gap.

## 13. Vincent's bullets vs the soldiers (worker 0139, 2026-10-30; trace `k_hits12`)

`k_hits12` = k_enemy_engage12 cut to 900 frames (identical records) with `TRACE_CALLLOG=spread` on 835:900.
Translated in enemy_ai.js / enemy_hit.js; test_enemy_hits.mjs. D = code, T = trace.
- **Shot** FUN_00577480: muzzle FUN_00341ec8 = marker 15 (j56, offset 0); FUN_00563148 pulls it back
  by 0.8 × |muzzle − marker 1| along the shot (marker 1 = bone 5, offset 0; FUN_00342000). Per pellet
  i < W+0x16 (3): FUN_005408e8(mgr, 0, muzzle, aim, W, status, **i**, 0) → FUN_005405b0 spread → FUN_00552dd8
  with countdown **i + 2**: the pellets fly one frame apart. D. Pellets are updated in their spawn
  frame (R1 840 → #66 hit in record 843; R1 866 → 870 / 871). T
- **Spread** FUN_005405b0: s = W+0x1c / +0x20 / +0x24 (Vincent's composed W: **4.8 / 3.0 / 13.5**) for
  |aim − muzzle| < 120 / < 380 / beyond, ÷ (0.15 × status+0x10 + 2.5) × 0.01 + 1 (Vincent +0x10 = 2 →
  1.028), then FUN_0025a058 (§11.2). Logged 4.66926° at 31 u and 111 u (locked aim); engage8's 2.918°
  = 3.0 / 1.028 (its aim point lay > 120 u away). Draw order r1, r2 per pellet. T
- **Part boxes** FUN_005ad118 → FUN_0030baa0: the 'mkd' chunk ('mk2') has two arrays: +4 count / +8
  markers (16 B: xyz, bone) and **+6 count / +0xc boxes (32 B: offset xyz, bone, scale xyz)**. A box is
  the unit cube (±1, DAT_0046dc10; faces DAT_0046dc90) × diag(scale) + offset, × the posed bone
  (FUN_005b29c0) × MP+0x10. Segment vs each face plane, point-in-quad FUN_00259580, smallest t wins.
  e027: 13 boxes = the 13 zone records (0 head bone 6, 1 / 2 chest / pelvis, 3-6 arms, 7-12 legs). D.
- **Cylinder** FUN_0055bda8 mode 0: e027 has no actor +0x28 bit 0x20 (0x208403), so plain FUN_0054b2b0
  (feet .. 18 up, r 5); FUN_0025c0f8 (segment crosses the closed cylinder) else FUN_0025c770 (segment
  start inside). D. Dead soldiers are not tested (AI +0x70 stops at the death, T; filter FUN_0055d100 §11.3).
- **Reaction** (corrects §10.4): the push-back FUN_003531e0(4, 4) and the death slide FUN_00353340(12, 16)
  run when actor +0x28 bit 0x20 is **clear** (e027). They set C+0x310/0x350 dir, speed 2d/T, decel speed/T;
  mover 9 (FUN_00351550) / 0xb (FUN_003516a0) hands speed to FUN_0034af98 (C+0xc0 / +0xcc), moved by
  FUN_0034c388, then speed −= decel × step. T: #67 push-back 2.400 u in the hit frame, death slide 1.800,
  1.665, … 0.045 (12.9 u). FUN_00561718's 12 / 6-frame throttle draws one FUN_0040b5c0. D
- **Settled** (+0x118 |= 0x100) when the death clip's clock (0.6 / frame) passes its frame count:
  r_stlaf0 32 → +53 (f896), r_stbf0 70 → +117 (f988). T
- **Zones** (worker 0140): status +0xa8 = FUN_00310e40(parameter set +0x84) = section-20 base +
  section-19 [i] (FUN_003153d0); its +0x30 = entry + u32 +8 + u32 +4 × 4 + u32 +0 (FUN_0030fc68, all
  section-19 entries at stage load, FUN_0030f968). z128 e027 parameter sets use index 6 or 7 (#66 / #67:
  7); both give the RAM zones exactly. Exported as ai.json `zones`. D
- **Frame order** (worker 0140): FUN_00323128 = FUN_0033ab78 (DAT_0046e590), FUN_00365008 (AI scheduler + fire
  timers: jitter draws), FUN_0053b500 (player vt+0x214 of vtable 0x58f128 = FUN_005747a0 → FUN_005786e8
  → FUN_00579570 → FUN_00577480: cone draws; then the FUN_005810e8 object lists: bullets, flinch draw). D
- **HP** stays on the status across deactivate / reactivate: FUN_00314ea8 runs only from placement setup
  (FUN_00315050 / FUN_003151c8 via FUN_003153d0); no callee of FUN_00559748 writes +0x20. D
- Open: the clip choice of classes 0x1e / 0x36 (traced r_stlld0 = part 10, r_stlaf0 = part 6, r_stbf0
  = head); limb 24 vs 25 (§10.2).
- **Lock-on** (worker 0141): the aim points of these shots were the lock-on's (CAM+0x1e4 = 0x42 / 0x43):
  bone 3 of the target (vt+0x5c FUN_00563720, C+0x765 = zone header +0x14). On death +0x38 goes 0x387 →
  0x183 (probe 2 / lockable bit cleared; k_lock14 f836). Decode and page: aim-research.md §4.1, lock_on.js.

