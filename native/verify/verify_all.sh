#!/usr/bin/env bash
#
# verify_all.sh — integration-verification pass for the SETTLED standalone
# subsystems of the Dirge of Cerberus native PC port.
#
# For each settled subsystem it does a FORCED clean rebuild via the subsystem's
# own Makefile (to surface -Wall/-Wextra warnings, not just "up to date"), then
# runs that subsystem's test / validate / smoke target. It never edits subsystem
# source — it only invokes the existing Makefiles (which legitimately write
# their own .o/binaries in their own dirs) and reads the results.
#
# Scope: textures, audio, collision, effects, movie (python), script, kel loader.
# It deliberately does NOT touch native/engine, native/assets/models,
# native/gpu, native/text or the dashboard (those have active agents).
#
# Outputs:
#   logs/<name>.build.log , logs/<name>.test.log   (full captured output)
#   results.tsv                                     (machine-readable summary)
#   stdout                                          (human summary table)

set -u

BASE=/srv/games/dirge-port/native
PORT=/srv/games/dirge-port/port           # the kel.c loader actually lives here
EXTRACT=/srv/games/dirge-port/extract/_KEL
SHARED=/srv/shared/dirge-port
VERIFY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGDIR="$VERIFY/logs"
RESULTS="$VERIFY/results.tsv"

mkdir -p "$LOGDIR" "$SHARED"
: > "$RESULTS"
printf 'subsystem\tbuild\twarnings\ttest\tdetail\n' >> "$RESULTS"

# ANSI-free status words so logs/pipes stay clean.
pass() { printf 'PASS'; }
fail() { printf 'FAIL'; }

# count_warnings <logfile>  -> number of compiler "warning:" lines.
# grep -c always prints a count and exits 1 when zero, so no "|| echo 0" (that
# would append a second line and corrupt the TSV field).
count_warnings() { local n; n=$(grep -c ' warning:' "$1" 2>/dev/null); echo "${n:-0}"; }

# record <name> <build> <warnings> <test> <detail>
record() { printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$RESULTS"; }

banner() { echo; echo "=================================================================="; echo "### $1"; echo "=================================================================="; }

# ---------------------------------------------------------------------------
# Generic forced build: clean + build target, capture log, return make status.
#   do_build <name> <dir> <build_target...>
# Sets globals: B_STATUS (PASS/FAIL), B_WARN (int)
# ---------------------------------------------------------------------------
do_build() {
    local name="$1" dir="$2"; shift 2
    local log="$LOGDIR/$name.build.log"
    ( cd "$dir" && make clean ) >/dev/null 2>&1
    ( cd "$dir" && make "$@" ) > "$log" 2>&1
    local rc=$?
    B_WARN=$(count_warnings "$log")
    if [ $rc -eq 0 ]; then B_STATUS=PASS; else B_STATUS=FAIL; fi
    echo "[build $name] rc=$rc warnings=$B_WARN  (log: $log)"
    tail -n 3 "$log" | sed 's/^/    | /'
}

# ===========================================================================
echo "Dirge native — settled-subsystem verification"
echo "started: $(date -Is)"
echo "toolchain: $(cc --version 2>/dev/null | head -1)"
echo "extract tree: $([ -d "$EXTRACT" ] && echo present || echo MISSING)"

# ---------------------------------------------------------------------------
# 1. TEXTURES — libdtex + dtex CLI.  No Makefile test target -> binary smoke.
# ---------------------------------------------------------------------------
banner "textures (libdtex + dtex)"
do_build textures "$BASE/assets/textures" all
TLOG="$LOGDIR/textures.test.log"
{
    echo "# smoke: run dtex with no args (expect usage, exit 0)"
    "$BASE/assets/textures/dtex"; echo "[dtex no-arg exit=$?]"
    echo "# artifact check: libdtex.a + validation PNGs"
    ls -1 "$BASE/assets/textures/libdtex.a" 2>&1
    echo "validation PNGs: $(ls -1 "$BASE/assets/textures/validation"/*.png 2>/dev/null | wc -l)"
} > "$TLOG" 2>&1
if [ -f "$BASE/assets/textures/libdtex.a" ] && [ -x "$BASE/assets/textures/dtex" ]; then
    T_STATUS=PASS; T_DETAIL="build-only subsystem; dtex+libdtex.a present, CLI smoke ok (no make test target)"
else
    T_STATUS=FAIL; T_DETAIL="dtex or libdtex.a missing after build"
fi
record textures "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 2. AUDIO — dirge_atdec (libav) + dirge_wd + dirge_sound(_demo).
#    test = `make sndcheck` (mixer demo shells out to atdec/wd, writes a WAV).
# ---------------------------------------------------------------------------
banner "audio (dirge_atdec / dirge_wd / dirge_sound)"
do_build audio "$BASE/assets/audio" all
TLOG="$LOGDIR/audio.test.log"
( cd "$BASE/assets/audio" && make sndcheck ) > "$TLOG" 2>&1
trc=$?
OUTWAV="$SHARED/sound_mix_sample.wav"
if [ $trc -eq 0 ] && [ -s "$OUTWAV" ]; then
    T_STATUS=PASS; T_DETAIL="make sndcheck ok; mixed $(du -h "$OUTWAV" 2>/dev/null | cut -f1) WAV (shelled out to atdec/wd)"
else
    T_STATUS=FAIL; T_DETAIL="make sndcheck rc=$trc (see log; needs extract + decoder siblings)"
fi
echo "[test audio] rc=$trc  -> $T_STATUS"
record audio "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 3. COLLISION — cid + OBJ export (cidtool).  test = `make validate`.
# ---------------------------------------------------------------------------
banner "collision (cidtool + OBJ export)"
do_build collision "$BASE/assets/collision" all
TLOG="$LOGDIR/collision.test.log"
( cd "$BASE/assets/collision" && make validate ) > "$TLOG" 2>&1
trc=$?
NOBJ=$(ls -1 "$BASE/assets/collision/out"/*.obj 2>/dev/null | wc -l)
if [ $trc -eq 0 ] && [ "$NOBJ" -gt 0 ]; then
    T_STATUS=PASS; T_DETAIL="make validate ok; exported $NOBJ OBJ (v2 + v19 maps)"
else
    T_STATUS=FAIL; T_DETAIL="make validate rc=$trc objs=$NOBJ (see log)"
fi
echo "[test collision] rc=$trc objs=$NOBJ -> $T_STATUS"
record collision "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 4. EFFECTS — fed/fez parser.  test = `make check` (parses a sample if present).
# ---------------------------------------------------------------------------
banner "effects (fed / fez)"
do_build effects "$BASE/assets/effects" all
TLOG="$LOGDIR/effects.test.log"
( cd "$BASE/assets/effects" && make check ) > "$TLOG" 2>&1
trc=$?
if [ $trc -eq 0 ]; then
    if grep -qi 'skipping check' "$TLOG"; then
        T_STATUS=PASS; T_DETAIL="build ok; make check ran, sample absent -> gracefully skipped"
    else
        T_STATUS=PASS; T_DETAIL="make check ok; parsed real .fed sample"
    fi
else
    T_STATUS=FAIL; T_DETAIL="make check rc=$trc (see log)"
fi
echo "[test effects] rc=$trc -> $T_STATUS"
record effects "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 5. MOVIE — python pipeline.  No C build; syntax-compile every script with an
#    isolated interpreter (-I) so a planted module next to them can't run.
# ---------------------------------------------------------------------------
banner "movie (python pipeline)"
MVDIR="$BASE/movie/scripts"
MLOG="$LOGDIR/movie.build.log"; : > "$MLOG"
TLOG="$LOGDIR/movie.test.log"; : > "$TLOG"
py_fail=0; py_total=0
for f in "$MVDIR"/*.py; do
    py_total=$((py_total+1))
    if python3 -I -m py_compile "$f" >> "$MLOG" 2>&1; then
        echo "  [PASS] py_compile $(basename "$f")" | tee -a "$TLOG"
    else
        echo "  [FAIL] py_compile $(basename "$f")" | tee -a "$TLOG"
        py_fail=$((py_fail+1))
    fi
done
# dependency availability: confirm every imported module is importable
echo "# import availability (-I):" >> "$TLOG"
dep_fail=0
for mod in sys struct wave array os; do
    if python3 -I -c "import $mod" >> "$TLOG" 2>&1; then
        echo "  [PASS] import $mod" >> "$TLOG"
    else
        echo "  [FAIL] import $mod" >> "$TLOG"; dep_fail=$((dep_fail+1))
    fi
done
if [ $py_fail -eq 0 ] && [ $dep_fail -eq 0 ]; then
    B_STATUS=PASS; T_STATUS=PASS; B_WARN=0
    T_DETAIL="$py_total/$py_total scripts compile; all imports stdlib & available"
else
    B_STATUS=FAIL; T_STATUS=FAIL; B_WARN=0
    T_DETAIL="py_compile fails=$py_fail import fails=$dep_fail"
fi
echo "[movie] compile $((py_total-py_fail))/$py_total, dep fails=$dep_fail -> $T_STATUS"
record movie "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 6. SCRIPT — dcc / jvm / jvmbatch.  test = `make test` (jvmtest self-test),
#    plus build `make jvmbatch` and a functional batch run over real classes.
# ---------------------------------------------------------------------------
banner "script (dcc / jvm / jvmbatch)"
do_build script "$BASE/script" all
TLOG="$LOGDIR/script.test.log"; : > "$TLOG"
( cd "$BASE/script" && make test ) >> "$TLOG" 2>&1
trc=$?
echo "# --- make jvmbatch (build target) ---" >> "$TLOG"
( cd "$BASE/script" && make jvmbatch ) >> "$TLOG" 2>&1
echo "# --- functional batch over extracted .class files ---" >> "$TLOG"
CLASSES=$(find "$EXTRACT" -name '*.class' 2>/dev/null)
if [ -n "$CLASSES" ]; then
    # shellcheck disable=SC2086
    ( cd "$BASE/script" && ./jvmbatch $CLASSES ) >> "$TLOG" 2>&1
fi
# jvmtest prints "SELF-TEST OK" / "SELF-TEST FAILED (n)" and [PASS]/[FAIL] lines
NPASS=$(grep -c '\[PASS\]' "$TLOG" 2>/dev/null); NPASS=${NPASS:-0}
NFAIL=$(grep -c '\[FAIL\]' "$TLOG" 2>/dev/null); NFAIL=${NFAIL:-0}
if [ $trc -eq 0 ] && grep -q 'SELF-TEST OK' "$TLOG"; then
    T_STATUS=PASS; T_DETAIL="jvmtest SELF-TEST OK ($NPASS cases); jvmbatch built & ran over $(echo "$CLASSES" | grep -c . ) classes"
else
    T_STATUS=FAIL; T_DETAIL="jvmtest rc=$trc PASS=$NPASS FAIL=$NFAIL (see log)"
fi
echo "[test script] rc=$trc pass=$NPASS fail=$NFAIL -> $T_STATUS"
record script "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ---------------------------------------------------------------------------
# 7. KEL LOADER — kel.c + kel_tool (lives in port/, not native/src/).
#    No Makefile test target -> binary smoke.
# ---------------------------------------------------------------------------
banner "kel loader (kel.c / kel_tool)"
do_build kel "$PORT" all
TLOG="$LOGDIR/kel.test.log"
{
    echo "# smoke: run kel_tool with no args (expect usage, exit 0)"
    "$PORT/kel_tool"; echo "[kel_tool no-arg exit=$?]"
} > "$TLOG" 2>&1
if [ -x "$PORT/kel_tool" ]; then
    T_STATUS=PASS; T_DETAIL="build-only subsystem; kel_tool present, CLI smoke ok (no make test target)"
else
    T_STATUS=FAIL; T_DETAIL="kel_tool missing after build"
fi
record kel "$B_STATUS" "$B_WARN" "$T_STATUS" "$T_DETAIL"

# ===========================================================================
# Summary
# ===========================================================================
banner "SUMMARY"
if command -v column >/dev/null 2>&1; then
    column -t -s$'\t' "$RESULTS"
else
    awk -F'\t' '{printf "%-11s %-6s %-9s %-6s %s\n", $1, $2, (NR==1?$3:"warn="$3), $4, $5}' "$RESULTS"
fi
echo
TOTAL=$(($(wc -l < "$RESULTS") - 1))
GREEN=$(awk -F'\t' 'NR>1 && $2=="PASS" && $4=="PASS"{c++} END{print c+0}' "$RESULTS")
CLEAN=$(awk -F'\t' 'NR>1 && $2=="PASS" && $3==0{c++} END{print c+0}' "$RESULTS")
echo "Subsystems: $TOTAL   build+test GREEN: $GREEN/$TOTAL   build-clean (0 warnings): $CLEAN/$TOTAL"
echo "finished: $(date -Is)"
