# TEST: verifying the APT system-updates patch and the Warsi rebrand

How to test the patch **after it has been applied** (PLAN.md §5.1–§5.3).
Read [PLAN.md](PLAN.md) first. Tests T1–T6 are automated and read-only;
T7 (opt-in) sends a real desktop notification. T8 is a visual comparison
(English and Indonesian), T9 lists manual tests (two of them really upgrade
the machine), and the **last step, T10, runs the real Bazaar app**.
On a tree rebranded to Warsi (PLAN.md §10), R1 checks the rebrand, and
every test runs Warsi instead: the scripts read the app ID (`APP_ID`),
program (`APP_BIN`) and text domain from `meson.build` (`patches/harness/brand.sh`).
In the commands below, read `io.github.kolunmi.Bazaar` as
`id.blankonlinux.warsi`, `build/src/bazaar` as `build/src/warsi`,
`bazaar.mo` as `warsi.mo`, `TEXTDOMAIN=bazaar` as `TEXTDOMAIN=warsi`, and
`bazaar-daemon` as `warsi-daemon` there.

The commands below run from the repository root with this directory at
`ai-patches/`. To run the scripts from a copy elsewhere (for example
`../bazaar-ai-patches`), set `BZ_REPO` to the Bazaar tree:
`BZ_REPO=$PWD ../bazaar-ai-patches/patches/harness/run-tests.sh`.

| Test | What it proves | How | Changes the system? |
|---|---|---|---|
| T1 | applied files match `patches/src/` and `patches/data/`, every hook is in place, translations merged and valid, Flatpak manifest valid | `run-tests.sh` | no |
| T2 | the patch's C code compiles standalone with `-Werror` | `run-tests.sh` | no |
| T3 | the full Bazaar build succeeds, no warnings from the patch's files | `run-tests.sh` | no |
| T4 | the card's numbers (upgrades, security, new, removals) match PackageKit **and** apt | `run-tests.sh` | no |
| T5 | the card renders in the standalone harness | `run-tests.sh` | no |
| T6 | the card renders on the Library page of the **real app** | `run-tests.sh` | no |
| T7 | the update worker counts Flatpak and APT updates and sends the notification | `run-tests.sh --notify` | posts a notification; sets `last-update-notification` |
| R1 | (Warsi only) the rebrand is complete: nothing left for `rebrand.py`, no Bazaar ID or file name installed, every Warsi file installed, metadata valid, Indonesian About note, and app search (Praya, GNOME Shell) finds Warsi by "warsi", "warung aplikasi", "store", "software", "gnome software", "bazaar" in English and Indonesian | `run-tests.sh` | no |
| T8 | screenshots look right, in English and Indonesian | by eye | no |
| T9 | buttons, error paths and the notification click behave | by hand | ⚠️ two rows upgrade the machine |
| T10 | run Bazaar and use it | by hand | no (unless you click update) |

## 0. Prerequisites

A Debian-based machine (Debian, Ubuntu, BlankOn, …) with a graphical session
(Wayland or X11) and the PackageKit daemon. Build dependencies:

```bash
sudo apt install packagekit meson ninja-build valac blueprint-compiler libxml2-utils python3-babel libgtk-4-dev libadwaita-1-dev libdex-dev libflatpak-dev libappstream-dev libxmlb-dev libyaml-dev libsoup-3.0-dev libjson-glib-dev libglycin-2-dev libglycin-gtk4-2-dev libwebkitgtk-6.0-dev libsecret-1-dev libproxy-dev libmalcontent-0-dev libgtksourceview-5-dev libmd4c-dev libsystemd-dev libdbus-1-dev
```

Test-only dependencies (for T4): Python bindings for PackageKit.

```bash
sudo apt install python3-gi gir1.2-packagekitglib-1.0
```

- `python3-babel` is an **upstream** build requirement that is easy to miss;
  without it the build fails at `src/countries.gvariant`.
- `pkcon` (`packagekit-tools`) is **not** needed. Some distributions,
  BlankOn included, do not ship it; T4 asks PackageKit through its Python
  bindings instead.
- T1 needs `gettext` (`msgattrib`, `msgcat`, `msgfmt`) for the translation check.
- Quit any running Bazaar before T6/T10 (`flatpak kill io.github.kolunmi.Bazaar`):
  it owns the application ID and would receive the window instead of the build.

## 1. Run the automated tests (T1–T7, D1)

From the repository root:

```bash
ai-patches/patches/harness/run-tests.sh --notify
```

It takes about 3 minutes (most of it T6 waiting for Bazaar's startup refresh)
and ends with a summary like:

```text
=== Summary
PASS  T1  static checks (sync, markers, translations, manifest)
PASS  T2  standalone harness builds with -Werror
PASS  T3  full warsi build, no patch warnings
PASS  R1  Warsi rebrand complete and installs cleanly
PASS  T4  card counts match PackageKit and apt
PASS  T5  harness screenshots
PASS  D1  .deb installer reads packages (simulated, installs nothing)
PASS  T6  real warsi app: Library page (and About) screenshots
PASS  T7  update worker sends the updates-available notification
Screenshots: <repo>/build/apt-tests
```

(On an un-rebranded tree, the names say `bazaar` and there is no R1.)
The exit status is 0 only if everything passed. `--no-app` skips T6, for a
machine without a full build; without `--notify`, T7 is skipped. Each test can also be run on its own, as
described below.

### T1: static checks

Compares every file in `patches/src/` and `patches/data/` with its applied
copy, checks that every hooked file carries the
`DOWNSTREAM PATCH (APT system updates)` marker, re-runs
`patches/po/merge-translations.py` and fails if it still had something to
add (hook H12 not applied), checks `po/id.po` with `msgfmt -c`, and
validates the Flatpak manifest (the JSON file cannot carry the marker, so
the script checks for the PackageKit permission instead). By hand:

```bash
for f in ai-patches/patches/src/* ai-patches/patches/data/*; do cmp "$f" "$(basename "$(dirname "$f")")/$(basename "$f")" || echo "OUT OF SYNC: $f"; done
```

```bash
git grep --untracked -c "DOWNSTREAM PATCH (APT system updates)" -- src data po
```

```bash
python3 ai-patches/patches/po/merge-translations.py && msgfmt -c --statistics -o /dev/null po/id.po
```

The first line must say `id: nothing to add`.

```bash
python3 -m json.tool build-aux/flatpak/io.github.kolunmi.Bazaar.json >/dev/null && echo valid
```

On a Warsi tree, T1 compares the masters (which stay Bazaar-branded) as
the rebrand turns them out, checks the `DOWNSTREAM PATCH (Warsi rebrand)`
markers, and also re-merges `patches/rebrand/po/id-rebrand.po`. By hand:

```bash
for f in ai-patches/patches/src/* ai-patches/patches/data/* ai-patches/patches/rebrand/src/*; do r="$(basename "$(dirname "$f")")/$(basename "$f")"; python3 ai-patches/patches/rebrand/rebrand.py --filter "$f" "$r" | cmp -s - "$r" || echo "OUT OF SYNC: $f"; done
```

### R1: rebrand check (Warsi only)

```bash
python3 ai-patches/patches/rebrand/rebrand.py --check && echo "nothing left to rebrand"
```

```bash
rm -rf build/apt-tests/install && DESTDIR=$PWD/build/apt-tests/install meson install -C build --no-rebuild >/dev/null && grep -rlE "io\.github\.kolunmi\.Bazaar|io/github/kolunmi/Bazaar" build/apt-tests/install; find build/apt-tests/install -iname "*bazaar*"
```

The second command must print nothing. App launchers must find Warsi by
the words people use. `patches/harness/app-search.py` runs GLib's
desktop-file search, as BlankOn's Praya and GNOME Shell do, in English and
Indonesian:

```bash
for l in C id; do env XDG_DATA_HOME=$PWD/build/apt-tests/app-search XDG_DATA_DIRS=/nonexistent LANGUAGE=$l python3 ai-patches/patches/harness/app-search.py id.blankonlinux.warsi.desktop warsi "warung aplikasi" store software "gnome software" bazaar; done
```

(with the installed `id.blankonlinux.warsi.desktop` copied into
`build/apt-tests/app-search/applications/` first; `run-tests.sh` does that).
Every query must say `found`. `run-tests.sh` also checks that the
Warsi programs, autostart file, desktop file, AppStream file, schema, icons,
D-Bus service and `id/LC_MESSAGES/warsi.mo` are installed, and runs
`desktop-file-validate` and `appstreamcli validate --no-net`.

### T2: standalone harness build

```bash
ai-patches/patches/harness/build.sh
```

Compiles `bz-apt-updates.c` and `bz-apt-updates-card.c` with
`-Wall -Wextra -Werror` plus the card's Blueprint, into
`build/apt-harness/harness`: a window that shows only the card. Needs only
`gcc`, `pkg-config`, `blueprint-compiler`, `libgtk-4-dev`, `libadwaita-1-dev`.
It can also run **before** applying the patch, against the master copies:
`BZ_APT_SRC=ai-patches/patches/src ai-patches/patches/harness/build.sh`.

### T3: full Bazaar build

```bash
meson setup build && ninja -C build
```

```bash
touch src/bz-apt-updates.c src/bz-apt-updates-card.c src/bz-library-page.c && ninja -C build 2>&1 | grep -iE "warning|error"
```

The second command must print nothing.

### T4: count check

```bash
python3 -I ai-patches/patches/harness/check-counts.py
```

Starts the harness, reads the line the card's model logs after every
`GetUpdates`:

```text
BAZAAR::APT-DEBUG: GetUpdates: 1393 upgrades (0 security), 61 new, 3 removals
```

and compares it with PackageKit (via `gir1.2-packagekitglib-1.0`) and with
`apt list --upgradable`. Expected:

```text
card:        1393 upgrades (0 security), 61 new, 3 removals
PackageKit:  1393 upgrades (0 security), 61 new, 3 removals  raw={'normal': 1393, 'install': 61, 'remove': 3}
PASS  card upgrades == PackageKit upgrades
PASS  card security == PackageKit security
PASS  card new == PackageKit new
PASS  card removals == PackageKit removals
apt:         1393 upgradable
PASS  card upgrades == apt list --upgradable
note: the upgrade would remove: libperl5.42, perl-modules-5.42, python3-pkg-resources
```

Why the categories matter: PackageKit's `GetUpdates` returns not only
upgrades but also packages a full upgrade **newly installs** (info
`install`, e.g. a renamed library) and **removes** (info `remove`/`obsolete`).
apt's `--upgradable` lists upgrades only. Before 2026-11-14 the card counted
all of them as "updates" (1457 instead of 1393). Now the title counts upgrades
only, so it must equal apt's number.

- If **card ≠ PackageKit**: a bug in the patch (the classification in
  `bz-apt-updates.c`, or PackageKit enum values changed: API.md §2.2).
- If **card = PackageKit ≠ apt**: the script prints the packages that differ.
  A small difference can be legitimate (PackageKit hides blocked/kept-back
  packages; apt may have newer lists than PackageKit's cache). Refresh both
  (`sudo apt update`, then ⟳ in the card) and run again.
- If the GetUpdates line is missing: PackageKit is not running or slow; raise
  the wait with `HARNESS_DELAY=30`.

### T5: harness screenshots

The script saves `build/apt-tests/harness.png` (collapsed) and
`build/apt-tests/harness-expanded.png` (package list open). By hand:

```bash
HARNESS_ICONS=$PWD/src/icons G_MESSAGES_DEBUG=BAZAAR::APT build/apt-harness/harness
```

Harness environment variables: `HARNESS_SHOT=out.png` (save, then quit),
`HARNESS_DELAY=20` (seconds before the shot), `HARNESS_EXPAND=1` (open the
package list), `HARNESS_HEIGHT=640`, `HARNESS_ACTION=apt-updates.check`
(trigger a refresh after 4 s; this downloads package lists, it does not
change installed packages).

### T6: real app screenshots

```bash
ai-patches/patches/harness/app-shot.sh build/apt-tests/app-library.png 40
```

```bash
APP_SHOT_EXPAND=1 ai-patches/patches/harness/app-shot.sh build/apt-tests/app-library-expanded.png 40
```

On a Warsi tree, a third screenshot shows the About dialog:

```bash
APP_SHOT_ABOUT=1 ai-patches/patches/harness/app-shot.sh build/apt-tests/app-about.png 20
```

Starts `build/src/bazaar` (`build/src/warsi`) with a small `LD_PRELOAD` shim
(`patches/harness/app-shot.c`) that opens the Library page and saves the
window to PNG after the given number of seconds, then exits. It clicks
nothing. The script also compiles the GSettings schema into `build/schemas`
and puts `build/src` on `PATH` (Bazaar re-executes itself by name for its
refresh worker). See [Troubleshooting](#troubleshooting) for what did not
work.

After D1 has run, T6 also screenshots the **.deb install dialog** for D1's
generated app package (`build/apt-tests/app-deb-install.png`). Any file can
be opened the same way:

```bash
APP_SHOT_OPEN=/path/to/package.deb ai-patches/patches/harness/app-shot.sh build/apt-tests/app-deb.png 15
```

Expected: the app's name and icon from the package, an **Install** button,
the "Only install packages you trust" row, Details. If another Warsi is
running in the session (an installed one), run the script under
`dbus-run-session -- …` so the build gets its own session bus.

### D1: .deb installer

```bash
ai-patches/patches/harness/deb-check.sh
```

Compiles `src/bz-deb-installer.c` with a small driver (`deb-check.c`, no
GTK) and runs it on packages it generates with `dpkg-deb` in
`build/apt-tests/deb/`:

| Case | Expected |
|---|---|
| new package with a launcher and SVG icon | `state=ready action=install`, app name and icon from the package |
| depends on a package that does not exist | `state=blocked`, "Other packages it needs are missing …" |
| `base-files` at the installed version | `state=ready action=none` (nothing to install) |
| `base-files` version 99999 | `state=ready action=upgrade` |
| a text file named `.deb` | `state=failed`, "The file is damaged …" |
| a file that does not exist | `state=failed` |

PackageKit reads the files and **simulates** installing them; nothing is
installed. run-tests.sh also checks that the desktop file lists
`application/vnd.debian.binary-package` and that
`data/bazaar-deb-mimeapps.list` names the app.

### D2: default handler (installed package)

```bash
gio mime application/vnd.debian.binary-package
```

Expected first line: the default application is `id.blankonlinux.warsi.desktop`
(`io.github.kolunmi.Bazaar.desktop` un-rebranded). If it still names
`gnome-software-local-file-packagekit.desktop`, check that
`/etc/xdg/gnome-mimeapps.list` is installed, that `XDG_CURRENT_DESKTOP`
contains `GNOME`, and that `~/.config/mimeapps.list` does not set another
default. Without installing, point `XDG_CONFIG_DIRS` and `XDG_DATA_DIRS` at
the build's files (DIFF.md H15).

### T7: notification (opt-in)

```bash
mkdir -p build/schemas && cp data/io.github.kolunmi.Bazaar.gschema.xml build/schemas/ && glib-compile-schemas build/schemas
```

```bash
GSETTINGS_SCHEMA_DIR=$PWD/build/schemas BAZAAR_NOTIFY_NOW=1 G_MESSAGES_DEBUG="BAZAAR::NOTIFY BAZAAR::APT" build/src/bazaar --run-update-worker
```

This is exactly what `bazaar-daemon` runs every hour (hook H7);
`BAZAAR_NOTIFY_NOW=1` skips the once-a-day check. Expected, within a few
seconds (longer when PackageKit refreshes its lists first):

```text
BAZAAR::APT-DEBUG: Using PackageKit backend 'Debian APT'
BAZAAR::APT-DEBUG: GetUpdates: 1393 upgrades (0 security), 61 new, 3 removals
BAZAAR::NOTIFY-DEBUG: Pending updates: 4 apps, 1393 system (0 security)
BAZAAR::NOTIFY-DEBUG: Sent the update notification through org.gtk.Notifications
```

and a notification on the desktop: **Updates Available** — "1393 system
updates from Debian APT are available. 4 app updates are available." with
a **View Updates** button. "N apps" must equal the Flatpak count on the
Library page ("N Available Updates"); "N system" must equal T4's upgrades.
The run also records the time in `last-update-notification`, so the real
daily notification may skip that day. With `auto-update` on, the line says
`0 apps` (upstream installs Flatpak updates itself).

On an **uninstalled** Warsi build the last line says `through the portal`
instead: GNOME Shell accepts `org.gtk.Notifications` only for an app ID it
has a desktop file for, and nothing installs `id.blankonlinux.warsi.desktop`
until Warsi is packaged. (Bazaar's ID worked uninstalled when Flathub's
Bazaar was installed.) The notification still appears, attributed to no app.

G_MESSAGES_DEBUG takes **space**-separated domains; a comma-separated list
silently shows nothing.

## 2. T8: compare the screenshots

Open `build/apt-tests/` and compare with
[`patches/harness/expected-library-page.png`](patches/harness/expected-library-page.png):

- **"System Updates"** is the first section of the Library page, directly
  above upstream's Flatpak **"Pending Updates"**.
- Title: **"N System Package Updates"**, where N is T4's upgrade count
  (not upgrades + new + removals).
- Subtitle: "Includes S security updates" when there are any, then
  "· K new packages" and "· R packages will be removed" when non-zero.
  With none of those it says "Provided by Debian APT".
- Expanded: packages to be **removed come first**, each with "Will be removed"
  and a red trash icon; then security updates (warning icon), other updates
  (new version as subtitle), and new packages ("New package, VERSION").
  At most 100 rows, then "And K more packages".
- Buttons: ⟳ and **Restart & Update** with a ▾ menu.

**Indonesian.** The dev build is not installed, so it cannot find its
translations; the harness can load them:

```bash
mkdir -p build/apt-tests/locale/id/LC_MESSAGES && msgfmt -o build/apt-tests/locale/id/LC_MESSAGES/bazaar.mo po/id.po
```

```bash
LANGUAGE=id HARNESS_LOCALEDIR=$PWD/build/apt-tests/locale HARNESS_ICONS=$PWD/src/icons HARNESS_EXPAND=1 HARNESS_SHOT=build/apt-tests/harness-id.png HARNESS_DELAY=12 build/apt-harness/harness
```

Expected: "Pembaruan Sistem", "1393 Pembaruan Paket Sistem",
"61 paket baru · 3 paket akan dihapus", "Akan dihapus" on removals,
"Mulai Ulang & Perbarui". No English text from the patch may remain (the
package names and versions are data, not text). The notification strings
can be checked with gettext's own tools:

```bash
LANGUAGE=id TEXTDOMAINDIR=$PWD/build/apt-tests/locale TEXTDOMAIN=bazaar ngettext '%u system update from %s is available.' '%u system updates from %s are available.' 2
```

Expected: `Tersedia %u pembaruan sistem dari %s.`

**Warsi** (`app-about.png`, and the app in T10):

- The About dialog shows the Warsi icon (a striped awning over a "W"),
  **Warsi**, then **"Originally Bazaar, patched by BlankOn Linux"**
  (Indonesian: "Dibangun dari Bazaar, dimodifikasi oleh BlankOn Linux"), then Bazaar's section unchanged: "The Bazaar
  Contributors", version, Website, Report an Issue, Credits, Legal,
  Acknowledgements.
- The window title and main menu say Warsi ("About Warsi", "Quit Warsi");
  "Donate to Bazaar" keeps Bazaar's name.
- The Library page is the same as above.

## 3. T9: manual functional tests

Run Bazaar as in T10, open **Library** (Ctrl+D).

| Scenario | How | Expected |
|---|---|---|
| Updates pending | normal start | counts as in T4/T8 |
| Up to date | after updating | "System Is Up to Date", only ⟳ shown |
| Refresh | click ⟳ | polkit prompt may appear; progress row with live status ("Downloading repository information", …), then the updated list |
| Cancel | Cancel while refreshing | stops, no error row |
| Auth refused | click ⟳, dismiss the polkit dialog | error row with PackageKit's message; ✕ dismisses it |
| ⚠️ Offline update | Restart & Update | polkit prompt → download progress → "Updates Ready to Install" → Restart Now → installed during boot |
| Un-schedule | Cancel Update on the ready row | row disappears; offline update no longer pending (`ls /system-update` fails) |
| ⚠️ Live update | ▾ → Install Now Without Restarting → confirm | polkit prompt → progress → list refreshes; maybe "Restart Required" |
| No PackageKit | `sudo systemctl mask --now packagekit` (unmask after: `sudo systemctl unmask packagekit`) | card hidden, Bazaar otherwise unchanged |
| No permission | Flathub build without overrides (PLAN.md §7) | card hidden |
| Zero Flatpaks | no Flatpak apps installed | card shown on the "No Apps Found" page |
| Notification action | Bazaar running: `gapplication action io.github.kolunmi.Bazaar open-library` | the window comes up on the Library page, also while it is still loading |
| Notification click | installed package: click the notification (T7) or **View Updates** | Bazaar starts (or comes to front) on the Library page. With Flathub's Bazaar also installed, it may open that one instead (PLAN.md §9) |
| Notifications off | `gsettings set io.github.kolunmi.Bazaar update-notifications false`, run T7's worker command | `Update notifications are disabled`, no notification; log in again: no `bazaar-daemon` started by `/etc/xdg/autostart/bazaar.desktop`. Turn it back on afterwards |
| Daily limit | run T7's command twice **without** `BAZAAR_NOTIFY_NOW` | second run logs `Skipping: notified N seconds ago` |
| Daemon stays | installed package, log in, wait 10 s, `pgrep -a bazaar-daemon` | daemon running (`journalctl --user` / its log says "update notifications are on so staying alive") |
| Warsi next to Bazaar | Warsi installed, and Flathub's Bazaar running | both run at once as separate apps (own windows, own GNOME notification settings); a Warsi notification click opens Warsi |
| Warsi in GNOME | installed Warsi: search "Warsi" in Activities; Settings → Apps | the Warsi icon and name; GNOME Shell search shows Flatpak results from Warsi |
| D3 ⚠️ Install a .deb | double-click a downloaded `.deb` in Files (e.g. D1's `build/apt-tests/deb/bz-deb-check-app_1.0_all.deb`), **Install** | dialog as in T6; polkit prompt; progress; "Installed" and **Open**; `dpkg -s` shows the package. Remove it afterwards with `sudo apt remove bz-deb-check-app` |
| .deb: auth refused | **Install**, dismiss the polkit dialog | back to **Install**, no error |
| .deb: cancel | **Install**, then **Cancel** while it downloads dependencies | back to **Install**, no error; the dialog could not be closed meanwhile |
| .deb: already installed | open the same file again after D3 | "This version is installed" and **Open** |
| .deb: network share | open a `.deb` on an SMB/SFTP share mounted in Files | works (copied to `~/.cache/warsi/deb-staging/` first) |

⚠️ **Offline update**, **Live update** and **D3** really change the machine, including
the removals the card lists. Only run them on a machine where that is wanted,
and when the person who owns it agrees.

## 4. T10: run Bazaar (final step)

After T1–T9, run the patched Bazaar from the build directory and use it
normally:

```bash
mkdir -p build/schemas && cp data/io.github.kolunmi.Bazaar.gschema.xml build/schemas/ && glib-compile-schemas build/schemas
```

```bash
PATH=$PWD/build/src:$PATH GSETTINGS_SCHEMA_DIR=$PWD/build/schemas G_MESSAGES_DEBUG=BAZAAR::APT ./build/src/bazaar
```

On a Warsi tree:

```bash
mkdir -p build/schemas && cp data/id.blankonlinux.warsi.gschema.xml build/schemas/ && glib-compile-schemas build/schemas
```

```bash
PATH=$PWD/build/src:$PATH GSETTINGS_SCHEMA_DIR=$PWD/build/schemas G_MESSAGES_DEBUG=BAZAAR::APT ./build/src/warsi
```

Expected terminal output while it starts:

```text
BAZAAR::APT-DEBUG: Using PackageKit backend 'Debian APT'
BAZAAR::APT-DEBUG: Package lists were refreshed N seconds ago
BAZAAR::APT-DEBUG: GetUpdates: U upgrades (S security), K new, R removals
```

Press Ctrl+D for the Library page. Browsing, Flatpak installs and Flatpak
updates must work exactly as upstream. Close the window to quit.

The patched app chooses GTK's renderer (hook H13): GL on a real graphics
card, Cairo where GL runs in software only (a VM without 3D acceleration).
The Search page must show its text and category tiles, and open without
freezing. To check whether the distribution's GTK
still needs H13, run once with `GSK_RENDERER=vulkan` in front of the command:
missing text and blank tiles on Search mean it does. `GSK_DEBUG=renderer`
prints which renderer GTK chose (`Using renderer 'GskGLRenderer'` by
default).

### Search page freeze (hook H13)

Measures how long the window freezes when the Search page opens, and with
`--vm` emulates a virtual machine without 3D acceleration (software GL,
Mesa's shader cache off, 2 CPU cores), where the freeze was seen:

```bash
ai-patches/patches/harness/stall-probe.sh --vm
```

It starts the app with an `LD_PRELOAD` probe (`stall-probe.c`), switches to
Search after 30 s, types "a", and prints the renderer, memory and
`longest main-loop stall after opening Search`. Exit 0 when that is at most
1 s. Expected (2026-11-15): `Using GTK's cairo renderer`, about 0.13 s.
For comparison, `GSK_RENDERER=gl ai-patches/patches/harness/stall-probe.sh --vm`
shows the freeze H13 avoids (about 5 s, exit 1). While the main loop is
blocked, the probe prints the main thread's stack to
`build/apt-harness/stall-probe.log` (resolve offsets with `addr2line`); that
is how the shader compiler was found.

## 5. Record the result

Add a row to PLAN.md §11 "Attempt log" with the summary of
`run-tests.sh`, T4's and T7's numbers, and which T9 rows were run.

## Troubleshooting

Lessons from earlier runs:

| Symptom | Cause | Fix |
|---|---|---|
| T6 times out, no screenshot | Bazaar shows its window from a **child process** it spawns; a shim that removes `LD_PRELOAD` never reaches it | `app-shot.c` keeps `LD_PRELOAD`; processes without a window just stop their timer |
| shim's timer never fires | `g_timeout_add` called from the shim's constructor runs before GLib/the app set up the main context | add the timeout from a helper thread after 2 s (as `app-shot.c` does) |
| T6 `app-about.png`: "could not render the window (… nothing drawn yet)" | a widget paintable created only at screenshot time came out empty while the About dialog was open | `app-shot.c` creates the paintable when the window first shows and keeps it; it retries for 10 s |
| T6 `app-about.png` not saved, `gsk_renderer_render_texture` CRITICAL | GTK 4.23's software renderer cannot draw the SVG app icon | `app-shot.sh` gives the uninstalled build a PNG copy of the icon (`rsvg-convert`) |
| T6 screenshot is blank (background only) | rendering Bazaar's window offscreen with its own GPU renderer drew only the background, although the render tree had the content | `app-shot.c` renders with `gsk_cairo_renderer_new ()` |
| GTK Broadway backend crashes | `broadway-output.c: assertion failed: (!reused_node->consumed)` on Bazaar's Explore page | do not use Broadway |
| "A Bazaar instance is already running" | the Flatpak Bazaar owns the app ID | `flatpak kill io.github.kolunmi.Bazaar` |
| build fails at `countries.gvariant` | upstream needs `python3-babel` | install it |
| Search page shows missing text, blank category tiles (upstream commit too) | bug in GTK **4.23.4**'s default **Vulkan** renderer (seen 2026-11-14 on AMD Radeon Cezanne, Wayland); the same render tree draws correctly with the GL and Cairo renderers, so it is not Bazaar or this patch | fixed by hook H13 (the app sets `GSK_RENDERER=gl`, or `cairo` on software GL, unless it is set). Still broken: check the renderer with `GSK_DEBUG=renderer`; an exported `GSK_RENDERER=vulkan` overrides H13 |
| Search tab heavy, takes 10–20 s, "not responding", sometimes crashes (VirtualBox VM) | GL only in software (llvmpipe): GTK's GPU renderers compile shaders on the main thread the first time each is needed (stacks in Mesa/LLVM under `gsk_renderer_render`) | fixed by hook H13 (Cairo renderer on software GL). Check: `G_MESSAGES_DEBUG=BAZAAR::MAIN` prints `GL renderer: llvmpipe …` and `Using GTK's cairo renderer`; measure with `stall-probe.sh --vm` |
| T7 prints nothing at all | `G_MESSAGES_DEBUG` with commas (`BAZAAR::NOTIFY,BAZAAR::APT`) matches no domain | separate domains with spaces, or use `G_MESSAGES_DEBUG=all` |
| Notification shows, click opens Flathub's Bazaar | both builds installed; the D-Bus name `io.github.kolunmi.Bazaar` activates the Flatpak one | test the action with `gapplication action` against the running dev build (T9), or remove the Flatpak while testing |
| `msgfmt: duplicate message definition` after merging | an obsolete `#~` entry with the same msgid was left in `po/<lang>.po` | the merge script removes it; do not append fragment entries by hand |
| `APP_SHOT_DEBUG=1` | logs every shim tick: pid, window type, mapped | for debugging T6 |
