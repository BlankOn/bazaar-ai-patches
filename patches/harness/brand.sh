# DOWNSTREAM PATCH (APT system updates): sourced by the harness scripts.
# Reads the app ID, program name and text domain from $REPO/meson.build, so
# the tests work on Bazaar and on the Warsi rebrand (patches/rebrand/).
# Sets APP_ID, APP_BIN, GETTEXT_DOMAIN and REBRANDED (1 or 0).
APP_ID="$(sed -n "s/.*set_quoted('APPLICATION_ID', '\([^']*\)').*/\1/p" "$REPO/meson.build" | head -n 1)"
APP_BIN="$(sed -n "s/^ *bazaar_bin_name = '\([^']*\)'.*/\1/p" "$REPO/meson.build" | head -n 1)"
GETTEXT_DOMAIN="$(sed -n "s/.*set_quoted('GETTEXT_PACKAGE', '\([^']*\)').*/\1/p" "$REPO/meson.build" | head -n 1)"
if [ "$APP_ID" = io.github.kolunmi.Bazaar ]; then REBRANDED=0; else REBRANDED=1; fi
