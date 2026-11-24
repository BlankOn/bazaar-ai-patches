#!/bin/sh
# DOWNSTREAM PATCH (GNOME Extensions): test the Extensions tab's model
# against the real extensions.gnome.org and a fake GNOME Shell on a private
# session bus. See ai-patches/TEST.md E1. Installs, enables and removes
# nothing on this computer: every Shell call goes to the fake.
#
# Needs: network access, dbus-run-session, python3-gi, gcc, pkg-config,
# libgtk-4-dev, libsoup-3.0-dev, libjson-glib-dev.
#
# Usage:  ai-patches/patches/harness/ext-check.sh
# Exit 1 when a check fails.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${BZ_REPO:-$(cd "$HERE/../../.." && pwd)}"
SRC="$(cd "${BZ_EXT_SRC:-$REPO/src}" && pwd)"
OUT="$REPO/build/apt-tests/ext"
. "$HERE/brand.sh"

rm -rf "$OUT" && mkdir -p "$OUT"
cd "$OUT"

printf '#define GETTEXT_PACKAGE "%s"\n' "$GETTEXT_DOMAIN" > config.h
# shellcheck disable=SC2046
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -Wdeclaration-after-statement -Wshadow -Wmissing-declarations \
    -I. -I"$SRC" $(pkg-config --cflags gtk4 libsoup-3.0 json-glib-1.0) \
    -o ext-check "$SRC/bz-gnome-extensions.c" "$HERE/ext-check.c" \
    $(pkg-config --libs gtk4 libsoup-3.0 json-glib-1.0)

# The fake Shell owns org.gnome.Shell.Extensions on the private bus only.
LANGUAGE=C dbus-run-session -- sh -c "
  python3 -I '$HERE/fake-shell-extensions.py' 2>'$OUT/fake-shell.log' &
  for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready '$OUT/fake-shell.log' 2>/dev/null && break; sleep 0.5; done
  ./ext-check
"
