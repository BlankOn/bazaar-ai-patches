#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): screenshot the real Bazaar app's
# Library page. See ai-patches/TEST.md. Read-only: clicks nothing.
#
# Needs: a graphical session, a full build in <repo>/build (meson/ninja),
# gcc, pkg-config, glib-compile-schemas, gdbus.
#
# Usage:  ai-patches/patches/harness/app-shot.sh OUT.png [delay-seconds]
#   APP_SHOT_EXPAND=1 also expands the APT package list.
#   APP_SHOT_ABOUT=1 opens the About dialog before the screenshot.
#   APP_SHOT_OPEN=FILE opens FILE like a double click would (e.g. a .deb,
#   for the package installer dialog).
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${BZ_REPO:-$(cd "$HERE/../../.." && pwd)}"
OUT="$(realpath -m "${1:?usage: app-shot.sh OUT.png [delay-seconds]}")"
DELAY="${2:-40}"
BUILD="$REPO/build"
. "$HERE/brand.sh"

[ -x "$BUILD/src/$APP_BIN" ] || { echo "no $BUILD/src/$APP_BIN; run: meson setup build && ninja -C build" >&2; exit 2; }

rm -f "$OUT"

# Another instance owning the app ID would receive the window instead of
# our build. Give a previous run (T6 runs this three times) time to exit.
owned () {
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null | grep -q true
}
n=0
while owned && [ $n -lt 20 ]; do sleep 1; n=$((n + 1)); done
if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
     --method org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null | grep -q true; then
  echo "$APP_ID is already running. Quit it first (e.g. $APP_BIN --quit, or flatpak kill $APP_ID)." >&2
  exit 2
fi

mkdir -p "$BUILD/apt-harness" "$BUILD/schemas"
# shellcheck disable=SC2046
gcc -std=gnu11 -shared -fPIC -Wall -Werror -o "$BUILD/apt-harness/app-shot.so" "$HERE/app-shot.c" \
    $(pkg-config --cflags --libs libadwaita-1 gtk4)
cp "$REPO/data/$APP_ID.gschema.xml" "$BUILD/schemas/"
glib-compile-schemas "$BUILD/schemas"

# Bazaar re-executes itself by name for its refresh worker, so build/src
# must be on PATH (upstream behaviour, not the patch).
# The app icon of an uninstalled build, as PNG: GTK's software renderer,
# which app-shot.c uses, cannot draw the SVG icon (GTK 4.23).
ICONS="$BUILD/apt-harness/share"
mkdir -p "$ICONS/icons/hicolor/128x128/apps"
if command -v rsvg-convert >/dev/null && [ -f "$REPO/data/icons/hicolor/scalable/apps/$APP_ID.svg" ]; then
  rsvg-convert -w 128 -h 128 "$REPO/data/icons/hicolor/scalable/apps/$APP_ID.svg" \
    -o "$ICONS/icons/hicolor/128x128/apps/$APP_ID.png"
fi

PATH="$BUILD/src:$PATH" GSETTINGS_SCHEMA_DIR="$BUILD/schemas" \
  XDG_DATA_DIRS="$ICONS:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" \
  G_MESSAGES_DEBUG=BAZAAR::APT,BAZAAR::DEB APP_SHOT="$OUT" APP_SHOT_DELAY="$DELAY" \
  LD_PRELOAD="$BUILD/apt-harness/app-shot.so" \
  timeout $((DELAY + 60)) "$BUILD/src/$APP_BIN" ${APP_SHOT_OPEN:+"$APP_SHOT_OPEN"}

[ -s "$OUT" ] && echo "Saved $OUT"
