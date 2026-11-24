#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): run every automated test in
# ai-patches/TEST.md (T1-T7, D1, E1, and R1 on a Warsi-rebranded tree) in order.
# Never installs or removes packages, never reboots.
#
# Usage:  ai-patches/patches/harness/run-tests.sh [--no-app] [--notify]
#   --no-app  skip T6 (needs a full build and no running Bazaar/Warsi)
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
. "$HERE/brand.sh"   # APP_ID, APP_BIN, GETTEXT_DOMAIN, REBRANDED
REBRAND="$AP/patches/rebrand/rebrand.py"

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
  masters="$AP/patches/src/* $AP/patches/data/*"
  [ $REBRANDED = 1 ] && masters="$masters $AP/patches/rebrand/src/*"
  for f in $masters; do
    rel="$(basename "$(dirname "$f")")/$(basename "$f")"
    if [ $REBRANDED = 1 ]; then
      # the masters are Bazaar's; compare them as the rebrand turns them out
      python3 -I "$REBRAND" --filter "$f" "$rel" | cmp -s - "$rel" || { echo "OUT OF SYNC: $f vs $rel (after rebrand)"; ok=1; }
    else
      cmp -s "$f" "$rel" || { echo "OUT OF SYNC: $f vs $rel"; ok=1; }
    fi
  done
  for f in src/meson.build src/bazaar.gresource.xml src/bz-library-page.c src/bz-library-page.blp po/POTFILES.in \
           src/update-worker.c src/bazaar-daemon.c src/bz-application.c src/main.c \
           data/meson.build "data/$APP_ID.gschema.xml" data/bazaar-update-notifications.desktop \
           src/bz-apt-updates.h src/bz-apt-updates.c src/bz-apt-updates-card.h src/bz-apt-updates-card.c src/bz-apt-updates-card.blp \
           src/bz-update-notifier.h src/bz-update-notifier.c \
           src/bz-deb-installer.h src/bz-deb-installer.c src/bz-deb-install-dialog.h src/bz-deb-install-dialog.c \
           src/bz-deb-install-dialog.blp data/bazaar-deb-mimeapps.list; do
    grep -q "DOWNSTREAM PATCH (APT system updates)" "$f" || { echo "MISSING MARKER: $f"; ok=1; }
  done
  for f in src/meson.build src/bazaar.gresource.xml po/POTFILES.in src/bz-window.c src/bz-window.blp \
           src/bz-gnome-extensions.h src/bz-gnome-extensions.c \
           src/bz-gnome-extensions-page.h src/bz-gnome-extensions-page.c src/bz-gnome-extensions-page.blp \
           src/bz-gnome-extension-dialog.h src/bz-gnome-extension-dialog.c src/bz-gnome-extension-dialog.blp; do
    grep -q "DOWNSTREAM PATCH (GNOME Extensions)" "$f" || { echo "MISSING GNOME EXTENSIONS MARKER: $f"; ok=1; }
  done
  grep -q -- "--talk-name=org.gnome.Shell.Extensions" build-aux/flatpak/io.github.kolunmi.Bazaar.json ||
    { echo "MISSING: org.gnome.Shell.Extensions in the Flatpak manifest"; ok=1; }
  if [ $REBRANDED = 1 ]; then
    for f in src/bz-rebrand.h src/bz-rebrand.c src/bz-application.c src/meson.build; do
      grep -q "DOWNSTREAM PATCH (Warsi rebrand)" "$f" || { echo "MISSING REBRAND MARKER: $f"; ok=1; }
    done
  fi
  # translations merged (H12, and R on a rebranded tree): nothing left to add
  frags="$(ls "$AP"/patches/po/*-apt-updates.po)"
  [ $REBRANDED = 1 ] && frags="$frags $(ls "$AP"/patches/rebrand/po/*.po)"
  for frag in $frags; do
    lang="$(basename "$frag" | cut -d- -f1)"
    cp "po/$lang.po" "$OUT/$lang.po.before"
    BZ_REPO="$REPO" python3 -I "$AP/patches/po/merge-translations.py" "$frag" >/dev/null
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
  touch src/bz-apt-updates.c src/bz-apt-updates-card.c src/bz-library-page.c src/bz-update-notifier.c \
        src/bz-deb-installer.c src/bz-deb-install-dialog.c src/bz-application.c
  [ $REBRANDED = 1 ] && touch src/bz-rebrand.c
  log="$(ninja -C build 2>&1)" || { echo "$log" | tail -20; return 1; }
  if echo "$log" | grep -iE "bz-apt-updates|bz-library-page|bz-update-notifier|bz-deb-install|bz-application|bz-rebrand" | grep -iqE "warning|error"; then
    echo "$log" | grep -iE "warning|error"; return 1
  fi
  echo "full build ok, no warnings from the patch's files"
}

r1_rebrand () {
  ok=0
  python3 -I "$REBRAND" --check >/dev/null 2>&1 || { python3 -I "$REBRAND" --check 2>/dev/null | head; echo "REBRAND INCOMPLETE: run patches/rebrand/rebrand.py"; ok=1; }
  dest="$OUT/install"
  rm -rf "$dest"
  DESTDIR="$dest" meson install -C build --no-rebuild >/dev/null 2>&1 || { echo "meson install failed"; return 1; }
  # bge-demo is libbge's demo program (not Warsi); it embeds upstream's docs
  left="$(grep -rlE "io\.github\.kolunmi\.Bazaar|io/github/kolunmi/Bazaar" "$dest" | grep -v "/bge-demo$"; find "$dest" -iname "*bazaar*")"
  [ -z "$left" ] || { echo "OLD NAME LEFT IN THE INSTALLED TREE:"; echo "$left"; ok=1; }
  for f in bin/warsi bin/warsi-daemon bin/warsi-dl-worker etc/xdg/autostart/warsi.desktop etc/xdg/gnome-mimeapps.list \
           "share/applications/$APP_ID.desktop" "share/metainfo/$APP_ID.metainfo.xml" \
           "share/glib-2.0/schemas/$APP_ID.gschema.xml" "share/icons/hicolor/scalable/apps/$APP_ID.svg" \
           "share/icons/hicolor/symbolic/apps/$APP_ID-symbolic.svg" "share/dbus-1/services/$APP_ID.service" \
           "share/locale/id/LC_MESSAGES/$GETTEXT_DOMAIN.mo"; do
    [ -n "$(find "$dest" -path "*/$f")" ] || { echo "NOT INSTALLED: $f"; ok=1; }
  done
  desktop-file-validate "$(find "$dest" -name "$APP_ID.desktop")" || ok=1
  if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net "$(find "$dest" -name "$APP_ID.metainfo.xml")" >/dev/null || ok=1
  fi
  # launchers (Praya, GNOME Shell) find the app by these words, in English
  # and Indonesian (GLib's desktop-file search, like they use)
  apps="$OUT/app-search/applications"
  rm -rf "$OUT/app-search" && mkdir -p "$apps"
  cp "$(find "$dest" -name "$APP_ID.desktop")" "$apps/"
  for lang in C id; do
    echo "app search, LANGUAGE=$lang:"
    env XDG_DATA_HOME="$OUT/app-search" XDG_DATA_DIRS="$OUT/app-search/none" LANGUAGE=$lang \
      python3 -I "$HERE/app-search.py" "$APP_ID.desktop" \
      warsi "warung aplikasi" store software "gnome software" bazaar || ok=1
  done
  note="$(LANGUAGE=id msgunfmt "$(find "$dest" -path "*/id/LC_MESSAGES/$GETTEXT_DOMAIN.mo")" | grep -A1 'msgid "Originally Bazaar')"
  echo "$note" | grep -q "Dibangun dari Bazaar" || { echo "MISSING: Indonesian About note"; ok=1; }
  [ $ok = 0 ] && echo "rebrand complete: $APP_ID, no Bazaar ID or file name installed, metadata valid"
  return $ok
}

d1_deb () {
  "$HERE/deb-check.sh" || return 1
  # the launcher offers to open .deb files, and is their default handler
  grep -q "^MimeType=.*application/vnd.debian.binary-package;" "data/$APP_ID.desktop.in" ||
    { echo "MISSING: .deb MIME type in data/$APP_ID.desktop.in"; return 1; }
  grep -qx "application/vnd.debian.binary-package=$APP_ID.desktop" data/bazaar-deb-mimeapps.list ||
    { echo "MISSING: default handler in data/bazaar-deb-mimeapps.list"; return 1; }
}

e1_extensions () {
  rc=0
  "$HERE/ext-check.sh" > "$OUT/ext-check.log" 2>&1 || rc=1
  grep -E "^(PASS|FAIL)" "$OUT/ext-check.log"
  return $rc
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
  { [ $REBRANDED = 0 ] || APP_SHOT_ABOUT=1 "$HERE/app-shot.sh" "$OUT/app-about.png" 20 2>&1 | grep -E "Saved"; } &&
  { [ ! -f "$OUT/deb/bz-deb-check-app_1.0_all.deb" ] ||
    APP_SHOT_OPEN="$OUT/deb/bz-deb-check-app_1.0_all.deb" "$HERE/app-shot.sh" "$OUT/app-deb-install.png" 20 2>&1 | grep -E "Saved"; } &&
  extensions_shot "$OUT/app-extensions.png" &&
  APP_SHOT_EXT_OPEN=0 extensions_shot "$OUT/app-extension-dialog.png" &&
  [ -s "$OUT/app-library.png" ]
}

# The Extensions tab with the fake GNOME Shell (TEST.md E2), on its own
# private bus. X11 backend: a covered Wayland window gets no frame callbacks,
# so results arriving later would never be laid out.
extensions_shot () {
  dbus-run-session -- sh -c "
    python3 -I '$HERE/fake-shell-extensions.py' 2>/dev/null &
    sleep 1
    GDK_BACKEND=x11 APP_SHOT_PAGE=gnome-extensions '$HERE/app-shot.sh' '$1' 15" 2>&1 | grep -E "Saved"
}

t7_notify () {
  mkdir -p build/schemas
  cp "data/$APP_ID.gschema.xml" build/schemas/ && glib-compile-schemas build/schemas || return 1
  log="$(GSETTINGS_SCHEMA_DIR="$REPO/build/schemas" BAZAAR_NOTIFY_NOW=1 G_MESSAGES_DEBUG="BAZAAR::NOTIFY BAZAAR::APT" \
         timeout 400 "build/src/$APP_BIN" --run-update-worker 2>&1)"
  echo "$log" | grep -E "Pending updates|Sent the update notification|Failed to send|disabled"
  echo "$log" | grep -q "Sent the update notification" || echo "$log" | grep -q "Pending updates: 0 apps, 0 system"
}

step T1 "static checks (sync, markers, translations, manifest)" t1_static
step T2 "standalone harness builds with -Werror" t2_harness
step T3 "full $APP_BIN build, no patch warnings" t3_full_build
[ $REBRANDED = 1 ] && step R1 "Warsi rebrand complete and installs cleanly" r1_rebrand
step T4 "card counts match PackageKit and apt" t4_counts
step T5 "harness screenshots" t5_harness_shots
step D1 ".deb installer reads packages (simulated, installs nothing)" d1_deb
step E1 "GNOME Extensions model: extensions.gnome.org and a fake Shell" e1_extensions
case " $* " in *" --no-app "*) ;; *)
  step T6 "real $APP_BIN app: Library page (and About) screenshots" t6_app ;;
esac
case " $* " in *" --notify "*)
  step T7 "update worker sends the updates-available notification" t7_notify ;;
esac

printf '\n=== Summary%b\n' "$RESULTS"
[ -d "$OUT" ] && printf 'Screenshots: %s\n' "$OUT"
printf 'Now compare the screenshots with %s (TEST.md T8).\n' "$AP/patches/harness/expected-library-page.png"
exit $FAILED
