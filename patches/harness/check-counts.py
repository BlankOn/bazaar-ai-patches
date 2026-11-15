#!/usr/bin/env python3
# DOWNSTREAM PATCH (APT system updates): count check. See ai-patches/TEST.md.
#
# Runs the standalone harness, reads the counts the card's model logged
# ("GetUpdates: U upgrades (S security), N new, R removals") and compares
# them with what PackageKit and apt report independently.
#
# Needs: a graphical session, the harness built (patches/harness/build.sh),
# python3-gi with the PackageKit typelib (gir1.2-packagekitglib-1.0) and apt.
# Read-only: it never changes the system.
#
# Usage:  python3 -I ai-patches/patches/harness/check-counts.py [harness-binary]
# Exit status: 0 = all checks pass, 1 = a check failed, 2 = could not run.

import collections
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("BZ_REPO") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))
HARNESS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "apt-harness", "harness")
LINE = re.compile(r"GetUpdates: (\d+) upgrades \((\d+) security\), (\d+) new, (\d+) removals")

failed = False


def check(ok, what):
    global failed
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        failed = True


# 1. What the card's model says
if not os.access(HARNESS, os.X_OK):
    sys.exit(f"harness not found at {HARNESS}; run ai-patches/patches/harness/build.sh first")

with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ,
               G_MESSAGES_DEBUG="BAZAAR::APT",
               HARNESS_SHOT=os.path.join(tmp, "shot.png"),
               HARNESS_DELAY=os.environ.get("HARNESS_DELAY", "15"),
               HARNESS_ICONS=os.path.join(REPO, "src", "icons"))
    run = subprocess.run([HARNESS], env=env, capture_output=True, text=True, timeout=180)

matches = LINE.findall(run.stdout + run.stderr)
if not matches:
    print(run.stdout + run.stderr)
    print("Could not find the GetUpdates debug line. Is PackageKit running? "
          "Did the card load within HARNESS_DELAY seconds?")
    sys.exit(2)
card_up, card_sec, card_new, card_rm = map(int, matches[-1])
print(f"card:        {card_up} upgrades ({card_sec} security), {card_new} new, {card_rm} removals")

# 2. What PackageKit says (same data source, via libpackagekit-glib2)
try:
    import gi
    gi.require_version("PackageKitGlib", "1.0")
    from gi.repository import PackageKitGlib as pk
except (ImportError, ValueError) as e:
    print(f"PackageKit Python bindings missing ({e}); install gir1.2-packagekitglib-1.0")
    sys.exit(2)

pkgs = pk.Client().get_updates(pk.FilterEnum.NONE, None, lambda *a: None, None).get_package_array()
kinds = collections.Counter(pk.info_enum_to_string(p.get_info()) for p in pkgs)
pk_rm = kinds["remove"] + kinds["obsolete"]
pk_new = kinds["install"]
pk_sec = kinds["security"] + kinds["critical"]
pk_up = len(pkgs) - kinds["blocked"] - pk_rm - pk_new
print(f"PackageKit:  {pk_up} upgrades ({pk_sec} security), {pk_new} new, {pk_rm} removals"
      f"  raw={dict(kinds)}")

check(card_up == pk_up, "card upgrades == PackageKit upgrades")
check(card_sec == pk_sec, "card security == PackageKit security")
check(card_new == pk_new, "card new == PackageKit new")
check(card_rm == pk_rm, "card removals == PackageKit removals")

# 3. What apt says (independent of PackageKit). apt lists upgrades only.
pk_upgrades = {(p.get_name(), p.get_arch()) for p in pkgs
               if pk.info_enum_to_string(p.get_info()) not in ("blocked", "install", "remove", "obsolete")}
apt_out = subprocess.run(["apt", "list", "--upgradable"], capture_output=True, text=True).stdout
apt_upgrades = set()
for line in apt_out.splitlines():
    if "/" not in line or "[upgradable" not in line:
        continue
    name, rest = line.split("/", 1)
    apt_upgrades.add((name, rest.split()[2]))
print(f"apt:         {len(apt_upgrades)} upgradable")

check(card_up == len(apt_upgrades), "card upgrades == apt list --upgradable")
if pk_upgrades != apt_upgrades:
    print("      only in PackageKit:", sorted(pk_upgrades - apt_upgrades)[:20])
    print("      only in apt:       ", sorted(apt_upgrades - pk_upgrades)[:20])
    print("      (a small difference can be legitimate: PackageKit hides blocked/kept-back packages)")

removed = sorted(p.get_name() for p in pkgs if pk.info_enum_to_string(p.get_info()) in ("remove", "obsolete"))
if removed:
    print("note: the upgrade would remove:", ", ".join(removed))

sys.exit(1 if failed else 0)
