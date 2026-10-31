#!/usr/bin/env bash
# build_web.sh -- build the browser prototype's data (native/web/data) from YOUR disc image.
#
#   tools/byo/build_web.sh /path/to/your.iso
#
# Bring your own disc: nothing from the game is in this repository. This script reads your
# image, unpacks the game archive into <repo>/extract/ and writes the exported scene into
# native/web/data/ and native/assets/models/out/byo/. All three are gitignored. Never commit
# or upload them, the image, or anything made from them.
#
# Supported disc: Dirge of Cerberus: Final Fantasy VII, PAL SLES-54185 (Europe, Australia).
# Prerequisites: python3 (3.8+) with numpy and Pillow (OS packages or a venv: the build runs
# python3 -I, which ignores pip --user installs), a C compiler (cc/gcc/clang), make, git.
# Node.js is only needed for the optional tests. No zlib, no Docker, no PCSX2.
# Disk: about 3.5 GB free (1.5 GB archive copy, deleted after unpacking; ~1.2 GB unpacked).
#
# Re-running is safe: a verified extract is reused, outputs are overwritten in place.
#
# Options:
#   --re-extract   unpack again even if extract/ verifies (only replaces an extract this
#                  script made; it never deletes an extract/_KEL it did not create)
#   --no-motions   skip the motion catalogue (data/vincent/motions.json, reference only)
#   -h, --help
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BYO=$REPO/tools/byo
EXT=$REPO/extract
KEL=$EXT/_KEL
NOPATH=$KEL/noPath
STAMP=$KEL/.byo-extract
WORK=$EXT/.byo-work
MODELS=$REPO/native/assets/models
OUT=$MODELS/out/byo
LOGS=$OUT/logs
DATA=$REPO/native/web/data

# Clip packs play.html loads (native/web/ACTIONS.md). Order matters only for byte-identity.
# Every clip comes from the container z128 loads (07703200 common, 0770320b handgun; FUN_005a7390,
# export_web_vincent.py CLIP_SETS). jump: the plain set, the aimed set h_jp*_a (FUN_00598f50
# phases 4-7, the landing FUN_00599aa0, the air shot FUN_00596830) and the second / back jumps
# (c_wjp*, c_bj*, c_bwj*). The _au/_ad partners aren't played.
PACK_JUMP=h_jp0,h_jp1,h_jp2,h_jp3,h_jp4,h_div,h_jp0_a,h_jp1_a,h_jp2_a,h_jp3_a,h_jp4_a,c_wjp0,c_wjp1,c_wjp2,c_bj0,c_bj1,c_bj2,c_bwj0,c_bwj1,c_bwj2
PACK_LADDER=c_ld_d,c_ld_u,c_ld_jp,c_ld_fa0,c_ld_lid,c_ld_rid,c_ld_del,c_ld_uel,c_ld_dml,c_ld_uml,c_ld_der,c_ld_uer,c_ld_dmr,c_ld_umr,h_ld_del,h_ld_uel,h_ld_der,h_ld_uer
PACK_SHOOT=h_wk_a,h_rn_a,h_sq_a,h_st_a,h_sq_f,h_st_f,h_wk_ad,h_rn_ad,h_sq_ad,h_st_ad,h_sq_fd,h_st_fd,h_wk_rd,h_rn_rd,h_sq_rd,h_st_rd,h_swtch,h_wk_au,h_rn_au,h_sq_au,h_st_au,h_sq_fu,h_st_fu,h_fwk_rd,h_sqm_rd,h_frn_rd,h_brn_a,h_brn_au,h_brn_ad,h_lrn_a,h_lrn_au,h_lrn_ad,h_rrn_a,h_rrn_au,h_rrn_ad
# GUESS: the static Vincent in index.html is clip h_st frame 91, the clip frame nearest the hero
# capture (not decoded; see docs/BROWSER.md "Known limitations" and NODUMP_PLACEMENT).
STATIC_CLIP=h_st
STATIC_FRAME=91

ISO=""; REEXTRACT=0; MOTIONS=1
usage() { sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case "$1" in
    --re-extract) REEXTRACT=1 ;;
    --no-motions) MOTIONS=0 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "unknown option $1" >&2; usage >&2; exit 2 ;;
    *) [ -z "$ISO" ] || { echo "one image only" >&2; exit 2; }; ISO=$1 ;;
  esac
  shift
done
[ -n "$ISO" ] || { usage >&2; exit 2; }

# Don't silently replace a web build that this script didn't make (e.g. a capture-based
# developer build, which has the sky and the capture placement). BYO_FORCE=1 overrides.
if [ -f "$DATA/scene.json" ] && [ ! -f "$DATA/.byo-build" ] && [ "${BYO_FORCE:-0}" != 1 ]; then
    echo "error: $DATA already holds a web build not made by this script." >&2
    echo "       Move it aside, or rerun with BYO_FORCE=1 to replace it." >&2
    exit 1
fi

T0=$(date +%s)
step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nbuild_web.sh: FAILED: %s\n' "$*" >&2; exit 1; }
# run NAME CMD...: log to $LOGS/NAME.log, show the tail on failure
run() {
  local name=$1; shift
  if ! "$@" > "$LOGS/$name.log" 2>&1; then
    echo "--- last lines of $LOGS/$name.log:" >&2
    tail -n 25 "$LOGS/$name.log" >&2
    fail "step '$name' (full log: $LOGS/$name.log)"
  fi
}
PY="python3 -I"

# ------------------------------------------------------------------ 0. prerequisites
step "checking prerequisites"
missing=()
command -v python3 >/dev/null || missing+=("python3 (3.8 or newer)")
command -v git >/dev/null || missing+=("git")
command -v make >/dev/null || missing+=("make")
CC_FOUND=""
for c in "${CC:-}" cc gcc clang; do
  [ -n "$c" ] && command -v "$c" >/dev/null && { CC_FOUND=$c; break; }
done
[ -n "$CC_FOUND" ] || missing+=("a C compiler (cc, gcc or clang)")
[ ${#missing[@]} -eq 0 ] || fail "missing: ${missing[*]}. Install them and run again (docs/BROWSER.md)."
$PY -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' ||
  fail "python3 is $(python3 -V 2>&1); 3.8 or newer is needed"
$PY -c 'import numpy' 2>/dev/null || fail "python3 -I sees no numpy (OS package python3-numpy, or pip install inside a venv; a pip --user install is not seen)"
$PY -c 'import PIL' 2>/dev/null || fail "python3 -I sees no Pillow (OS package python3-pil, or pip install inside a venv; a pip --user install is not seen)"
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || fail "$REPO is not a git checkout (clone the repo; the exporters use git check-ignore)"
# probe.md: .md is on the allow-list, so it is only ignored by the directory rule itself;
# the other names are the kinds of file the build writes there
for d in extract extract/_KEL/noPath native/web/data native/web/data/vincent native/assets/models/out/byo; do
  for f in probe.md probe.bin probe.json probe.glb probe.rgba probe.png probe.log; do
    git -C "$REPO" check-ignore -q "$d/$f" ||
      fail "$d/$f would not be gitignored: refusing to write game data there. Restore .gitignore (git checkout .gitignore)."
  done
done
[ -f "$ISO" ] || fail "no such file: $ISO"
echo "python3 $($PY -c 'import sys,numpy,PIL; print(sys.version.split()[0], "numpy", numpy.__version__, "Pillow", PIL.__version__)'), cc=$CC_FOUND, make, git: ok"
command -v node >/dev/null && echo "node $(node -v): ok (optional, for tests)" || echo "node: not found (optional, only for tests)"
mkdir -p "$LOGS"

# ------------------------------------------------------------------ 1. the disc
step "checking the disc image"
$PY "$BYO/disc.py" check "$ISO" || fail "unsupported or damaged disc image (see above)"

# ------------------------------------------------------------------ 2. unpack
step "unpacking the game archive into extract/ (gitignored)"
have_extract=0
if [ -d "$NOPATH" ] && [ $REEXTRACT -eq 0 ]; then
  if $PY "$BYO/disc.py" verify "$NOPATH" >/dev/null 2>&1; then
    have_extract=1
    echo "extract/_KEL/noPath verifies: reusing it (--re-extract to unpack again)"
  elif [ ! -f "$STAMP" ]; then
    $PY "$BYO/disc.py" verify "$NOPATH" | tail -n 5 || true
    fail "extract/_KEL exists, was not made by this script, and does not match the supported disc. Move it away (it is not deleted automatically) and run again."
  fi
fi
if [ $have_extract -eq 0 ]; then
  if [ -d "$KEL" ] && [ ! -f "$STAMP" ]; then
    fail "extract/_KEL exists and was not made by this script; move it away first (not deleted automatically)"
  fi
  df_kb=$(df -Pk "$REPO" | awk 'NR==2 {print $4}')
  [ "${df_kb:-0}" -ge 3500000 ] || fail "need about 3.5 GB free on $(df -P "$REPO" | awk 'NR==2 {print $6}'), have $((df_kb / 1024)) MB"
  rm -rf "$WORK" "$KEL.tmp"
  run disc_extract $PY "$BYO/disc.py" extract "$ISO" "$WORK"
  grep 'KEL.DAT  ' "$LOGS/disc_extract.log" | sed 's/^/  /'
  echo "  unpacking KEL.DAT (a few minutes)"
  run kel_unpack $PY "$REPO/tools/kel_unpack.py" "$WORK/FILELIST.BIN" "$WORK/KEL.DAT" "$KEL.tmp" bin
  tail -n 1 "$LOGS/kel_unpack.log" | sed 's/^/  /'
  grep -q ' fail=0$' "$LOGS/kel_unpack.log" || fail "kel_unpack reported failures (see $LOGS/kel_unpack.log)"
  $PY "$BYO/disc.py" verify "$KEL.tmp/noPath" || fail "the unpacked files do not match the supported disc"
  date -u +%Y-%m-%dT%H:%M:%SZ > "$KEL.tmp/.byo-extract"
  rm -rf "$KEL"
  mv "$KEL.tmp" "$KEL"
  rm -rf "$WORK"
  echo "  extract/_KEL ready ($(du -sh "$KEL" | cut -f1))"
fi

# ------------------------------------------------------------------ 3. native tools
step "building the native tools (dtex texture decoder, cidweb collision exporter)"
run make_dtex make -C "$REPO/native/assets/textures" CC="$CC_FOUND"
run make_cidweb make -C "$REPO/native/assets/collision" CC="$CC_FOUND" cidweb
echo "ok"

# ------------------------------------------------------------------ 4. exports
cd "$MODELS"
mkdir -p "$DATA" "$OUT"
# mark the data as this script's before the first write, so a re-run after a failed
# step passes the guard at the top
touch "$DATA/.byo-build"
step "room z128: 13 geometry chunks, textures from the files, fog from the BMD env block"
run room $PY export_z128_room.py -o "$OUT/z128_room/room"
echo "ok ($(grep -c . "$OUT/z128_room/room.list") lines in room.list; no sky dome: it only exists in a GS capture)"

step "static Vincent for the viewer (clip $STATIC_CLIP frame $STATIC_FRAME, placement GUESS)"
run p000_static $PY export_p000_gpu2.py --no-dump "$OUT/p000_static" --clip "$STATIC_CLIP" --frame "$STATIC_FRAME"
echo "ok"

step "scene.json + room/Vincent meshes and textures -> native/web/data"
run web_room $PY export_web_room.py --room "$OUT/z128_room/room.list" --char "$OUT/p000_static" -o "$DATA"
tail -n 1 "$LOGS/web_room.log"

step "collision (DCOL v2 with the walk mesh)"
run collision make -C "$REPO/native/assets/collision" CC="$CC_FOUND" KEL="$NOPATH" z128web
cp "$REPO/native/assets/collision/out/web/z128_collision.bin" "$DATA/z128_collision.bin"
echo "ok"

step "ladders (stage file section 8)"
run ladders $PY export_web_ladders.py "$NOPATH/06800001.bin" "$DATA/z128_collision.bin" "$DATA/z128_ladders.json"
echo "ok"

step "Vincent for play.html: skinned glb + walk/run/idle clips + cape cloth data"
# the default export also runs its cloth step (export_web_vincent.py cloth): data/vincent/
# cloth.json, the cape's cloth group, skin and clip windows for native/web/cloth.js
run vincent $PY export_web_vincent.py export --no-dump
[ -s "$DATA/vincent/cloth.json" ] || fail "data/vincent/cloth.json missing after the vincent export (log: $LOGS/vincent.log)"
# the locomotion clips' local TRS (player.js builds the pose the game's way, FUN_005a9db0)
[ -s "$DATA/vincent/vincent_local.bin" ] || fail "data/vincent/vincent_local.bin missing after the vincent export (log: $LOGS/vincent.log)"
grep 'cloth.json' "$LOGS/vincent.log" | tail -n 1 | sed "s#$REPO/##" | cut -c1-160
echo "ok"

step "Vincent's light rig (env slot + room BMD light grids -> data/vincent/light.json)"
run light $PY export_web_light.py "$DATA/vincent"
tail -n 1 "$LOGS/light.log" | sed "s#$REPO/##"
for p in jump ladder shoot; do
  case $p in jump) clips=$PACK_JUMP ;; ladder) clips=$PACK_LADDER ;; shoot) clips=$PACK_SHOOT ;; esac
  step "clip pack: $p"
  run "pack_$p" $PY export_web_vincent.py export --no-dump --clips "$clips" --pack "$p"
  # the pack clips' local TRS (vincent_<pack>_local.bin, same format as vincent_local.bin)
  [ -s "$DATA/vincent/vincent_${p}_local.bin" ] || fail "data/vincent/vincent_${p}_local.bin missing after the $p pack export (log: $LOGS/pack_$p.log)"
  tail -n 1 "$LOGS/pack_$p.log" | cut -c1-160
done

step "Vincent's gun (Cerberus)"
run gun $PY export_web_gun.py --no-dump "$DATA/vincent"
echo "ok"

step "enemy e027 (z128 soldier): skinned glb per variant + r_gst idle -> data/enemies"
# clips: r_gst idle, r_gst_lk, r_st_a / r_st_f (enemy_ai.js), r_stlld0 / r_stlaf0 / r_stbf0 (hit / death, notes/enemy-research.md §10);
# the m_* rows of stance 0x1e (class-2 weapons, FUN_00564228) for the same motions (ai.json model_clips lists them);
# r_gwk / r_grn (+ m_*): the walk / run rows 9 / 0x10 of the locomotion table (motion state 8, path walking);
# r_sq_a / r_sq_f / m_sq_*: crouched aim / fire (brain mode 1's posture change, motions 0x78 / 0x7b);
# r_atk_0 / m_atk: the attack tables' play-clip commands (zone sets 7 / 6, ai.json attacks)
run enemy_e027 $PY export_web_enemy.py export --model e027 --out "$DATA/enemies" --clips r_gst,r_gst_lk,r_st_a,r_st_f,r_stlld0,r_stlaf0,r_stbf0,m_gst,m_st_a,m_st_f,m_stlld0,m_stlaf0,m_stbf0,r_sq_a,r_sq_f,m_sq_a,m_sq_f,r_atk_0,m_atk,r_gwk,r_grn,m_gwk,m_grn
[ -s "$DATA/enemies/e027.glb" ] || fail "data/enemies/e027.glb missing after the enemy export (log: $LOGS/enemy_e027.log)"
grep '^wrote' "$LOGS/enemy_e027.log" | sed "s#$REPO/##" | cut -c1-160
echo "ok"

step "z128's other enemy types (stage section 1: e048, e030, e036, e023, e024, e001) -> data/enemies"
# Same decode as e027; one idle clip each (GUESS which idle the game starts: enemies.js IDLE, notes/enemy-model.md),
# e048 / e030: the clips of the motions enemy_ai.js requests (their stance rows and attack tables, ai.json model_clips)
E048_CLIPS=r_gst,r_gst_lk,r_st_a,r_st_f,r_stlld0,r_stlaf0,r_stbf0,m_gst,m_gst_lk,m_st_a,m_st_f,m_stlld0,m_stlaf0,m_stbf0,r_sq_a,r_sq_f,m_sq_a,m_sq_f,m_atk,r_atk
E030_CLIPS=st_a,look,stld0,stbf0,jp0,jp1,jp2,atk_0,atk_1,atk_2,atk_3,b_stp,st_btl
for mc in e048:$E048_CLIPS e030:$E030_CLIPS e036:m_gst e023:idle e024:idle e001:n_st; do
  m=${mc%%:*}
  run enemy_$m $PY export_web_enemy.py export --model $m --out "$DATA/enemies" --clips ${mc#*:}
  [ -s "$DATA/enemies/$m.glb" ] || fail "data/enemies/$m.glb missing after the enemy export (log: $LOGS/enemy_$m.log)"
  grep '^wrote' "$LOGS/enemy_$m.log" | sed "s#$REPO/##" | cut -c1-160
done
echo "ok"

step "enemy weapons (e027 slot 0: parameter set +0x44 -> w003 / w004 at marker 15) -> data/enemies"
# inputs 077700c1/c2 (w003) and 07770101/02 (w004), checked by disc.py verify; the skip only guards a partial tree
if [ -s "$NOPATH/077700c1.bin" ] && [ -s "$NOPATH/077700c2.bin" ] && [ -s "$NOPATH/07770101.bin" ] && [ -s "$NOPATH/07770102.bin" ]; then
  run enemy_weapons $PY export_web_enemy.py weapon --owner e027 --slot 0 --out "$DATA/enemies"
  [ -s "$DATA/enemies/e027_weapons.json" ] || fail "data/enemies/e027_weapons.json missing (log: $LOGS/enemy_weapons.log)"
  grep '^wrote' "$LOGS/enemy_weapons.log" | sed "s#$REPO/##" | cut -c1-160
  # e048 (w003 / w004) and e036 (w004) carry the same weapons at marker 15 (offset 0): map only, own socket bone
  for o in e048 e036; do
    run enemy_weapons_$o $PY export_web_enemy.py weapon --owner $o --slot 0 --out "$DATA/enemies" --map-only
    [ -s "$DATA/enemies/${o}_weapons.json" ] || fail "data/enemies/${o}_weapons.json missing (log: $LOGS/enemy_weapons_$o.log)"
    grep '^wrote' "$LOGS/enemy_weapons_$o.log" | sed "s#$REPO/##" | cut -c1-160
  done
  echo "ok"
else
  echo "skipped: weapon files 077700c1/c2, 07770101/02 not extracted (soldiers drawn without rifles)"
fi

step "enemy placements (stage file sections 0/1: stage-load e027s -> data/enemies/placements.json)"
run enemy_placements $PY export_web_placements.py "$NOPATH/06800001.bin" "$DATA/enemies"
[ -s "$DATA/enemies/placements.json" ] || fail "data/enemies/placements.json missing (log: $LOGS/enemy_placements.log)"
head -n 1 "$LOGS/enemy_placements.log" | cut -c1-160
run enemy_ai $PY export_web_triggers.py "$NOPATH/06800001.bin" "$DATA/enemies"   # trigger volumes + scripts -> data/enemies/ai.json (enemy_ai.js)
[ -s "$DATA/enemies/ai.json" ] || fail "data/enemies/ai.json missing (log: $LOGS/enemy_ai.log)"
grep '^wrote' "$LOGS/enemy_ai.log" | sed "s#$REPO/##" | cut -c1-160
echo "ok"

step "weapon stats + reticle tables and texture (bgd.bin parts, menudata.bin menu2, ELF from the disc) -> data/aim"
run aim $PY export_web_aim.py --iso "$ISO" -o "$DATA/aim"
tail -n 1 "$LOGS/aim.log"

if [ $MOTIONS -eq 1 ]; then
  step "motion catalogue (data/vincent/motions.json, reference only)"
  run motions $PY export_web_vincent.py catalogue
  echo "ok"
fi

# ------------------------------------------------------------------ 5. done
python3 - "$DATA/byo-build.json" "$ISO" "$REPO" <<'PYEOF'
import json, os, subprocess, sys, time
out, iso, repo = sys.argv[1:4]
rev = subprocess.run(['git', '-C', repo, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
dirty = subprocess.run(['git', '-C', repo, 'status', '--porcelain', '--untracked-files=no'],
                       capture_output=True, text=True).stdout.strip() != ''
json.dump(dict(built=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), commit=rev + ('+local changes' if dirty else ''),
               disc='SLES_541.85', image_bytes=os.path.getsize(iso), dump_free=True,
               note='built by tools/byo/build_web.sh from the user\'s own disc; game-derived, do not share'),
          open(out, 'w'), indent=1)
PYEOF
n=$(find "$DATA" -type f | wc -l); sz=$(du -sh "$DATA" | cut -f1)
printf '\nDone in %ss: %s files, %s in native/web/data (gitignored: never commit or upload it).\n' "$(( $(date +%s) - T0 ))" "$n" "$sz"
cat <<EOF

Serve it (pick one), then open the printed address in a browser with WebGL:

  tools/byo/serve.sh            python3 http.server on http://127.0.0.1:8000/  (no Docker)
  tools/byo/serve.sh docker     nginx + the optional Report backend, http://127.0.0.1:8796/

  Pages:  index.html  free-camera viewer     play.html  walk Vincent around
  The pages load three.js from cdn.jsdelivr.net, so the browser needs internet access.
  Without the Docker option the Report button (R) can't save; everything else works.
EOF
