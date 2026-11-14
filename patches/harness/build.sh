#!/bin/sh
# DOWNSTREAM PATCH (APT system updates): build the standalone test harness.
# See ai-patches/TEST.md.
#
# Needs only: gcc, pkg-config, libadwaita-1-dev, libgtk-4-dev, blueprint-compiler
# (NOT the full Bazaar build dependencies).
#
# Usage:  ai-patches/patches/harness/build.sh [output-dir]
#   output-dir defaults to <repo>/build/apt-harness (git-ignored).
#
# By default it compiles the copies already applied in <repo>/src.
# Set BZ_APT_SRC to compile other copies, e.g. the master copies before
# applying them:  BZ_APT_SRC=ai-patches/patches/src ai-patches/patches/harness/build.sh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SRC="$(cd "${BZ_APT_SRC:-$REPO/src}" && pwd)"
OUT="${1:-$REPO/build/apt-harness}"

mkdir -p "$OUT"
cd "$OUT"

printf '#define GETTEXT_PACKAGE "bazaar"\n' > config.h
cat > res.xml <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<gresources>
  <gresource prefix="/io/github/kolunmi/Bazaar">
    <file>bz-apt-updates-card.ui</file>
  </gresource>
</gresources>
EOF

blueprint-compiler compile --output bz-apt-updates-card.ui "$SRC/bz-apt-updates-card.blp"
glib-compile-resources --generate-source --target=res.c res.xml

CFLAGS_ADW="$(pkg-config --cflags libadwaita-1 gtk4)"
LIBS_ADW="$(pkg-config --libs libadwaita-1 gtk4)"

# Same strictness as Bazaar's own build for the patch sources
# shellcheck disable=SC2086
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -Wdeclaration-after-statement -Wshadow -Wmissing-declarations \
    -I. -I"$SRC" $CFLAGS_ADW \
    -c "$SRC/bz-apt-updates.c" "$SRC/bz-apt-updates-card.c"
# shellcheck disable=SC2086
gcc -std=gnu11 -I. -I"$SRC" $CFLAGS_ADW -c "$HERE/harness.c" res.c
# shellcheck disable=SC2086
gcc bz-apt-updates.o bz-apt-updates-card.o harness.o res.o -o harness $LIBS_ADW

echo "Built $OUT/harness from $SRC"
echo "Run:  HARNESS_ICONS=$REPO/src/icons G_MESSAGES_DEBUG=BAZAAR::APT $OUT/harness"
