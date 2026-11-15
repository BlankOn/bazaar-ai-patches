#!/usr/bin/env python3
# DOWNSTREAM PATCH (APT system updates): add the patch's translations to
# po/<lang>.po. See ai-patches/DIFF.md, hook H12.
#
# For each ai-patches/patches/po/<lang>-apt-updates.po, appends the entries
# whose msgid po/<lang>.po does not translate yet. Entries po/<lang>.po
# already has (upstream's translations, or a previous run) are left exactly
# as they are, so running it twice changes nothing. An obsolete "#~" entry
# with the same msgid is removed first (gettext rejects the duplicate).
#
# Needs python3 and gettext (msgattrib, msgcat, msgfmt).
# Usage, from the repository root:  python3 ai-patches/patches/po/merge-translations.py
# BZ_REPO=/path/to/bazaar selects another tree.
# Fragment files given as arguments are merged instead of *-apt-updates.po
# (the rebrand merges patches/rebrand/po/<lang>-rebrand.po this way); the
# language is the file name up to the first "-".

import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("BZ_REPO") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def run(*args):
    subprocess.run(args, check=True)


def entries(po_path):
    """(msgid, text) of every non-header entry without a msgctxt, unwrapped.
    Entries with a msgctxt are different messages for gettext, so they do
    not count as translating the context-free msgid."""
    out = subprocess.run(["msgcat", "--no-wrap", po_path], check=True,
                         capture_output=True, text=True).stdout
    result = []
    for block in out.split("\n\n"):
        lines = block.strip("\n").splitlines()
        if any(l.startswith("msgctxt ") for l in lines):
            continue
        ids = [l[len("msgid "):] for l in lines if l.startswith("msgid ")]
        if ids and ids[0] != '""':
            result.append((ids[0], "\n".join(lines)))
    return result


def drop_obsolete(text, ids):
    """Remove "#~" blocks whose msgid is in ids."""
    blocks = text.split("\n\n")
    kept = []
    for block in blocks:
        lines = [l for l in block.splitlines() if l.strip()]
        if lines and all(l.startswith("#") for l in lines):
            mid = [l[len("#~ msgid "):] for l in lines if l.startswith("#~ msgid ")]
            if mid and mid[0] in ids:
                continue
        kept.append(block)
    return "\n\n".join(kept)


def main():
    frags = sys.argv[1:] or sorted(glob.glob(os.path.join(HERE, "*-apt-updates.po")))
    for frag in frags:
        lang = os.path.basename(frag).split("-")[0]
        target = os.path.join(REPO, "po", lang + ".po")
        if not os.path.exists(target):
            print(f"skip {lang}: no {target}")
            continue
        with tempfile.TemporaryDirectory() as tmp:
            active = os.path.join(tmp, "active.po")
            run("msgattrib", "--no-obsolete", "--translated", target, "-o", active)
            present = {mid for mid, _ in entries(active)}
            new = os.path.join(tmp, "new.po")
            # the fragment's entries the target does not translate yet
            body = [text for mid, text in entries(frag) if mid not in present]
            missing = {mid for mid, _ in entries(frag) if mid not in present}
            if not body:
                print(f"{lang}: nothing to add")
                continue
            with open(target, encoding="utf-8") as f:
                text = f.read()
            text = drop_obsolete(text, missing).rstrip("\n") + "\n\n" + "\n\n".join(body) + "\n"
            with open(new, "w", encoding="utf-8") as f:
                f.write(text)
            run("msgfmt", "-c", "-o", os.devnull, new)
            with open(target, "w", encoding="utf-8") as f:
                f.write(text)
            print(f"{lang}: added {len(body)} entries to {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
