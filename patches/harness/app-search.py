#!/usr/bin/env python3
# DOWNSTREAM PATCH (Warsi rebrand): check that app launchers find the app.
# See ai-patches/TEST.md R1.
#
# Runs GLib's desktop-file search, Gio.DesktopAppInfo.search(), which is what
# BlankOn's Praya and GNOME Shell use for their app search, for each query and
# reports whether the app's desktop file is among the results. It searches
# only APPDIR (set XDG_DATA_HOME to its parent and XDG_DATA_DIRS to an empty
# directory), in the language of LANGUAGE / LANG.
#
# Usage: app-search.py DESKTOP_ID QUERY...
#   e.g. env XDG_DATA_HOME=/tmp/x XDG_DATA_DIRS=/nonexistent LANGUAGE=id \
#        app-search.py id.blankonlinux.warsi.desktop warsi store
# Exit 1 if any query does not find it.

import sys

import gi

gi.require_version("Gio", "2.0")
try:
    gi.require_version("GioUnix", "2.0")
    from gi.repository import GioUnix
    search = GioUnix.DesktopAppInfo.search
except (ValueError, ImportError):
    from gi.repository import Gio
    search = Gio.DesktopAppInfo.search


def main(argv):
    if len(argv) < 3:
        print(__doc__ or "usage: app-search.py DESKTOP_ID QUERY...", file=sys.stderr)
        return 2
    desktop_id, queries = argv[1], argv[2:]
    failed = 0
    for query in queries:
        found = any(desktop_id in group for group in search(query))
        print(f"  {query!r:20} {'found' if found else 'NOT FOUND'}")
        failed += not found
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
