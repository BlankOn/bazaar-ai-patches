#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): run every automated test in
# ai-patches/TEST.md (T1-T6) in order, ending with the real Bazaar app.
# Read-only: never installs or removes packages, never reboots.
#
# Usage:  ai-patches/patches/harness/run-tests.sh [--no-app]
#   --no-app  skip T6 (needs a full build and no running Bazaar)
# Output:  build/apt-tests/*.png and a PASS/FAIL summary. Exit 1 on failure.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="$REPO/build/apt-tests"
AP="$REPO/ai-patches"
RESULTS=""
FAILED=0

cd "$REPO" || exit 2
mkdir -p "$OUT"

step () {  # step ID "description" command...
  id="$1"; desc="$2"; shift 2
  printf '\n=== %s: %s\n' "$id" "$desc"
  if "$@"; then
    RESULTS="$RESULTS\nPASS  $id  $desc"
  else
    RESULTS="$RESULTS\nFAIL  $id  $desc"
    FAILED=1
  fi
}

t1_static () {
  ok=0
  for f in "$AP"/patches/src/*; do
    cmp -s "$f" "src/$(basename "$f")" || { echo "OUT OF SYNC: $f"; ok=1; }
  done
  for f in src/meson.build src/bazaar.gresource.xml src/bz-library-page.c src/bz-library-page.blp po/POTFILES.in \
           src/bz-apt-updates.h src/bz-apt-updates.c src/bz-apt-updates-card.h src/bz-apt-updates-card.c src/bz-apt-updates-card.blp; do
    grep -q "DOWNSTREAM PATCH (APT system updates)" "$f" || { echo "MISSING MARKER: $f"; ok=1; }
  done
  grep -q -- "--system-talk-name=org.freedesktop.PackageKit" build-aux/flatpak/io.github.kolunmi.Bazaar.json \
    || { echo "MISSING: PackageKit permission in Flatpak manifest"; ok=1; }
  python3 -m json.tool build-aux/flatpak/io.github.kolunmi.Bazaar.json >/dev/null || { echo "INVALID JSON manifest"; ok=1; }
  [ $ok = 0 ] && echo "applied files in sync, markers present, manifest valid"
  return $ok
}

t2_harness () {
  "$HERE/build.sh" >/dev/null
}

t3_full_build () {
  [ -d build ] && [ -f build/build.ninja ] || meson setup build >/dev/null || return 1
  touch src/bz-apt-updates.c src/bz-apt-updates-card.c src/bz-library-page.c
  log="$(ninja -C build 2>&1)" || { echo "$log" | tail -20; return 1; }
  if echo "$log" | grep -iE "bz-apt-updates|bz-library-page" | grep -iqE "warning|error"; then
    echo "$log" | grep -iE "warning|error"; return 1
  fi
  echo "full build ok, no warnings from the patch's files"
}

t4_counts () {
  python3 -I "$HERE/check-counts.py"
}

t5_harness_shots () {
  env HARNESS_ICONS="$REPO/src/icons" HARNESS_SHOT="$OUT/harness.png" HARNESS_DELAY=12 \
    timeout 60 build/apt-harness/harness >/dev/null 2>&1
  env HARNESS_ICONS="$REPO/src/icons" HARNESS_SHOT="$OUT/harness-expanded.png" HARNESS_DELAY=12 \
    HARNESS_EXPAND=1 HARNESS_HEIGHT=900 timeout 60 build/apt-harness/harness >/dev/null 2>&1
  [ -s "$OUT/harness.png" ] && [ -s "$OUT/harness-expanded.png" ] && echo "saved $OUT/harness.png, $OUT/harness-expanded.png"
}

t6_app () {
  "$HERE/app-shot.sh" "$OUT/app-library.png" 40 2>&1 | grep -E "APT-DEBUG|Saved|already running|^no " &&
  APP_SHOT_EXPAND=1 "$HERE/app-shot.sh" "$OUT/app-library-expanded.png" 40 2>&1 | grep -E "Saved" &&
  [ -s "$OUT/app-library.png" ]
}

step T1 "static checks (sync, markers, manifest)" t1_static
step T2 "standalone harness builds with -Werror" t2_harness
step T3 "full Bazaar build, no patch warnings" t3_full_build
step T4 "card counts match PackageKit and apt" t4_counts
step T5 "harness screenshots" t5_harness_shots
if [ "$1" != "--no-app" ]; then
  step T6 "real Bazaar app: Library page screenshots" t6_app
fi

printf '\n=== Summary%b\n' "$RESULTS"
[ -d "$OUT" ] && printf 'Screenshots: %s\n' "$OUT"
printf 'Now compare the screenshots with ai-patches/patches/harness/expected-library-page.png (TEST.md T7).\n'
exit $FAILED
