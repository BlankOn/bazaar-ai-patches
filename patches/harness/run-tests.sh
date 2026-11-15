#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): run every automated test in
# ai-patches/TEST.md (T1-T7) in order.
# Never installs or removes packages, never reboots.
#
# Usage:  ai-patches/patches/harness/run-tests.sh [--no-app] [--notify]
#   --no-app  skip T6 (needs a full build and no running Bazaar)
#   --notify  also run T7: sends a real "updates available" notification to
#             the desktop and records it in the last-update-notification
#             setting (so the daily notification may skip a day)
#   BZ_REPO=/path/to/bazaar  test that tree (default: the repository this
#             directory was copied into, i.e. ../../..)
# Output:  build/apt-tests/*.png and a PASS/FAIL summary. Exit 1 on failure.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${BZ_REPO:-$(cd "$HERE/../../.." && pwd)}"
OUT="$REPO/build/apt-tests"
AP="$(cd "$HERE/../.." && pwd)"
export BZ_REPO="$REPO"
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
  for f in "$AP"/patches/src/* "$AP"/patches/data/*; do
    rel="$(basename "$(dirname "$f")")/$(basename "$f")"
    cmp -s "$f" "$rel" || { echo "OUT OF SYNC: $f vs $rel"; ok=1; }
  done
  for f in src/meson.build src/bazaar.gresource.xml src/bz-library-page.c src/bz-library-page.blp po/POTFILES.in \
           src/update-worker.c src/bazaar-daemon.c src/bz-application.c \
           data/meson.build data/io.github.kolunmi.Bazaar.gschema.xml data/bazaar-update-notifications.desktop \
           src/bz-apt-updates.h src/bz-apt-updates.c src/bz-apt-updates-card.h src/bz-apt-updates-card.c src/bz-apt-updates-card.blp \
           src/bz-update-notifier.h src/bz-update-notifier.c; do
    grep -q "DOWNSTREAM PATCH (APT system updates)" "$f" || { echo "MISSING MARKER: $f"; ok=1; }
  done
  # translations merged (H12): the merge script has nothing left to add
  for frag in "$AP"/patches/po/*-apt-updates.po; do
    lang="$(basename "$frag" -apt-updates.po)"
    cp "po/$lang.po" "$OUT/$lang.po.before"
    BZ_REPO="$REPO" python3 -I "$AP/patches/po/merge-translations.py" >/dev/null
    if ! cmp -s "po/$lang.po" "$OUT/$lang.po.before"; then
      echo "TRANSLATIONS NOT MERGED: po/$lang.po (run patches/po/merge-translations.py)"
      cp "$OUT/$lang.po.before" "po/$lang.po"
      ok=1
    fi
    msgfmt -c -o /dev/null "po/$lang.po" || ok=1
  done
  grep -q -- "--system-talk-name=org.freedesktop.PackageKit" build-aux/flatpak/io.github.kolunmi.Bazaar.json \
    || { echo "MISSING: PackageKit permission in Flatpak manifest"; ok=1; }
  python3 -m json.tool build-aux/flatpak/io.github.kolunmi.Bazaar.json >/dev/null || { echo "INVALID JSON manifest"; ok=1; }
  [ $ok = 0 ] && echo "applied files in sync, markers present, translations merged, manifest valid"
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

t7_notify () {
  mkdir -p build/schemas
  cp data/io.github.kolunmi.Bazaar.gschema.xml build/schemas/ && glib-compile-schemas build/schemas || return 1
  log="$(GSETTINGS_SCHEMA_DIR="$REPO/build/schemas" BAZAAR_NOTIFY_NOW=1 G_MESSAGES_DEBUG="BAZAAR::NOTIFY BAZAAR::APT" \
         timeout 400 build/src/bazaar --run-update-worker 2>&1)"
  echo "$log" | grep -E "Pending updates|Sent the update notification|Failed to send|disabled"
  echo "$log" | grep -q "Sent the update notification" || echo "$log" | grep -q "Pending updates: 0 apps, 0 system"
}

step T1 "static checks (sync, markers, translations, manifest)" t1_static
step T2 "standalone harness builds with -Werror" t2_harness
step T3 "full Bazaar build, no patch warnings" t3_full_build
step T4 "card counts match PackageKit and apt" t4_counts
step T5 "harness screenshots" t5_harness_shots
case " $* " in *" --no-app "*) ;; *)
  step T6 "real Bazaar app: Library page screenshots" t6_app ;;
esac
case " $* " in *" --notify "*)
  step T7 "update worker sends the updates-available notification" t7_notify ;;
esac

printf '\n=== Summary%b\n' "$RESULTS"
[ -d "$OUT" ] && printf 'Screenshots: %s\n' "$OUT"
printf 'Now compare the screenshots with %s (TEST.md T8).\n' "$AP/patches/harness/expected-library-page.png"
exit $FAILED
