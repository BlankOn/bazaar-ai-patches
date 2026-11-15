#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): measure how long the real app's
# main loop freezes when the Search page opens. See ai-patches/TEST.md,
# "Search page freeze" (hook H13). Read-only: clicks nothing.
#
# Needs: a graphical session, a full build in <repo>/build, gcc, pkg-config.
#
# Usage:  ai-patches/patches/harness/stall-probe.sh [--vm] [delay-seconds]
#   --vm  emulate a virtual machine without 3D acceleration: software GL
#         (LIBGL_ALWAYS_SOFTWARE=1, Mesa's llvmpipe), no Mesa shader cache,
#         2 CPU cores. That is where the freeze was seen.
#   GSK_RENDERER=... set by the caller is passed through (e.g. gl, to see
#   the freeze without hook H13's choice).
# Exit 1 when the longest stall is over 1 second.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${BZ_REPO:-$(cd "$HERE/../../.." && pwd)}"
BUILD="$REPO/build"
. "$HERE/brand.sh"

VM=""
[ "$1" = "--vm" ] && { VM=1; shift; }
DELAY="${1:-30}"

[ -x "$BUILD/src/$APP_BIN" ] || { echo "no $BUILD/src/$APP_BIN; build first" >&2; exit 2; }

mkdir -p "$BUILD/apt-harness" "$BUILD/schemas"
# shellcheck disable=SC2046
gcc -std=gnu11 -shared -fPIC -Wall -o "$BUILD/apt-harness/stall-probe.so" "$HERE/stall-probe.c" \
    $(pkg-config --cflags --libs libadwaita-1 gtk4) -lpthread
cp "$REPO/data/$APP_ID.gschema.xml" "$BUILD/schemas/"
glib-compile-schemas "$BUILD/schemas"

set -- "$BUILD/src/$APP_BIN"
if [ -n "$VM" ]; then
  export LIBGL_ALWAYS_SOFTWARE=1 MESA_SHADER_CACHE_DISABLE=true
  set -- taskset -c 0,1 "$@"
fi

LOG="$BUILD/apt-harness/stall-probe.log"
PATH="$BUILD/src:$PATH" GSETTINGS_SCHEMA_DIR="$BUILD/schemas" GSK_DEBUG=renderer \
  G_MESSAGES_DEBUG=BAZAAR::MAIN STALL_PROBE=1 STALL_PROBE_DELAY="$DELAY" STALL_PROBE_TYPE=a \
  LD_PRELOAD="$BUILD/apt-harness/stall-probe.so" \
  timeout $((DELAY + 60)) "$@" > "$LOG" 2>&1 || true

grep -E "Using GTK's|Using renderer|^stall-probe: (switching|t=|typed|longest)" "$LOG" | sed 's/^([a-z-]*:[0-9]*): //' | awk '!seen[$0]++'
echo "(full log with stacks: $LOG)"
stall="$(sed -n 's/^stall-probe: longest main-loop stall after opening Search: \([0-9.,]*\) s$/\1/p' "$LOG" | tr , .)"
[ -n "$stall" ] || { echo "no result"; exit 1; }
awk -v s="$stall" 'BEGIN { exit !(s <= 1.0) }'
