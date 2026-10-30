# z128 enemy activation after the save, and whether woken soldiers move (worker 0133, 2026-10-30)

Research only; no code changed. Sources: stage file `extract/_KEL/noPath/06800001.bin` read at run
time (scripts = sections 2/3, triggers = section 8, placements = section 1), analysis/decomp, the
ELF vtables, and the existing `tools/trace/out/k_enemy_engage*.jsonl` / `k_bullet*.jsonl`. No new
emulator runs (none were needed: both questions are answered from code + data + existing traces).
D = decoded from code/data, T = matches a trace, L = likely, G = guess. Builds on
notes/enemy-research.md §1, §7, §8, §9.

## 1. How a record becomes active (D)

- **Only object command 0x108 activates** an actor after stage load: FUN_0054cd70 case 0x108 →
  vtable +0x144(1, 0) (= FUN_00559748 for actors), then queues manager event 7 if the actor is
  active. 0x109 / 0x130 deactivate (+0x144(0, 0)); the e027 handler FUN_005661e8 has its own 0x130
  (also clears the status, FUN_00317420). No other command in FUN_005661e8 / FUN_0054cd70 calls +0x144.
- Scripts name the target with a **manager id lookup** (FUN_00545b20), then send 0x108 to that id:
  | cmd | table (manager +) | filled by | indexes |
  |---|---|---|---|
  | 0x3a, 0x3f | +0x3b2 | stage load | section-1 record (actors / NPCs) |
  | 0x3d | +0x7b0 | FUN_00547f50 (section 8) | trigger volume |
  | 0x3e | +0x5b2 | FUN_00548348 (section 15, 146 × 0x90) | props / gimmicks |
  | 0x44 | +0xfac | FUN_00548788 (section 22, 34 × 0xc0) | other placed objects |
  | 0x3b | +0x2b4 | — | not checked |
- **Section-1 +0x04 (s16) is the record's script index** (§1 called it "group id"): matches all
  seven known pairs (44→108, 45→109, 46→127, 47→128, 48/49→126, 66→92).
- **Triggers can start unarmed.** FUN_00550ad8 activates a trigger object unless its flags (+0xa4)
  have **bit 17 (0x20000)**. 26 of the 34 triggers with that bit are exactly the ones some script
  arms with 0x108 (D for the code; L for FUN_00550ad8 being the stage-start pass — its caller is a
  vtable slot, not traced).
- **Difficulty gates.** Manager 0xa6 / 0xa7 = FUN_00242c28() == 2 / 3. In this save FUN_00242c28()
  is 1 (worker 0125, §9.11), so every `diff2` / `diff3` branch below is **skipped** on the page's
  save. The extra soldiers / triggers they add (164–175, 184/185, 205/208/209/211–216, …) exist only
  on harder difficulties.
- **Event flag 0 = 0 in the save** (evt_flags XOR evt_key in k_enemy_engage7: only flag 2 = 7 and
  flag 46 = 1 are set). Stage controller script 0 runs event 0x22 / 0x23 (stage start / restart: L)
  and branches on flag 0; for flag0 < 2 it activates **rec 44** (unless manager 0xa4) and, on
  diff3 only, arms triggers 205/208/209. At flag0 == 2 it deactivates 44–49 (0x130). So the save's
  44 comes from script 0; **how 45–49 (and 66) were activated is not found** — no 0x108 names them
  and they lack +0x40 bit 3 (one attempt; open).
- **Section 21 is not an actor cap.** It is a model streaming set: trigger +0xb0 / manager cmd 0x15
  → FUN_003114d0 → FUN_00408ea8 (load request); FUN_00550cc8 checks it is resident
  (FUN_00408cd8). Its "e027 ×3" can't limit live actors: the save already has 7 live e027s (plus 3
  dead). Per-group rules are in the scripts instead (manager shorts via 0x30 / 0x2f, actor vars
  0x104 / 0x105, e.g. "once": trigger var0 set on first entry).

## 2. What activates after the save (normal difficulty, flag0 = 0)

The full per-trigger tables (positions, half-box sizes, the records' placements) come from the
stage file, so they are not copied here (public repo). Regenerate them with
`python3 -I tools/stage_events.py extract/_KEL/noPath/06800001.bin --activations`.
"ev d" = enter (FUN_005517a8, flag bit 0); "ev 0x12" = flag-0x20 volumes (condition not decoded,
§8). "wake n" = 0x101 message n to the actor (§8: lands as its script event 2, L).

**On the start deck / the route east (the ladder area):**

| trig | script / event | effect (normal difficulty) |
|---|---|---|
| 56 | 91 ev d, once | **0x108 rec 50, 51** (e027); wake n1 → 41–43 (dead). diff3: + 164–172 |
| 95 | 27 ev d, once | wake n0 → 44 (#66), 45 (#67) (traced, §8) |
| 75 | 98 ev 0x12, once | wake n1 → 44, 45 |
| 89 | 110 ev d, once | wake n1 → 46–49 (e027) |
| 77 | 100 ev 0x12, once | wake n1 → 46–49 (204 shares the script; unarmed) |
| 76 | 99 ev 0x12 | wake n2 → 50, 51; wake n1 → 46–49 |
| 90 | 129 ev d, once | wake n2 → 50, 51 (205 shares it; unarmed) |
| 78 | 101 ev 0x12 | wake n1 → 66 (id 88) |

**Street level below / west of the deck:**

| trig | script / event | effect |
|---|---|---|
| 96 | 23 ev d | **0x108 rec 69, 70 (e027), 71 (e030)** + wake n1; also NPCs 111/113/114 |
| 102 | 71 ev d, once | wake n0 → 69–71 |
| 100 | 18 ev 0x12 | wake n0 → 69–71 |
| 63 | 34 ev d, once | **0x108 rec 59, 60, 61 (e027), 64, 65 (e030)**; NPCs 131–133. diff3: + 173–175 (e048) |
| 206 / 207 | 90 ev d / 70 ev 0x12 | wake n1 → 59, 60, 65 (207 unarmed) |
| 68 | 70 (fl 0x23) | **0x108 rec 50, 51, 88–92 (e027), 93 (e030)** |
| 129 | 169 **ev 1** | 0x108 + mode → 55, 56, 58 (e027); ev d: NPCs 111/113/114/125. Event 1's trigger is not decoded |
| 66 / 178 | 63 ev d / 235 ev 0x12 | wake n0 → 88–93 |
| 149 | 192 ev d, once | 0x108 rec 150 (e048) |
| 186 | 240 ev d | 0x108 rec 50, 51, 56, 57, 88–93, 147, 149, 150 (re-sends to active ones are no-ops, §6) |
| 71 / 137 | 86 / 180 ev d, once | 0x108 rec 87 (e023) |
| 57 | 76 ev d, once | 0x108 + mode → 67, 68 (e027, **mode 1**) |
| 74 | 166 | unarmed (0x20000) on normal |

Further triggers (58, 59, 64, 67/70, 32, 31, 85, 88, 22, 128, 163, 166, 211, 213, 217, 139–143 …)
follow the same pattern; regenerate them rather than copying.

**Waves / respawn (D for the shape, not traced):** scripts 104 (rec 72–77), 124 (33–38), 159–164
(81–86), 190 (138–143) and 223 (152–157) send **0x108 to themselves** on message n1 when a manager
counter (0x30 short) is in range (≤ 4 / ≤ 5) and object command 0x177 returns true — i.e. pools that activate one
at a time from the same spot. All are > 1500 u from the save. 203–211 share one placement
and are activated by trigger 213, which is itself unarmed (0x20000) until script 0 arms it at flag0 == 24 on diff3.
The meaning of 0x177 and of each counter: not decoded.

## 3. Do the woken ladder-top soldiers (#66 / #67, recs 44 / 45) move? **No.**

- **Code (D).** Mode 2 brain update FUN_00362ef8 state 0 calls **FUN_0035f540(ai, 0)** →
  FUN_0035f590 default case: no move layer (AI+0xe4 = 0, AI+0xd4 = 0). No other branch of the
  mode-2 brain sets one. The move layers (FUN_0035f590: 1 vt 0x4a2a28 upd FUN_00366228, 2 0x4a29f8
  FUN_00366390, **3 0x4a29c8 FUN_003667b0**, 4 0x4a2998 FUN_00366ee0, 5 0x4a2968 FUN_003670e8,
  6 0x4a2938 FUN_003672d8, 7 0x4a2908 FUN_00367508, 0xb 0x4a28d8) are requested only by brain
  mode 5 (FUN_003639d0: layers 2, 3, 5, 6), mode 6 (FUN_00364558: 5, 7), mode 7 (FUN_00363350:
  5, 1), and by script cmd **0x19f** (FUN_005661e8 → FUN_0035f540(ai, r0)). Modes 0, 1, 2 set 0.
  Brain mode 3 (FUN_00361690) calls no layer setter (it drives actor states via FUN_0055b8e0).
- **Scripts (D).** 108 / 109 never send 0x19f. #66's script 108 sets mode **5** only (a) in event 8
  (hit reaction) when manager short 15 ≠ 0 — short 15 is set by trigger 25's script 139, far away —
  which also activates recs 56 / 57; and (b) in event 2 with r0 == 5, but no script sends #66
  message 5. #66 has HP 1, so its first part hit kills it (death → modes 0, FUN_0035f728). #67's
  script 109 only ever sets mode 2 (or 0).
- **Traces (T).** In engage7–12 and k_bullet1/2 (~235 frames after the f784 wake each), body
  C+0x30 of #66 and #67 moves **0.000 u** while alive. The only movement is the hit knock-back of #67
  (2.4 u, engage12, §10) and the death slides (FUN_00353340, up to 15 u), both already decoded.
- So the page's standing-and-turning soldiers (commit c2605e9) are faithful for 44/45. Movement
  matters for other groups: recs 20, 21, 67, 68, 94, 97 (mode 1) and 181/182 (e030, mode 3); and
  anything put in mode 5–7.

## 4. Worker-sized jobs

**A. Activation export + page (one worker).**
1. `tools/stage_events.py --activations` (or a new exporter next to export_web_placements.py):
   resolve the target of each 0x108 / 0x101 / 0x143 / 0x130 by tracking `push_i k; pop_reg 0;
   push_i -2; send 0x3a|0x3f` → section-1 record k (0x3d → trigger k), keep the innermost guards,
   and evaluate the guards the page can know at export time: diff2/diff3 = false (save value 1),
   flag0 = 0, own var0 "once" = runtime. My scratch version of this resolver produced the tables
   above (≈80 lines; same symbolic pass as stage_events.actions plus the table names in §1).
2. Generalise export_web_triggers.py: export every trigger (not just 95) with box, script, flag
   bits 0 / 1 / 0x20 / 0x20000 and its resolved actions; generalise enemy_ai.js's trigger handling
   to: enter/exit events, "once" via trigger var0, 0x108 activate (load placement + body + AI at
   mode = section-1 +8), wake-n messages (event 2: var7 = 1, 0x193 + 0x143 mode from the
   record's script), unarmed triggers stay off until a 0x108 arms them.
3. Page needs the e027 model per record (already there), e030 / e048 models (not exported:
   separate job; until then leave those records out, don't substitute e027).
4. GUESS to list: 0x101 n → script event 2 (L only), flag-0x20 volume condition (ev 0x12) — leave
   ev-0x12-only triggers (75, 76, 77, 78, 100, 207, 178) out until decoded, or label them GUESS.
5. Check against a new trace: run east from the save across trigger 56 and log recs 50 / 51
   (needs `find_enemies` to pick up actors activated after load, not only at load).

**B. Movement (later, separate).** Translate brain mode 1 (FUN_00362998), mode 3 (FUN_00361690)
and the mode-5 brain FUN_003639d0 with move layers 2 / 3 / 5 / 6 (update fns above), checked by a
trace of recs 67 / 68 (trigger 57) or 20 / 21. Not needed for the ladder-top pair.

## 5. Open (stopped, one attempt each)
- What activated 45–49 and 66 before the save (no 0x108 names them; not stage-load bit 3).
- Script event 1 (trigger 129), manager cmd 0xa4 and object cmd 0x177 meanings.
- Whether a 0x108 to an already-active actor is a no-op (FUN_00559748 with param 1 on an active
  actor: not read).

## 6. Job A done (worker 0134, 2026-10-30)
- `tools/stage_events.py STAGE --activations` prints every trigger's resolved 0x101 / 0x108 / 0x109 /
  0x130 / 0x143 for the save (difficulty 1, flags 2 = 7, 46 = 1); export_web_triggers.py puts the same
  list in ai.json (`triggers[k].acts`), plus `records[k].mode` (+8) and script 0.
- enemy_ai.js: record objects for all 221 records, `setActive` = FUN_00559748 / trigger +0x144,
  manager 0x3d / 0x3f / 0xc / 0xa6 / 0xa7, triggers with bit 0x20000 start off. Callbacks
  `onActivate(record, actor|null)`, `onDeactivate(record)`, per-frame `activated` list.
- §5 answered: **0x108 to an already-active actor is a no-op** (FUN_00559748 returns when
  param_2 == +0x28 bit 0); a dead status is refused; 0x108 passes param_3 = 0, so event 3 does not run.
- Script 0 event 0x22 runs in the VM until op 37 (push_data_ref, not translated) and skips manager
  0x1e / 0x3e / 0xa4 / 0x35 / 0x38; the page does not run it yet.
