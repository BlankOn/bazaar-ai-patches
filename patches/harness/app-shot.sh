#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): screenshot the real Bazaar app's
# Library page. See ai-patches/TEST.md. Read-only: clicks nothing.
#
# Needs: a graphical session, a full build in <repo>/build (meson/ninja),
# gcc, pkg-config, glib-compile-schemas, gdbus.
#
# Usage:  ai-patches/patches/harness/app-shot.sh OUT.png [delay-seconds]
#   APP_SHOT_EXPAND=1 also expands the APT package list.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${BZ_REPO:-$(cd "$HERE/../../.." && pwd)}"
OUT="$(realpath -m "${1:?usage: app-shot.sh OUT.png [delay-seconds]}")"
DELAY="${2:-40}"
BUILD="$REPO/build"

[ -x "$BUILD/src/bazaar" ] || { echo "no $BUILD/src/bazaar; run: meson setup build && ninja -C build" >&2; exit 2; }

# Another Bazaar (e.g. the Flatpak) owning the app ID would receive the
# window instead of our build.
if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
     --method org.freedesktop.DBus.NameHasOwner io.github.kolunmi.Bazaar 2>/dev/null | grep -q true; then
  echo "A Bazaar instance is already running. Quit it first (flatpak kill io.github.kolunmi.Bazaar)." >&2
  exit 2
fi

mkdir -p "$BUILD/apt-harness" "$BUILD/schemas"
# shellcheck disable=SC2046
gcc -std=gnu11 -shared -fPIC -Wall -Werror -o "$BUILD/apt-harness/app-shot.so" "$HERE/app-shot.c" \
    $(pkg-config --cflags --libs libadwaita-1 gtk4)
cp "$REPO/data/io.github.kolunmi.Bazaar.gschema.xml" "$BUILD/schemas/"
glib-compile-schemas "$BUILD/schemas"

# Bazaar re-executes itself by name for its refresh worker, so build/src
# must be on PATH (upstream behaviour, not the patch).
PATH="$BUILD/src:$PATH" GSETTINGS_SCHEMA_DIR="$BUILD/schemas" \
  G_MESSAGES_DEBUG=BAZAAR::APT APP_SHOT="$OUT" APP_SHOT_DELAY="$DELAY" \
  LD_PRELOAD="$BUILD/apt-harness/app-shot.so" \
  timeout $((DELAY + 60)) "$BUILD/src/bazaar"

[ -s "$OUT" ] && echo "Saved $OUT"
