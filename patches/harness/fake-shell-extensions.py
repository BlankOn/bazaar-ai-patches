#!/usr/bin/env python3
# DOWNSTREAM PATCH (GNOME Extensions): a stand-in for GNOME Shell's
# org.gnome.Shell.Extensions D-Bus service, for tests on a private session
# bus (dbus-run-session). See ai-patches/TEST.md E1 and E2.
#
# It installs, enables and removes nothing on the computer: it only keeps a
# list in memory and answers like GNOME Shell does (ExtensionStateChanged
# after each change). Every method call is printed to stderr as
# "fake-shell: METHOD ARGS".
#
# Usage: python3 -I fake-shell-extensions.py [--shell-version 50.4]
# Starts with three "installed" extensions: one from extensions.gnome.org,
# enabled; one disabled with preferences; one system extension in error.

import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

XML = """
<node>
  <interface name="org.gnome.Shell.Extensions">
    <method name="ListExtensions"><arg type="a{sa{sv}}" direction="out"/></method>
    <method name="GetExtensionInfo"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
    <method name="GetExtensionErrors"><arg type="s" direction="in"/><arg type="as" direction="out"/></method>
    <method name="InstallRemoteExtension"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
    <method name="UninstallExtension"><arg type="s" direction="in"/><arg type="b" direction="out"/></method>
    <method name="ReloadExtension"><arg type="s" direction="in"/></method>
    <method name="EnableExtension"><arg type="s" direction="in"/><arg type="b" direction="out"/></method>
    <method name="DisableExtension"><arg type="s" direction="in"/><arg type="b" direction="out"/></method>
    <method name="LaunchExtensionPrefs"><arg type="s" direction="in"/></method>
    <method name="OpenExtensionPrefs"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="CheckForUpdates"/>
    <signal name="ExtensionStateChanged"><arg type="s"/><arg type="a{sv}"/></signal>
    <signal name="ExtensionStatusChanged"><arg type="s"/><arg type="i"/><arg type="s"/></signal>
    <property name="ShellVersion" type="s" access="read"/>
    <property name="UserExtensionsEnabled" type="b" access="readwrite"/>
  </interface>
</node>
"""

PATH = "/org/gnome/Shell/Extensions"
IFACE = "org.gnome.Shell.Extensions"
ACTIVE, INACTIVE, ERROR, UNINSTALLED = 1, 2, 3, 99
SYSTEM, PER_USER = 1, 2

shell_version = "50.4"
if "--shell-version" in sys.argv:
    shell_version = sys.argv[sys.argv.index("--shell-version") + 1]

user_extensions_enabled = True
extensions = {
    "dash-to-dock@micxgx.gmail.com": dict(
        name="Dash to Dock", description="A dock for the Gnome Shell.",
        state=ACTIVE, enabled=True, type=PER_USER, hasPrefs=True, error=""),
    "caffeine@patapon.info": dict(
        name="Caffeine", description="Disable the screensaver and auto suspend",
        state=INACTIVE, enabled=False, type=PER_USER, hasPrefs=True, error=""),
    "fake-system@example.invalid": dict(
        name="Fake System Extension", description="Installed by the system; cannot be removed.",
        state=ERROR, enabled=True, type=SYSTEM, hasPrefs=False,
        error="TypeError: this is a fake error"),
}

connection = None


def info(uuid):
    e = extensions[uuid]
    return {
        "uuid": GLib.Variant("s", uuid),
        "name": GLib.Variant("s", e["name"]),
        "description": GLib.Variant("s", e["description"]),
        "state": GLib.Variant("d", float(e["state"])),
        "enabled": GLib.Variant("b", e["enabled"]),
        "type": GLib.Variant("d", float(e["type"])),
        "hasPrefs": GLib.Variant("b", e["hasPrefs"]),
        "hasUpdate": GLib.Variant("b", False),
        "canChange": GLib.Variant("b", True),
        "error": GLib.Variant("s", e["error"]),
        "url": GLib.Variant("s", ""),
    }


def changed(uuid, state_info):
    connection.emit_signal(None, PATH, IFACE, "ExtensionStateChanged",
                           GLib.Variant("(sa{sv})", (uuid, state_info)))


def set_state(uuid, enabled):
    e = extensions.get(uuid)
    if e is None:
        return False
    e["enabled"] = enabled
    if e["state"] != ERROR:
        e["state"] = ACTIVE if enabled else INACTIVE
    changed(uuid, info(uuid))
    return True


def on_call(conn, sender, path, iface, method, params, invocation):
    args = params.unpack()
    print("fake-shell:", method, *args, file=sys.stderr, flush=True)
    if method == "ListExtensions":
        invocation.return_value(GLib.Variant("(a{sa{sv}})", ({u: info(u) for u in extensions},)))
    elif method == "GetExtensionInfo":
        invocation.return_value(GLib.Variant("(a{sv})", (info(args[0]) if args[0] in extensions else {},)))
    elif method == "GetExtensionErrors":
        e = extensions.get(args[0])
        invocation.return_value(GLib.Variant("(as)", ([e["error"]] if e and e["error"] else [],)))
    elif method == "InstallRemoteExtension":
        uuid = args[0]
        # GNOME Shell would ask the user here; the fake accepts.
        extensions[uuid] = dict(name=uuid, description="", state=ACTIVE, enabled=True,
                                type=PER_USER, hasPrefs=False, error="")
        changed(uuid, info(uuid))
        invocation.return_value(GLib.Variant("(s)", ("successful",)))
    elif method == "UninstallExtension":
        uuid = args[0]
        ok = uuid in extensions and extensions[uuid]["type"] == PER_USER
        if ok:
            gone = info(uuid)
            gone["state"] = GLib.Variant("d", float(UNINSTALLED))
            del extensions[uuid]
            changed(uuid, gone)
        invocation.return_value(GLib.Variant("(b)", (ok,)))
    elif method in ("EnableExtension", "DisableExtension"):
        invocation.return_value(GLib.Variant("(b)", (set_state(args[0], method == "EnableExtension"),)))
    else:
        invocation.return_value(None)


def on_get(conn, sender, path, iface, prop):
    if prop == "ShellVersion":
        return GLib.Variant("s", shell_version)
    if prop == "UserExtensionsEnabled":
        return GLib.Variant("b", user_extensions_enabled)
    return None


def on_set(conn, sender, path, iface, prop, value):
    global user_extensions_enabled
    print("fake-shell: Set", prop, value.unpack(), file=sys.stderr, flush=True)
    if prop != "UserExtensionsEnabled":
        return False
    user_extensions_enabled = value.unpack()
    conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                     GLib.Variant("(sa{sv}as)", (IFACE, {"UserExtensionsEnabled": value}, [])))
    return True


def on_bus(conn, name):
    global connection
    connection = conn
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object(PATH, node.interfaces[0], on_call, on_get, on_set)


def on_name(conn, name):
    print("fake-shell: ready", file=sys.stderr, flush=True)


def on_lost(conn, name):
    print("fake-shell: could not own", name, file=sys.stderr, flush=True)
    sys.exit(1)


Gio.bus_own_name(Gio.BusType.SESSION, "org.gnome.Shell.Extensions",
                 Gio.BusNameOwnerFlags.NONE, on_bus, on_name, on_lost)
GLib.MainLoop().run()
