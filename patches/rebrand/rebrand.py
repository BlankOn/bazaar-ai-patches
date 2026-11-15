#!/usr/bin/env python3
# DOWNSTREAM PATCH (Warsi rebrand): turn the patched Bazaar tree into Warsi.
# See ai-patches/PLAN.md, "Rebrand", and ai-patches/DIFF.md, step R.
#
# Run it last, after the APT patch (DIFF.md) and its translations are in.
# It changes, in meson.build, src/, data/ and po/ only:
#
#   1. files named after the app ID         io.github.kolunmi.Bazaar.*  -> id.blankonlinux.warsi.*
#   2. the app ID and resource path         everywhere in those files (GApplication, D-Bus
#                                           names, GSettings schema, icons, caches, keyring)
#   3. installed program and file names     bazaar -> warsi, bazaar-daemon -> warsi-daemon,
#                                           bazaar-dl-worker, libbazaar-login-page.so,
#                                           gettext domain, autostart file, ...
#   4. strings naming the app               "Bazaar" -> "Warsi" in the source (EDITS) and the
#                                           matching msgid/msgstr in every po/<lang>.po.
#                                           Strings about upstream keep "Bazaar": "The Bazaar
#                                           Contributors", donations, copyright, links.
#   5. new code and art                     src/bz-rebrand.[ch] (About dialog note),
#                                           the Warsi icons, and their translations.
#
# Every edit names the exact upstream text it replaces. If upstream changes
# that text, the script stops and says which edit no longer applies rather
# than half-rebranding. Running it twice changes nothing.
#
# Usage, from the repository root:
#   python3 ai-patches/patches/rebrand/rebrand.py           apply
#   python3 ai-patches/patches/rebrand/rebrand.py --check   exit 1 if applying would change anything
#   python3 ai-patches/patches/rebrand/rebrand.py --filter FILE RELPATH
#        print FILE as it would read after the rebrand, if it lived at RELPATH (for comparing
#        the master copies in ai-patches/patches/ with a rebranded tree)
# BZ_REPO=/path/to/tree selects another tree. Needs python3 and gettext.

import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("BZ_REPO") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))

OLD_ID, NEW_ID = "io.github.kolunmi.Bazaar", "id.blankonlinux.warsi"
OLD_PATH, NEW_PATH = "/io/github/kolunmi/Bazaar", "/id/blankonlinux/warsi"
OLD_NAME, NEW_NAME = "Bazaar", "Warsi"

SCOPE_DIRS = ["src", "data", "po"]
SCOPE_FILES = ["meson.build"]

# (pattern, replacement), applied to every text file in scope, in order.
TOKENS = [
    (re.compile(re.escape(OLD_ID)), NEW_ID),
    (re.compile(re.escape(OLD_PATH)), NEW_PATH),
    # programs; the source file bazaar-daemon.c keeps its name
    (re.compile(r"bazaar-daemon(?!\.[ch]\b)"), "warsi-daemon"),
    (re.compile(r"bazaar-dl-worker"), "warsi-dl-worker"),
    (re.compile(r"bazaar-host-bwrap"), "warsi-host-bwrap"),
    (re.compile(r"bazaar-login-page"), "warsi-login-page"),
    # translator comments: the rebranded tree has both names in its strings
    (re.compile(r'Please treat "Bazaar" as a brand name'),
     'Please treat "Warsi" and "Bazaar" as brand names'),
]

META = "data/" + NEW_ID + ".metainfo.xml.in"
ABOUT_NOTE_HOOK = (
    "  /* DOWNSTREAM PATCH (Warsi rebrand): \"Originally Bazaar, patched by BlankOn\n"
    "   * Linux\" under the name; see ai-patches/PLAN.md, \"Rebrand\". */\n"
    "  bz_rebrand_about_dialog (ADW_ABOUT_DIALOG (dialog));\n\n")

# (relative path after step 1, upstream text, rebranded text). Every
# occurrence is replaced. An edit whose rebranded text contains the upstream
# text (an insertion) is skipped once the rebranded text is present.
EDITS = [
    # build: project, gettext domain, installed names
    ("meson.build", "project('bazaar',", "project('warsi',"),
    ("meson.build", "config_h.set_quoted('GETTEXT_PACKAGE', 'bazaar')",
     "config_h.set_quoted('GETTEXT_PACKAGE', 'warsi')"),
    ("meson.build", "bazaar_bin_name = 'bazaar'", "bazaar_bin_name = 'warsi'"),
    ("meson.build", "get_option('libdir') / 'bazaar'", "get_option('libdir') / 'warsi'"),
    ("meson.build", "get_option('datadir') / 'bazaar'", "get_option('datadir') / 'warsi'"),
    ("po/meson.build", "i18n.gettext('bazaar',", "i18n.gettext('warsi',"),
    ("src/meson.build", "  'bz-application.c',\n",
     "  'bz-application.c',\n  # DOWNSTREAM PATCH (Warsi rebrand)\n  'bz-rebrand.c',\n"),
    ("po/POTFILES.in", "src/bz-application.c\n", "src/bz-application.c\nsrc/bz-rebrand.c\n"),
    # autostart files
    ("src/bazaar.gresource.xml", 'alias="bazaar.desktop"', 'alias="warsi.desktop"'),
    ("src/bazaar-autostart.desktop", "Name=Bazaar", "Name=Warsi"),
    ("src/bz-preferences-dialog.c", '"bazaar.desktop"', '"warsi.desktop"'),
    ("src/bz-preferences-dialog.c", '/bazaar.desktop"', '/warsi.desktop"'),
    ("data/bazaar-update-notifications.desktop", "Name=Bazaar", "Name=Warsi"),
    ("data/bazaar-update-notifications.desktop", "autostart/bazaar.desktop. Runs Bazaar's",
     "autostart/warsi.desktop. Runs Warsi's"),
    ("data/meson.build", "rename: 'bazaar.desktop',", "rename: 'warsi.desktop',"),
    ("data/meson.build", "Named bazaar.desktop like", "Named warsi.desktop like"),
    # desktop file and AppStream metadata
    ("data/" + NEW_ID + ".desktop.in", "Name=Bazaar", "Name=Warsi"),
    (META, "<name>Bazaar</name>", "<name>Warsi</name>"),
    (META, '<translation type="gettext">bazaar</translation>',
     '<translation type="gettext">warsi</translation>'),
    (META, "particularly from Flathub.\n    </p>\n",
     "particularly from Flathub.\n    </p>\n"
     "    <p>\n"
     "      Warsi is based on Bazaar and patched by BlankOn Linux. Besides\n"
     "      Flatpak apps, it also keeps the system's Debian APT packages up to date.\n"
     "    </p>\n"),
    # About dialog: name, and the note (bz-rebrand.c). Developer, website,
    # issue link, credits and copyright stay Bazaar's.
    ("src/bz-application.c", '#include "bz-application.h"\n',
     '#include "bz-application.h"\n'
     '/* DOWNSTREAM PATCH (Warsi rebrand) */\n#include "bz-rebrand.h"\n'),
    ("src/bz-application.c", '"application-name", "Bazaar",', '"application-name", "Warsi",'),
    ("src/bz-application.c",
     "      special_thanks);\n\n  adw_dialog_present (dialog, GTK_WIDGET (window));\n",
     "      special_thanks);\n\n" + ABOUT_NOTE_HOOK +
     "  adw_dialog_present (dialog, GTK_WIDGET (window));\n"),
    # strings naming the app (their msgids are renamed in po/, see RENAMED)
    ("src/bz-application.c", "applications in Bazaar if its", "applications in Warsi if its"),
    ("src/bz-application.c", "You can still use Bazaar to", "You can still use Warsi to"),
    ("src/bz-window.blp", '_("Bazaar")', '_("Warsi")'),
    ("src/bz-window.blp", "a new version of Bazaar!", "a new version of Warsi!"),
    ("src/bz-window.blp", '_("_About Bazaar")', '_("_About Warsi")'),
    ("src/bz-window.blp", '_("_Quit Bazaar")', '_("_Quit Warsi")'),
    ("src/bz-window.c", '_ ("Bazaar")', '_ ("Warsi")'),
    ("src/bz-window.c", '_ ("Bazaar — %s")', '_ ("Warsi — %s")'),
    ("src/bz-window.c", "You can't remove Bazaar from Bazaar!", "You can't remove Warsi from Warsi!"),
    ("src/bz-bundle-install-dialog.blp", "will show up in Bazaar.", "will show up in Warsi."),
    ("src/bz-preferences-dialog.c", "Bazaar needs to run in the background",
     "Warsi needs to run in the background"),
    ("src/shortcuts-dialog.blp", '"Quit Bazaar"', '"Quit Warsi"'),
    ("src/bz-inspector.c", 'g_value_set_string (value, "Bazaar");',
     'g_value_set_string (value, "Warsi");'),
]

# msgids (as written in .po files, unwrapped) renamed Bazaar -> Warsi in
# every po/<lang>.po, matching the EDITS above, so no translation is lost.
RENAMED = [
    "Bazaar",
    r"Flathub is not set up on this system. You will not be able to browse and "
    r"install applications in Bazaar if its unavailable.\n\nYou can still use "
    r"Bazaar to browse and remove already installed apps.",
    r"Installing this app may require adding a new software source. Other apps "
    r"from this source will show up in Bazaar.\n\nOnly add this source if you're "
    r"sure you trust it.",
    "Bazaar needs to run in the background to check for app updates",
    "You are running a new version of Bazaar!",
    "_About Bazaar",
    "_Quit Bazaar",
    "Bazaar — %s",
    "You can't remove Bazaar from Bazaar!",
    "Quit Bazaar",
]

ICON_DIR = "data/icons/hicolor"
ICONS = {
    ICON_DIR + "/scalable/apps/" + NEW_ID + ".svg": "icons/app.svg",
    ICON_DIR + "/scalable/apps/" + NEW_ID + ".Devel.svg": "icons/app-devel.svg",
    ICON_DIR + "/symbolic/apps/" + NEW_ID + "-symbolic.svg": "icons/app-symbolic.svg",
}
REMOVED = ["data/icons/" + NEW_ID + ".Source.svg"]  # Bazaar's logo, Inkscape source


class Drift(Exception):
    pass


def scope_files(repo):
    for name in SCOPE_FILES:
        path = os.path.join(repo, name)
        if os.path.isfile(path):
            yield path
    for d in SCOPE_DIRS:
        for root, dirs, files in os.walk(os.path.join(repo, d)):
            dirs.sort()
            for f in sorted(files):
                yield os.path.join(root, f)


def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except (UnicodeDecodeError, IsADirectoryError):
        return None


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def apply_tokens(text):
    for pattern, new in TOKENS:
        text = pattern.sub(new, text)
    return text


def apply_edits(rel, text, strict):
    for path, old, new in EDITS:
        if path != rel:
            continue
        if old in new and new in text:
            continue                      # insertion already made
        if old in text:
            text = text.replace(old, new)
        elif new not in text and strict:
            raise Drift(f"{rel}: upstream text not found: {old!r}")
    return text


def rename_files(repo):
    for d in SCOPE_DIRS:
        for root, dirs, files in os.walk(os.path.join(repo, d)):
            for f in files:
                if OLD_ID in f:
                    os.rename(os.path.join(root, f), os.path.join(root, f.replace(OLD_ID, NEW_ID)))


def name_stem(blocks):
    """Three letters from the middle of this language's name for the app
    (its msgstr for "Bazaar"): "aza" for Bazar, "азa" for Базар, "asa" for
    Basar, "ија" for Пијаца. Found inside declined or mutated forms too
    (Bazárból, Базару, Vasar)."""
    for block in blocks:
        if '\nmsgid "Bazaar"\n' in "\n" + block + "\n":
            m = re.search(r'^msgstr "(.+)"$', block, re.M)
            if m and len(m.group(1)) >= 4:
                return m.group(1)[1:4].lower()
    return "aza"


def rename_msgids(text):
    """Rename the RENAMED msgids (and their translations) in one .po file.
    "Bazaar" in a translation becomes "Warsi". A translation that names the
    app some other way (Bazar, Базару, Пијаца, or an inflected Bazaaru) is marked fuzzy, so English
    shows until a translator updates it; one that does not name the app at
    all ("About the app") is kept as it is."""
    blocks = text.split("\n\n")
    stem = name_stem(blocks)
    for n, block in enumerate(blocks):
        lines = block.split("\n")
        if all(l.startswith("#~") or not l for l in lines):
            continue
        idx = next((i for i, l in enumerate(lines) if l.startswith('msgid "')), None)
        if idx is None:
            continue
        end = idx + 1
        while end < len(lines) and lines[end].startswith('"'):
            end += 1
        msgid = "".join(re.findall(r'"(.*)"', "\n".join(lines[idx:end])))
        if msgid not in RENAMED:
            continue
        new_id = msgid.replace(OLD_NAME, NEW_NAME)
        lines[idx:end] = ['msgid "' + new_id + '"']
        s = idx + 1
        e = s
        while e < len(lines) and (lines[e].startswith("msgstr") or lines[e].startswith('"')):
            e += 1
        msgstr = "".join(re.findall(r'"(.*)"', "\n".join(lines[s:e])))
        # "Bazaar" with an ending (Polish Bazaaru, Bazaarze) needs a translator
        inflected = re.search(OLD_NAME + r"(?=\w)", msgstr) is not None
        if OLD_NAME in msgstr and not inflected:
            lines[s:e] = ['msgstr "' + msgstr.replace(OLD_NAME, NEW_NAME) + '"']
        elif inflected or stem in msgstr.lower():
            flags = next((i for i, l in enumerate(lines) if l.startswith("#,")), None)
            if flags is None:
                first = next(i for i, l in enumerate(lines) if not l.startswith("#") or l.startswith("#|"))
                lines.insert(first, "#, fuzzy")
            elif "fuzzy" not in lines[flags]:
                lines[flags] += ", fuzzy"
        blocks[n] = "\n".join(lines)
    return "\n\n".join(blocks)


def apply(repo):
    rename_files(repo)

    for path in scope_files(repo):
        rel = os.path.relpath(path, repo)
        text = read_text(path)
        if text is None:
            continue
        new = apply_edits(rel, apply_tokens(text), strict=True)
        if rel.startswith("po" + os.sep) and rel.endswith(".po"):
            new = rename_msgids(new)
        if new != text:
            write_text(path, new)

    for rel in set(p for p, _, _ in EDITS):
        if not os.path.exists(os.path.join(repo, rel)):
            raise Drift(f"{rel}: file not found")

    for f in sorted(os.listdir(os.path.join(HERE, "src"))):
        shutil.copyfile(os.path.join(HERE, "src", f), os.path.join(repo, "src", f))
    for rel, art in ICONS.items():
        shutil.copyfile(os.path.join(HERE, art), os.path.join(repo, rel))
    for rel in REMOVED:
        if os.path.exists(os.path.join(repo, rel)):
            os.remove(os.path.join(repo, rel))

    frags = sorted(os.path.join(HERE, "po", f) for f in os.listdir(os.path.join(HERE, "po")))
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "po", "merge-translations.py"), *frags],
                   check=True, env={**os.environ, "BZ_REPO": repo}, stdout=subprocess.DEVNULL)

    problems = []
    for path in scope_files(repo):
        text = read_text(path)
        if text is not None and (OLD_ID in text or OLD_PATH in text):
            problems.append(os.path.relpath(path, repo))
        if path.endswith(".po"):
            if subprocess.run(["msgfmt", "-c", "-o", os.devnull, path]).returncode != 0:
                problems.append(os.path.relpath(path, repo) + " (msgfmt)")
    if problems:
        raise Drift("old app ID left in, or invalid: " + ", ".join(problems))


def copy_scope(src, dst):
    for name in SCOPE_FILES:
        shutil.copy2(os.path.join(src, name), os.path.join(dst, name))
    for d in SCOPE_DIRS:
        shutil.copytree(os.path.join(src, d), os.path.join(dst, d), symlinks=True)


def diff_trees(a, b, rel=""):
    cmp = filecmp.dircmp(os.path.join(a, rel), os.path.join(b, rel))
    out = [os.path.join(rel, f) for f in cmp.left_only + cmp.right_only + cmp.diff_files + cmp.funny_files]
    for d in cmp.common_dirs:
        out += diff_trees(a, b, os.path.join(rel, d))
    return out


def check(repo):
    with tempfile.TemporaryDirectory() as tmp:
        copy_scope(repo, tmp)
        apply(tmp)
        changed = []
        for d in SCOPE_DIRS:
            changed += [os.path.join(d, f) for f in diff_trees(os.path.join(repo, d), os.path.join(tmp, d))]
        changed += [f for f in SCOPE_FILES if not filecmp.cmp(os.path.join(repo, f), os.path.join(tmp, f), False)]
    for f in changed:
        print("would change:", f)
    return 1 if changed else 0


def main(argv):
    try:
        if argv[:1] == ["--check"]:
            return check(REPO)
        if argv[:1] == ["--filter"] and len(argv) == 3:
            text = read_text(argv[1])
            sys.stdout.write(apply_edits(argv[2], apply_tokens(text), strict=False))
            return 0
        if argv:
            print(__doc__ or "usage: rebrand.py [--check | --filter FILE RELPATH]", file=sys.stderr)
            return 2
        apply(REPO)
    except Drift as e:
        print("rebrand.py:", e, file=sys.stderr)
        return 1
    print(f"Rebranded {REPO} to {NEW_NAME} ({NEW_ID})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
