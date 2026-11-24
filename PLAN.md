# PLAN: APT system updates for Bazaar, rebranded as Warsi

This is the **main entry point** for the downstream patch. It explains what
the patch achieves, how it is built, and how to (re-)apply it to any version
of the Bazaar codebase. The patch has two parts, applied in this order:

1. **APT system updates** (sections 1–9): the System Updates card and the
   "updates available" notifications, as hooks H1–H13; and the **.deb
   file installer** ([§12](#12-deb-file-installer)): opening a downloaded
   `.deb` file shows an install dialog, hooks H14–H15; and the **GNOME
   Extensions tab** ([§13](#13-gnome-extensions-tab)): browse, search and
   manage GNOME Shell extensions from extensions.gnome.org, hooks H16–H17.
2. **Rebrand to Warsi** ([§10](#10-rebrand-warsi)): turns the patched Bazaar
   into **Warsi** (`id.blankonlinux.warsi`), BlankOn Linux's app store, with
   one script, `patches/rebrand/rebrand.py`.
 It is written for humans **and** AI agents, on any
machine, at any later time.

| Document | Read it for |
|---|---|
| `PLAN.md` (this file) | goal, design, step-by-step procedure, verification |
| [`API.md`](API.md) | every external API the patch relies on, the base commit, and how to check each API for breaking changes upstream |
| [`DIFF.md`](DIFF.md) | every change to **existing** upstream files: what, why, and how it connects to the new files |
| [`TEST.md`](TEST.md) | how to test the patch once applied: automated tests, screenshots, manual tests, running the app |
| `patches/` | the **master copies** of every entirely new file written by the patch; `patches/rebrand/` holds the rebrand script, its code, icons and translations |

> **If you are an AI agent asked to apply or re-apply this patch:** read all
> four documents fully before changing anything, then follow
> [§5 Procedure](#5-procedure). Do not redesign the patch; the
> [§3 Scope](#3-scope) and [§8 Design decisions](#8-design-decisions) were
> chosen on purpose by the user.

---

## 1. Goal

Bazaar is a Flatpak-only app store. On Debian-based distributions (Debian,
Ubuntu, BlankOn, …) the operating system itself is made of **APT/deb
packages** that Bazaar ignores, so users need a second tool to keep the
system up to date.

**The patch adds system update capability for APT to Bazaar:** Bazaar shows
pending system package updates and lets the user install them, next to its
existing Flatpak updates, and **notifies on the desktop** when app (Flatpak)
or system (APT) updates are available, so Bazaar is the one place for both.

## 2. What the user sees

A **"System Updates"** card at the top of the **Library** page, directly
above upstream's Flatpak "Pending Updates" section
(reference screenshot: [`patches/harness/expected-library-page.png`](patches/harness/expected-library-page.png)):

- **"N System Package Updates"**, where N counts **upgrades only** (equal to
  `apt list --upgradable`). Subtitle: "Includes M security updates", then
  "· K new packages" and "· R packages will be removed" when the upgrade
  installs or removes packages; with none of those, "Provided by Debian
  APT" (PackageKit calls its apt backend just "APT"; the patch names it
  "Debian APT").
  It is an expander listing up to 100 packages, **removals first** ("Will be
  removed", red trash icon), then security updates (warning icon), other
  updates (new version), new packages ("New package, VERSION"); summary as
  tooltip; then "And K more packages".
- **⟳ Check for Updates**: downloads fresh package lists (`apt update`).
- **Restart & Update** (primary button): downloads all updates now and
  installs them during the next boot (PackageKit *offline update*). After
  the download, a row says "Updates Ready to Install" with **Restart Now**
  and **Cancel Update**.
- **▾ menu → Install Now Without Restarting**: live install, after a
  confirmation dialog that warns about running apps.
- While working: a progress row with live status text ("Downloading
  packages", "Waiting for authentication", …), a progress bar and **Cancel**.
- Errors: an error row with PackageKit's message and a dismiss button.
- After a live update that needs it: "Restart Required" with **Restart Now**.
- When there is nothing to do: "System Is Up to Date".
- The card **also appears on the "No Apps Found" page**, so it works on a
  machine with zero Flatpak apps installed.
- **Without PackageKit** (or without D-Bus permission in a sandbox) the card
  is hidden and Bazaar behaves exactly like upstream.

Package lists are refreshed automatically when older than 24 hours, and the
list refreshes itself when PackageKit reports `UpdatesChanged`.

**Desktop notification** (at most once a day, only when something is
pending), from Bazaar's background service even when Bazaar's window is
closed:

> **Updates Available** — 1393 system updates from Debian APT are available.
> 4 app updates are available. [View Updates]

- Title "Security Updates Available" (and high priority) when APT reports
  security updates; the body then adds "This includes N security updates."
- Clicking the notification or **View Updates** opens Bazaar on the
  Library page.
- With upstream's **Automatic Updates** preference on, Flatpak updates are
  installed by upstream (which sends its own "N Apps Updated"
  notification), so this notification covers APT only.
- Controlled by the GSettings key `io.github.kolunmi.Bazaar
  update-notifications` (default on), which also starts Bazaar's background
  service at login. There is no switch in Preferences;
  `gsettings set io.github.kolunmi.Bazaar update-notifications false`
  turns it off, as does GNOME Settings → Notifications → Bazaar.

**Indonesian:** every string the patch adds is translated
(`patches/po/id-apt-updates.po`, merged into `po/id.po`).

## 3. Scope

**In scope:** detect APT through PackageKit, list updates, refresh lists,
apply all updates (offline by default, live optionally), progress, cancel,
errors, reboot; daily "updates available" notifications for APT and
Flatpak; installing a local `.deb` file the user opens (§12, added on
2026-11-17 at the user's request, replacing GNOME Software's installer for
`.deb` files); a GNOME Extensions tab (§13, added on 2026-11-24 at the
user's request) that browses extensions.gnome.org and installs, enables,
disables, configures and removes GNOME Shell extensions through GNOME
Shell; Indonesian translations of the patch's strings.

**Out of scope (do NOT add when re-applying):**

- Browsing, searching or removing APT packages, or installing them from the
  repositories by name; users do that in a terminal. (Opening a local
  `.deb` file is in scope, §12.)
- Showing APT packages as Bazaar app entries or app pages.
- Implementing Bazaar's `BzBackend` interface, or touching its transaction
  manager. (`bazaar-daemon` and the update worker get small hooks for the
  notifications only.)
- Installing APT updates automatically in the background.
- Per-package update selection, changelogs, distribution upgrades.

## 4. How it works

```
 Library page  (upstream; hooks described in DIFF.md)
   ├─ normal content page:  $BzAptUpdatesCard {}   ← first child, above "Pending Updates"
   └─ "No Apps Found" page: $BzAptUpdatesCard {}   ← so it works with 0 Flatpaks
                │
                ▼
 BzAptUpdatesCard   patches/src/bz-apt-updates-card.{c,h,blp}
   AdwBin. Pure view. visible == PackageKit available.
   Actions group "apt-updates": install-now, check.
                │   bz_apt_updates_get_default ()   (process-wide singleton)
                ▼
 BzAptUpdates       patches/src/bz-apt-updates.{c,h}
   GObject state model + PackageKit client over GDBus.
   No GTK, no Bazaar dependencies. One transaction at a time, FIFO queue.
                │   system D-Bus
                ▼
 org.freedesktop.PackageKit  (packagekitd as root; apt backend; polkit auth)
```

Notifications:

```
 bazaar-daemon (upstream; hook H8 keeps it alive while update-notifications is on)
   │  started at login by /etc/xdg/autostart/bazaar.desktop (H11; condition:
   │  update-notifications), or by upstream's per-user autostart
   │  every hour: spawns  bazaar --run-update-worker
   ▼
 run_update_worker ()  src/update-worker.c (upstream; hook H7, one call)
   │  bz_update_notifier_run (settings, auto_update)
   ▼
 bz-update-notifier.{c,h}   patches/src/
   ├─ at most once per 23 h (key last-update-notification)
   ├─ Flatpak: flatpak_installation_list_installed_refs_for_update ()
   │     (system + user, minus .Locale/.Debug/.Sources), unless auto-update
   │     is on or the network is metered
   ├─ APT: bz_apt_updates_get_default (), main context iterated until
   │     "loaded" (same PackageKit client as the card)
   └─ org.gtk.Notifications.AddNotification ("io.github.kolunmi.Bazaar", …)
        (GNOME Shell; falls back to the notification portal)
                │  click → D-Bus activation of io.github.kolunmi.Bazaar
                ▼
 app.open-library   (bz-update-notifier.c, added by hook H9 in bz-application.c)
   present/activate a window, repeat window.open-library until shown
```

**The card's files depend on nothing inside Bazaar** except two conventions:
`config.h` defines `GETTEXT_PACKAGE`, and the GResource prefix is
`/io/github/kolunmi/Bazaar/`. `bz-update-notifier.c` additionally uses two
Bazaar-internal helpers from `bz-flatpak-private.h` and `APPLICATION_ID`
(API.md §3.6). So upstream refactors rarely touch them; mainly the hooks in
DIFF.md need attention.

### 4.1 `BzAptUpdates` behaviour

1. **Startup** (first card constructed): `g_bus_get (SYSTEM)` →
   `Properties.GetAll ("org.freedesktop.PackageKit")`, which also D-Bus-activates
   the daemon. Failure, or backend `dummy`/empty, leaves `available = FALSE`.
2. Subscribe `UpdatesChanged` (queue a background re-query) and
   `PropertiesChanged` of the `Offline` interface (track `UpdateTriggered`).
   Read the initial offline state.
3. `GetTimeSinceAction (refresh-cache)`: over 24h queues a background
   `RefreshCache`, otherwise a background `GetUpdates`.
4. **Transaction runner:** `CreateTransaction` → subscribe that path's
   signals and `PropertiesChanged` → `SetHints` (`interactive`,
   `background`, `locale`) → call the method → collect `Package`/`Packages`
   (de-duplicated), `ErrorCode`, `RequireRestart` → `Finished` (or
   `Destroy` = failure).
5. **After each transaction:** `GetUpdates` replaces the list (minus
   `blocked` packages). PackageKit's `GetUpdates` also returns packages the
   full upgrade newly installs (info `install`) or removes (`remove`,
   `obsolete`); they are kept in the list (the update transaction acts on
   all of them) but counted separately: `n-upgrades`, `n-new`,
   `n-removals`, `n-security` (security counts upgrades only). Sort order:
   removals, security, other upgrades, new. A debug line
   `GetUpdates: U upgrades (S security), K new, R removals` is logged;
   TEST.md T4 parses it. `RefreshCache` chains `GetUpdates`.
   Live `UpdatePackages` may set `restart-required`, then re-queries.
   Download-only `UpdatePackages` calls `Offline.Trigger ("reboot")`.
6. **Background vs user-initiated:** automatic transactions do not set
   `busy` and never show errors; user clicks set `busy`, use
   `interactive=true`, and allow polkit interactive authorization.
7. **Reboot:** `org.gnome.SessionManager.Reboot` first (GNOME's own dialog,
   which knows about the prepared offline update), falling back to
   `org.freedesktop.login1.Manager.Reboot (true)`.

8. **`loaded`** turns TRUE once the first update list arrived, or once it
   is known there will be none (no system bus, no PackageKit, dummy
   backend, cancelled refresh). The update worker, which has no running
   main loop, iterates the main context until then (timeout 300 s).

Exact methods, signatures and enum values are in [API.md](API.md).

### 4.2 Notification behaviour

1. `bz_update_notifier_run ()` returns at once when `update-notifications`
   is off, or when the last notification is less than 23 hours old
   (`BAZAAR_NOTIFY_NOW=1` skips that check, for testing).
2. Counts Flatpak updates unless `flatpak_auto_updated` (upstream installs
   them itself) or the network is metered (listing downloads metadata).
3. Counts APT upgrades and security upgrades through `BzAptUpdates`. New
   and removed packages are not mentioned; the Library page shows them.
4. Logs `Pending updates: A apps, S system (X security)` (TEST.md T7 parses
   it). Nothing pending → no notification, timestamp unchanged.
5. Sends notification ID `bazaar-updates-available` (a new one replaces the
   old one) with title, body, priority, `default-action` and one button,
   both `app.open-library`. Only after a successful send is
   `last-update-notification` written.
6. **Why `org.gtk.Notifications` first:** it takes the application ID
   explicitly, so a short-lived worker process can post a notification
   that belongs to Bazaar and whose click D-Bus-activates Bazaar. The
   portal derives the app ID from the caller; for a non-sandboxed worker
   that is empty, so a click could not reach Bazaar. Inside Flatpak, or on
   desktops without GNOME Shell, the call fails and the portal is used, as
   upstream does.

## 5. Procedure

### 5.0 Inputs

- A Bazaar source tree (any version) = **the target**.
- This `ai-patches/` directory, copied into the target root (it is portable).
  The scripts in `patches/` assume they live at `<repo>/ai-patches/patches/`;
  to run them from a copy elsewhere, set `BZ_REPO=/path/to/bazaar`.

### 5.1 Check API stability first

Follow [API.md §2 "Stability check procedure"](API.md#2-stability-check-procedure).
It compares the target against the **base commit** the patch was written
for and lists exactly what to look at. If an API changed, adapt the code in
`patches/src/` (the master copies) **before** copying it.

### 5.2 Copy the new files

```bash
cp ai-patches/patches/src/* src/ && cp ai-patches/patches/data/* data/
```

`patches/src/` and `patches/data/` mirror the repository layout: every file
there goes to the same relative path in the target. `patches/harness/` is
**test-only** and is never copied. `patches/po/` is applied by a script
(hook H12), not copied.

### 5.3 Apply the hooks

Apply every hook in [DIFF.md](DIFF.md), in its order (H1 → H13). Each hook
has an *anchor* to find, the exact change, the *reason*, and what to do if
the anchor no longer exists. Try `git apply --3way` with the unified diff in
DIFF.md first (it covers H1–H11 and H13); fall back to manual edits per hook. Then
merge the translations (H12):

```bash
python3 ai-patches/patches/po/merge-translations.py
```

### 5.3a Rebrand to Warsi (step R, last)

```bash
python3 ai-patches/patches/rebrand/rebrand.py
```

Run it after H1–H12, on the patched Bazaar tree. It renames files, IDs,
programs and strings and adds the About note (details in
[§10](#10-rebrand-warsi), DIFF.md "R"). If upstream changed a string it
rewrites, it stops with a message naming that edit; update the edit in
`rebrand.py` (`EDITS`, and `RENAMED` for a translated string) and run it
again. Running it twice changes nothing; `--check` reports what it would
still change.

### 5.4 Build and verify

Follow [TEST.md](TEST.md) (summary in [§6 Verification](#6-verification)).
Minimum bar: `ai-patches/patches/harness/run-tests.sh --notify` passes
T1–T7 (and R1 on a rebranded tree) and the screenshots match the reference.

### 5.5 Record the attempt

Append a row to [§11 Attempt log](#11-attempt-log) (date, upstream commit,
result, notes). If you changed anything in `patches/src/`, keep the master
copies and the applied copies identical (on a rebranded tree, T1 compares
them through `rebrand.py --filter`, since the masters stay Bazaar-branded):

```bash
for f in ai-patches/patches/src/* ai-patches/patches/data/*; do cmp "$f" "$(basename "$(dirname "$f")")/$(basename "$f")" || echo "OUT OF SYNC: $f"; done
```

If you had to change a hook, update DIFF.md. If an API changed, update
API.md, including the base commit. If you add or change a translatable
string, add its Indonesian translation to `patches/po/id-apt-updates.po`.
**Keep the four documents true.**

## 6. Verification

Testing is described step by step in **[TEST.md](TEST.md)**. In short:

1. Install the build and test dependencies (TEST.md §0). `pkcon` is not
   needed; the count check uses PackageKit's Python bindings
   (`gir1.2-packagekitglib-1.0`).
2. Run the automated tests from the repository root (`BZ_REPO=/path` when
   running them from a copy of this directory elsewhere):

   ```bash
   ai-patches/patches/harness/run-tests.sh --notify
   ```

   | Test | Checks |
   |---|---|
   | T1 | applied copies identical to `patches/src/` and `patches/data/`, every hook marker present, translations merged and valid, manifest valid JSON |
   | T2 | harness (`patches/harness/build.sh`) compiles the patch with `-Werror` |
   | T3 | full Bazaar build, no warnings from the patch's files |
   | T4 | `check-counts.py`: card's upgrades/security/new/removals equal PackageKit's, and upgrades equal `apt list --upgradable` |
   | T5 | harness screenshots, collapsed and expanded |
   | T6 | `app-shot.sh`: screenshots of the **real app's** Library page |
   | T7 | (`--notify` only) the update worker counts Flatpak and APT updates and sends the "updates available" notification; this puts a real notification on the desktop |
   | D1 | `deb-check.sh`: the .deb installer's model reads generated packages (new app with launcher and icon, missing dependency, same version as installed, newer version, damaged file, missing file) and reports the right state; PackageKit only simulates; the desktop file lists the `.deb` MIME type and `bazaar-deb-mimeapps.list` makes it the default |
   | R1 | (rebranded tree only) `rebrand.py --check` has nothing left to do; a `meson install` into a temporary directory contains no Bazaar app ID or `bazaar` file name, installs every Warsi file, passes `desktop-file-validate` and `appstreamcli validate`, and has the Indonesian About note |

3. T8: compare the screenshots in `build/apt-tests/` with
   [`patches/harness/expected-library-page.png`](patches/harness/expected-library-page.png),
   and check the Indonesian card (TEST.md T8).
4. T9: manual tests of buttons, error paths and the notification click.
   "Restart & Update" and "Install Now" really upgrade the machine: only
   with the owner's consent.
5. T10, the final step: run the patched Bazaar from the build directory and
   use it.

Test tooling lives in `patches/harness/` and is never copied into `src/`:

| File | Purpose |
|---|---|
| `build.sh`, `harness.c` | standalone window with only the card (`HARNESS_LOCALEDIR` for translations) |
| `check-counts.py` | T4 count comparison |
| `app-shot.sh`, `app-shot.c` | `LD_PRELOAD` shim that screenshots the real app |
| `stall-probe.sh`, `stall-probe.c` | measures how long opening the Search page freezes the real app (`--vm`: emulated VM without 3D acceleration); prints the main thread's stack while it is blocked (hook H13) |
| `app-search.py` | GLib's desktop-file search, as Praya and GNOME Shell use it (R1) |
| `deb-check.sh`, `deb-check.c` | D1: runs the .deb installer's model (`bz-deb-installer.c`) on generated packages; installs nothing |
| `brand.sh` | sourced by the scripts: reads the app ID, program name and text domain from `meson.build`, so every test works on Bazaar and on Warsi |
| `run-tests.sh` | runs T1–T6 (T7 with `--notify`, R1 on a rebranded tree) and prints a summary |
| `expected-library-page.png` | reference screenshot of the real Library page |

## 7. Runtime requirements and packaging

| Requirement | Debian package |
|---|---|
| PackageKit daemon (apt backend) | `packagekit` |
| `packagekit-offline-update.service` (offline install at boot) | `packagekit` |
| polkit agent in the desktop session | desktop default |
| GNOME Shell (`org.gtk.Notifications`), or any notification portal backend | `gnome-shell` / `xdg-desktop-portal-*` |
| `gsettings` (the daemon reads `update-notifications` with it, like upstream reads `auto-update`) | `libglib2.0-bin` |
| `dpkg-deb` and `tar` (the .deb installer reads the app's launcher and icon from the file) | `dpkg`, `tar` (both essential) |

Packaging: the build now also installs
`$sysconfdir/xdg/autostart/bazaar.desktop` (hook H11) and
`$sysconfdir/xdg/gnome-mimeapps.list` (hook H15); a Debian package must
list `etc/xdg/autostart/bazaar.desktop` and `etc/xdg/gnome-mimeapps.list`
in its `.install` file. After
the rebrand every installed name changes; see
[§10.4](#104-packaging-a-rebranded-build).

Flatpak builds need D-Bus permissions (DIFF.md hook H6). The **Flathub**
Bazaar does not have them, so the card stays hidden there. They can be
granted manually:

```bash
flatpak override --user --system-talk-name=org.freedesktop.PackageKit --system-talk-name=org.freedesktop.login1 --talk-name=org.gnome.SessionManager io.github.kolunmi.Bazaar
```

No new **build** dependency: PackageKit is reached over GDBus (part of
GLib), not `libpackagekit-glib2`.

## 8. Design decisions

- **A separate card, not a `BzBackend` implementation.** Bazaar's backend
  interface and `BzEntry` model are built around apps, Flatpak refs and
  Flathub metadata; system packages are not apps. The user asked only for
  system updates. A self-contained card keeps upstream edits to about 30 lines,
  which makes re-applying cheap.
- **Plain GDBus instead of `libpackagekit-glib2`.** No new build dependency;
  works in the Flatpak SDK unchanged; PackageKit's D-Bus API is stable.
- **GDBus callbacks instead of libdex fibers.** Upstream `CODESTYLE.md`
  prefers libdex. The patch stays GLib/GIO-only so the new files can be built
  and tested standalone (TEST.md T2). This is a deliberate exception for a
  downstream patch. Everything else follows `CODESTYLE.md` (GNU style,
  `g_autoptr`, declarations first, file layout order).
- **Offline update is the default action.** Upgrading hundreds of packages,
  the desktop included, inside a running session is risky. Debian's GNOME
  Software does the same.
- **`only-trusted` is always set.** Unsigned packages are never installed.
- **Max 100 rows** in the expander: real systems were observed with ~1,500
  pending updates.
- **Title counts upgrades only; new and removed packages are named in the
  subtitle and listed (removals first).** The first version counted every
  `GetUpdates` entry, so the card said 1457 while apt said 1393 (61 new,
  3 removals). The user asked for the fix on 2026-11-14. Removals matter:
  "Restart & Update" removes those packages.
- **Card placed twice** (content page and "No Apps Found" page): upstream
  replaces the whole content with the empty page when no Flatpak apps exist.
- **Notifications in the existing background machinery.** Upstream already
  has a login-started daemon that runs an hourly worker, but both only run
  while Flatpak auto-update is on. The patch keeps that structure and adds
  one call in the worker, one condition in the daemon's idle exit, and a
  system autostart file, instead of a separate notifier service.
- **Autostart file named `bazaar.desktop`** with
  `X-GNOME-Autostart-Condition=GSettings io.github.kolunmi.Bazaar
  update-notifications`. Upstream writes `~/.config/autostart/bazaar.desktop`
  when the user enables automatic updates; the same name makes that per-user
  file replace the system one, so the daemon never starts twice.
- **One notification for both sources, once a day.** Matches GNOME
  Software's cadence; a replaced notification (fixed ID) never piles up.
- **No Preferences switch.** Kept the hooks small (a switch would touch
  upstream's Preferences dialog); the GSettings key and GNOME's per-app
  notification switch cover turning it off.
- **"Debian APT"** instead of PackageKit's "APT": the user asked for it
  (2026-11-15). Only the apt backend is renamed; other backends keep
  PackageKit's description.
- **Translations as a fragment plus a merge script** instead of a diff of
  `po/id.po`: upstream's translators change `po/id.po` constantly, so a diff
  would conflict on every re-apply. The script only appends missing
  entries, so upstream translations always win and re-running is harmless.

- **GTK renderer chosen by the app (H13).** GTK 4.23's Vulkan renderer
  draws the Search page broken (missing text, blank tiles), on real
  hardware and in a VirtualBox VM, so the app uses GL. Where GL is
  software-only (llvmpipe, e.g. a VM without 3D acceleration), every GPU
  renderer compiles shaders on the main thread, and opening Search froze
  the user's VM for 10–20 s ("not responding", crashes). There the app
  uses the Cairo renderer: 0.12 s instead of 4.5–5.6 s in the emulation,
  and less memory. The user asked for permanent fixes on 2026-11-15.
  Deciding in `main ()` covers every way the app starts, unlike a wrapper
  in the desktop file, which D-Bus activation and the daemon's own spawns
  bypass. An explicit `GSK_RENDERER` still wins.

## 9. Known limitations

- H13 forces GL even where GTK's Vulkan renderer would work, and Cairo on
  software GL even where a warm Mesa shader cache would make GL fast
  enough; neither costs anything visible in an app store. Drop it once
  GTK is fixed (DIFF.md H13, "When to drop it").
- H13 reads the GL driver name through libepoxy's `epoxy_glGetString`
  (found with `dlsym`). If a GTK release stops loading libepoxy, the check
  finds nothing and the app uses GL, as before H13 learned about software
  GL.
- The first notification comes up to an hour after login (the daemon's
  timer), then at most once a day.
- A notification click opens whatever owns `io.github.kolunmi.Bazaar` on
  D-Bus. With both the patched native Bazaar and Flathub's Bazaar installed,
  that can be the Flatpak one, which lacks `app.open-library` and just
  opens its main window.
- Notification strings are translated only into Indonesian (other languages
  fall back to English for the patch's strings).
- No per-package selection (same as GNOME Software on Debian).
- Only the new version is shown; `GetUpdates` does not return the installed one.
- No changelog view (`GetUpdateDetail` unused).
- `Offline.GetResults` (result of the last offline update) is not shown.
- Flatpak "Update All" and the APT card are independent.
- The update-count badge on the Library tab counts Flatpak updates only.
- Removals are shown in the card but not confirmed separately: "Restart &
  Update" has no confirmation dialog, even when packages will be removed.
- `clang-format` was not available when the code was written; formatting
  was done by hand following `.clang-format`.
- .deb installer limitations: see §12.5.

## 10. Rebrand: Warsi

### 10.1 Goal

On 2026-11-15 the user decided to ship the patched Bazaar as **Warsi**,
BlankOn Linux's app store, with its own app ID, `id.blankonlinux.warsi`.
Bazaar's license (GPL-3.0-or-later) allows this: the license doesn't require
keeping the name, the copyright notices stay, the source stays available,
and the About dialog says where Warsi comes from.

This is a full rebrand, not just a new display name. With its own app ID,
the desktop treats Warsi as a separate app. It installs next to Flathub's or
Debian's Bazaar and shares nothing with it: no settings, D-Bus names, search
provider, notifications or autostart file. Clicking a notification opens
Warsi.

### 10.2 What changes

| What | Bazaar | Warsi |
|---|---|---|
| App ID: GApplication, D-Bus names, desktop file, AppStream ID, GSettings schema, icons, systemd unit, search provider, cache/data directories, keyring schema | `io.github.kolunmi.Bazaar` | `id.blankonlinux.warsi` |
| Resource and GSettings path | `/io/github/kolunmi/Bazaar` | `/id/blankonlinux/warsi` |
| Programs | `bazaar`, `bazaar-daemon`, `bazaar-dl-worker` | `warsi`, `warsi-daemon`, `warsi-dl-worker` |
| Login page module | `$libdir/bazaar/libbazaar-login-page.so` | `$libdir/warsi/libwarsi-login-page.so` |
| Gettext domain, Meson project | `bazaar` | `warsi` |
| Autostart file (system and per user) | `bazaar.desktop` | `warsi.desktop` |
| Strings naming the app: window title, menu items, desktop and AppStream name, background-permission reason, "new version" banner, Flathub and software-source dialogs, shortcuts, inspector | Bazaar | Warsi |
| App icon (scalable, development, symbolic) | Bazaar's logo | Warsi's (`patches/rebrand/icons/`) |
| About dialog | name "Bazaar" | name "Warsi", then **"Originally Bazaar, patched by BlankOn Linux"** (Indonesian: "Dibangun dari Bazaar, dimodifikasi oleh BlankOn Linux"; wording changed on 2026-11-15 at the user's request, from "heavily patched" / "Berasal dari … besar-besaran") |
| AppStream description | | one more paragraph: Warsi is based on Bazaar, patched by BlankOn Linux, and also updates the system's Debian APT packages |
| Desktop file `Keywords`, in every language | System, Package, Manager, Discover, Flatpak, Software, App, Store | the same plus **Warung Aplikasi; Bazaar; GNOME Software**, and every translation also gets Software; Store; Warung Aplikasi; Bazaar; GNOME Software. So BlankOn's Praya and GNOME Shell find Warsi by "warsi", "warung aplikasi", "store", "software", "gnome software" and "bazaar" in English and Indonesian (asked for on 2026-11-15) |

These keep "Bazaar" on purpose:
- **The About dialog's section below the note**: developer "The Bazaar
  Contributors", website, "Report an Issue", credits, legal and
  acknowledgements. The user asked for this on 2026-11-15.
- **The AppStream developer name and links, and the copyright lines.**
- **The donation dialog, banner button and menu item** ("Donate to Bazaar",
  Bazaar's release notes). They support upstream's developer.
- **Internal names users never see**: types and files (`BzWindow`,
  `bazaar-daemon.c`), log domains `BAZAAR::…`, and the `bazaar-hook://`
  content scheme.
- **`libbge`**, a library with its own name.
- **`build-aux/`, `README.md`, `docs/` and CI**: upstream's Flatpak and
  development tooling.

### 10.3 How the script works

`patches/rebrand/rebrand.py` only touches `meson.build`, `src/`, `data/` and `po/`. It runs these steps:

1. **Renames files** named after the app ID.
2. **Replaces names in every text file** (`TOKENS`): the app ID, the
   resource path and the program names. It also changes translator
   comments to `Please treat "Warsi" and "Bazaar" as brand names`.
3. **Applies `EDITS`.** Each edit names a file, the **exact upstream text**
   and its replacement.
   - If the text isn't found (upstream changed it), the script stops and
     names the edit's file and text.
   - Insertions are skipped once present: the About hook, the new source
     file in `src/meson.build` and `po/POTFILES.in`, and the AppStream
     paragraph.
4. **Renames translated strings.** The `RENAMED` msgids change "Bazaar" →
   "Warsi" in every `po/<lang>.po`, so no translation is lost. In each
   translation:
   - a whole-word "Bazaar" becomes "Warsi";
   - a translation that names the app another way (Spanish "Bazar",
     Ukrainian "Базару", Serbian "Пијаца", Polish "Bazaaru") is marked
     **fuzzy**, so it shows in English until a translator updates it;
   - a translation that doesn't name the app ("About the app") is kept.

   On 2026-11-15 this marked 86 translations in 15 languages fuzzy, none in
   Indonesian.

   The desktop file's `Keywords` msgid gets the new search words too, and
   each translation keeps its own keywords with the English search words
   appended (`OLD_KEYWORDS`, `EXTRA_KEYWORDS`, `add_keywords ()`). App
   launchers search with GLib's `g_desktop_app_info_search ()`, which reads
   `Keywords` in the current language only, so a translation without "store"
   would hide Warsi from an English search word in that language.
5. **Adds the new files.** It copies `patches/rebrand/src/bz-rebrand.{c,h}`
   and the icons, removes Bazaar's icon source (`….Source.svg`), and merges
   `patches/rebrand/po/id-rebrand.po` with `merge-translations.py`.
6. **Checks the result.** It fails if the old app ID is still anywhere in
   scope, or if a `.po` file no longer passes `msgfmt -c`.

**The About note.** libadwaita's `AdwAboutDialog` has no property for a line
under the name. Its `comments` property would replace the "Website" row
with a "Details" row, which changes the Bazaar section the user wanted kept.
So `bz_rebrand_about_dialog ()` finds the name label in the dialog's header
and adds a label after it. If a libadwaita release changes that layout, it
falls back to `comments` (API.md §3.5).

**Why a script and not a diff.** The rename touches about 150 files and
about 1,000 lines, most of them a resource path in every widget. A diff of
that would conflict with nearly every upstream change. Instead the script is
re-run on a fresh upstream tree after H1–H12, and only its `EDITS` need care
when upstream rewords a string.

**Updating to a new upstream.** Start from upstream (not from a rebranded
commit), apply H1–H12, then run the script. Merging upstream into a
rebranded branch also works, but it conflicts on renamed files and strings.

### 10.4 Packaging a rebranded build

A Debian package of Warsi installs different files (R1 lists them):
- `/usr/bin/warsi`, `/usr/bin/warsi-daemon`, `/usr/bin/warsi-dl-worker`;
- `/usr/lib/<triplet>/warsi/libwarsi-login-page.so`;
- `/etc/xdg/autostart/warsi.desktop`;
- `/etc/xdg/gnome-mimeapps.list` (H15: Warsi opens `.deb` files);
- `id.blankonlinux.warsi.*` desktop, AppStream, D-Bus, systemd, search
  provider, schema and icon files;
- translations as `warsi.mo`.

The source and binary packages would be named `warsi`. No installed file is
shared with Debian's `bazaar` package any more, except in the `libbge`
packages, which are unchanged. So the two can be installed together without
`Conflicts` or `Replaces`.

### 10.5 Limitations

- Settings, the Flathub sign-in (keyring entry), caches and the per-user
  autostart file are stored under the new ID. Nothing carries over from an
  installed Bazaar and there is no migration: Warsi starts fresh.
- 86 translations in 15 languages stay fuzzy until their translators adapt
  "Warsi". Indonesian is complete.
- The Warsi icon is the owner's artwork (2026-11-24): a market stall, orange
  awning, blue body with a script "W". `patches/rebrand/icons/app-source.svg`
  is their Inkscape file (not installed); `app.svg` is it cleaned of editor
  data; `app-devel.svg` is the same stall in grey; `app-symbolic.svg` is the
  awning over the "W" in one colour. Edit the source, then regenerate the
  other three from its paths.
- The Warsi wordmark (owner's artwork, 2026-11-24) sits at the start of the
  main header bar, before the account avatar and the background-task
  spinner: `src/warsi-wordmark-symbolic.svg` (cropped from
  `patches/rebrand/icons/wordmark-source.svg`), drawn by style.css
  `.warsi-wordmark` with `-gtk-recolor()` in `--accent-color`, so it follows
  the user's accent colour and dark mode. Hidden while loading (the title
  says "Warsi" then); below 700 px the header bar's title is hidden instead,
  as the wordmark already names the app. rebrand.py adds it (EDITS on
  bz-window.blp, style.css, bazaar.gresource.xml). GNOME's HIG does not put
  branding in header bars; the owner chose this, kept small and themed.
- The logo is set in the Praise typeface by Robert Leuschke (Copyright 2014
  The Praise Project Authors, SIL Open Font License 1.1). The font is not
  shipped, only outlines; it is credited in the About dialog's Legal page
  (bz-rebrand.c, "Warsi Logo") and in the package's debian/copyright.
- The About dialog's "Report an Issue" still points at upstream's tracker,
  as the user asked. Reports about the APT card or notifications belong to
  BlankOn.

## 11. Attempt log

Append one row per attempt. Never delete rows.

| Date | Upstream commit (target) | Bazaar version | Result | Notes |
|---|---|---|---|---|
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Applied, full build clean, card verified in the real app | Original implementation. BlankOn (Debian-based), GTK 4.23.4, libadwaita 1.9.2, GLib 2.89.4, PackageKit 1.3.6 (apt). Offline/live install buttons not exercised (would upgrade the dev machine). |
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Re-applied from `../bazaar-ai-patches` onto clean tree; `git apply --3way` clean, harness + full build with no warnings, harness card shows 1457 updates | Same base commit, so API check trivial. Install buttons not exercised. |
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Count fix + TEST.md; `run-tests.sh` T1–T6 all PASS, real app screenshot taken | Card said 1457 updates vs apt's 1393: PackageKit's `GetUpdates` also lists 61 new and 3 removed packages. Title now counts upgrades only (1393 = apt), subtitle/list show new and removed packages. Added TEST.md, `run-tests.sh`, `check-counts.py`, `app-shot.{sh,c}`, harness `HARNESS_EXPAND`/`HARNESS_HEIGHT`; refreshed `expected-library-page.png`. `pkcon` unavailable on BlankOn, replaced by PackageKit Python bindings. Install buttons not exercised. |
| 2026-11-15 | `9560ad18cd60652aae10e81708ff4d36df775aa8` (applied on top of the user's commit `3c12e48a`) | 0.10.1 | ✅ Notifications + "Debian APT" + Indonesian; `run-tests.sh --notify` T1–T7 all PASS | New: `bz-update-notifier.{c,h}`, `patches/data/bazaar-update-notifications.desktop`, `patches/po/{id-apt-updates.po,merge-translations.py}`; hooks H7–H12; `BzAptUpdates:loaded`. Real notification seen via org.gtk.Notifications (4 apps, 1393 system); `app.open-library` switched a running window to Library; daemon stays alive only with `update-notifications` on; daily limit and off switch verified. DIFF.md applied to a clean base checkout reproduces the tree byte for byte. Scripts take `BZ_REPO` so they run from `../bazaar-ai-patches`. Not tested: notification click on an installed (packaged) build; login autostart. |
| 2026-11-15 | `9560ad18cd60652aae10e81708ff4d36df775aa8` (on top of the user's commit `e90c8f37`) | 0.10.1 | ✅ Rebrand to Warsi (`id.blankonlinux.warsi`), step R; `run-tests.sh --notify` with R1 | New: `patches/rebrand/` (`rebrand.py`, `bz-rebrand.{c,h}`, icons, `po/id-rebrand.po`), `patches/harness/brand.sh`; `merge-translations.py` takes fragment arguments; app-shot gains `APP_SHOT_ABOUT`. `rebrand.py --check` clean after one run; installed tree has no Bazaar ID or file name; desktop file and AppStream valid; About dialog shows the note above Bazaar's section (screenshot). 86 translations in 15 languages marked fuzzy (none Indonesian). Uninstalled, T7's notification goes through the portal (no desktop file for the new ID). Not tested: an installed Warsi (notification click, autostart, GNOME search provider). |
| 2026-11-17 | `9560ad18cd60652aae10e81708ff4d36df775aa8` (on top of the user's commit `1e2931e4`) | 0.10.1 | ✅ .deb file installer (§12), hooks H14–H15; D1 passes; dialog screenshots for new app, older/newer/same version, broken dependencies and damaged file | New: `bz-deb-installer.{c,h}`, `bz-deb-install-dialog.{c,h,blp}`, `patches/data/bazaar-deb-mimeapps.list`, `patches/harness/deb-check.{sh,c}`, `APP_SHOT_OPEN`; 23 Indonesian strings. `gio mime` confirms `/etc/xdg/gnome-mimeapps.list` overrides GNOME Software. DIFF.md's diff applied to a clean base plus `rebrand.py` reproduces the tree. Not tested: a real install (needs the owner's consent), Open after install. |
| 2026-11-24 | `9560ad18cd60652aae10e81708ff4d36df775aa8` (on top of the user's commit `f19b429b`) | 0.10.1 | ✅ GNOME Extensions tab (§13), hooks H16–H17 (and H1, H2, H5, H6, H12 extended); `run-tests.sh` T1–T6, R1, D1, E1 all PASS (E1: 21 checks against extensions.gnome.org and a fake Shell) | New: `bz-gnome-extensions.{c,h}`, `bz-gnome-extensions-page.{c,h,blp}`, `bz-gnome-extension-dialog.{c,h,blp}`, harness `ext-check.{c,sh}`, `fake-shell-extensions.py`; app-shot gains `APP_SHOT_PAGE`, `APP_SHOT_EXT_*`, `APP_SHOT_WIDTH`. Screenshots: grid, search, Installed view, dialogs (user and system extension), dark, narrow. DIFF.md diff applied to a clean base checkout plus masters, translations and rebrand reproduces the tree. Not tested: installing from the real GNOME Shell (E3). |


## 12. .deb file installer

### 12.1 Goal

On BlankOn, double-clicking a downloaded `.deb` file opened GNOME Software's
local-file installer. The user asked on 2026-11-17 that the app handle
`.deb` files instead, so Warsi is the one place for software.

### 12.2 What the user sees

Opening a `.deb` (double click in Files, "Open With", or
`bazaar path/to/file.deb`) opens the main window with an **Install
Package** dialog:

- Header: the app's icon and name, read from the launcher (`.desktop` file)
  and icon inside the package; a package without a launcher shows its
  package name and a generic package icon. Then the package's summary.
- One main button, by what is installed:
  **Install** (not installed); **Update**, with "Version X is installed";
  **Install Older Version**, with "A newer version, X, is installed"; no
  button but "This version is installed" (and **Open** when it has a
  launcher). After installing: "Installed" and **Open**.
- A warning row before installing: "Only install packages you trust …":
  a local file has no repository signature.
- **Details**: package name, version, installed version (when different),
  installed size, license, website (opens the browser); an expander
  "Also installs or updates N other packages" (dependencies from the
  repositories), and "Removes N packages" (expanded, red) when installing
  would remove some.
- **Description**.
- While installing: progress bar, PackageKit's status text, **Cancel**; the
  dialog cannot be closed until it ends. Authentication is polkit's dialog;
  closing it is not an error.
- Errors: "This Package Cannot Be Installed" (dependencies missing or in
  conflict), "Cannot Open This Package" (damaged file, wrong format, no
  PackageKit), "Installation Failed"; apt's own message under
  **Technical Details**.
- "Restart Required" with **Restart Now** when the package asks for it.

### 12.3 How it works

```
 bz-application.c open_location_take ()   (hook H14: name ends in .deb)
        │
        ▼
 BzDebInstallDialog  patches/src/bz-deb-install-dialog.{c,h,blp}
   AdwDialog. Pure view of the model ("changed" signal → sync_ui).
        │
        ▼
 BzDebInstaller      patches/src/bz-deb-installer.{c,h}
   One file. Plain GDBus to PackageKit, no GTK, no other Bazaar code.
   Load:    GetDetailsLocal → GetFilesLocal → Resolve(installed) → InstallFiles(SIMULATE)
   Install: InstallFiles (ALLOW_DOWNGRADE for an older version), interactive polkit
   Launcher and icon: `dpkg-deb --fsys-tarfile FILE | tar -xO MEMBER`, read to
   memory only (nothing is extracted to disk)
```

- The installed version comes from `Resolve` with the `installed` filter
  (matching the file's architecture, or `all`). Same version → nothing to
  install (PackageKit's apt backend does nothing for a same-version local
  reinstall, checked on 2026-11-17), so no button is offered.
- The action comes from the simulated install's `Package` signal for the
  file's own package: `INSTALLING`, `UPDATING` or `DOWNGRADING`. Other
  packages are listed by their info: installing/updating, or
  removing/obsoleting.
- A simulated install that fails (apt's "unmet dependencies") blocks the
  install with apt's message as technical details.
- PackageKit runs as root and reads the file itself. A file not on a local
  disk (network share, GVfs FUSE path) is first copied to
  `~/.cache/<program>/deb-staging/`.
- Transactions send `interactive=true`, `background=false` and the
  user's `locale` hints, like `BzAptUpdates`.

### 12.4 Design decisions

- **PackageKit, not `apt`/`dpkg` directly:** same daemon, polkit rules and
  lock handling as the System Updates card; no root helper of our own.
  GNOME Software does the same.
- **Not ONLY_TRUSTED:** PackageKit refuses that flag for local files
  ("Local packages cannot be authenticated"), so the install uses polkit's
  `org.freedesktop.packagekit.package-install-untrusted` action, and the
  dialog warns.
- **Simulate before showing the button:** the user sees what else gets
  installed or removed before authenticating, and a package that cannot be
  installed says so before asking for a password.
- **No uninstall button:** removing system packages stays out of scope
  (§3).
- **Default handler through `/etc/xdg/gnome-mimeapps.list`** (H15), not by
  removing or diverting GNOME Software: both stay installable, the user's
  own default still wins.

### 12.5 Limitations

- Only one file at a time (upstream opens only the first location given).
- Icons: hicolor SVG or the largest PNG, else `/usr/share/pixmaps`; an icon
  only in another theme falls back to the generic package icon. AppStream
  metainfo inside the package (screenshots, long description) is not read.
- No "Open" for apps whose launcher has `NoDisplay=true`, or for packages
  without a launcher (libraries, command-line tools).
- In a Flatpak build the dialog cannot work (no `dpkg-deb`, paths inside
  the sandbox are not the host's); it shows PackageKit's error.
- The default-handler file applies to GNOME sessions (`XDG_CURRENT_DESKTOP`
  contains `GNOME`) only.
- Not exercised automatically: a real install (it changes the machine).
  TEST.md D3 is the manual test.

## 13. GNOME Extensions tab

### 13.1 Goal

Added 2026-11-24 at the owner's request: an **Extensions** tab after
Search that lists GNOME Shell extensions from
[extensions.gnome.org](https://extensions.gnome.org/) (search included),
and installs and manages them, so BlankOn users need neither the website's
browser connector nor a separate Extension Manager app.

### 13.2 What the user sees

- A fourth tab, **Extensions** (`application-x-addon-symbolic`), shown only
  when GNOME Shell's extension service is on the session bus (hidden in
  other desktops). In narrow windows it moves to the bottom bar with the
  others.
- At the top, the same search bar as the Search page (upstream's
  `BzSearchBar`), with a sort button where that page has its filter button
  (highlighted when not the default): **Popular** (downloads), **Trending**
  (recent popularity), **Recent** (last updated), and **Installed** (the
  user's extensions, from GNOME Shell, filtered by the same search). Only
  extensions compatible with the running GNOME Shell version are listed.
- Below it, topic pills styled like the Search page's (owner's request,
  2026-11-24): Dock, Top Bar, Workspaces, Windows, App Menu, Clipboard,
  System Monitor, Battery, Weather, Appearance, Wallpaper, Screenshot.
  extensions.gnome.org has no categories, so a pill searches for an English
  keyword (`pills[]` in `bz-gnome-extensions-page.c`; "Top Bar" searches
  "panel", "Appearance" "theme"). Wrapped when wide, one scrolling row
  when narrow.
- A grid of cards like the app cards: icon (a generic puzzle piece when the
  extension has none), name, first line of the description, and an
  **Enabled** pill (green) or a grey status pill (Disabled, Error,
  Incompatible, ...) for installed ones. Scrolling near the end loads the
  next page (24 per page); while it loads, three dots pulse slowly in a
  small pill at the bottom (owner's request; CSS loaded by the page, no
  change to upstream's `style.css`).
- Clicking a card opens a dialog: icon, name, "by" creator, **Install** for
  a new extension (GNOME Shell then shows its own confirmation), or for an
  installed one an **Enabled** switch, **Settings** (when it has
  preferences) and **Remove** (only for the user's own extensions; asks
  first). Then the screenshot, the description, downloads, links to its
  extensions.gnome.org page and website, Shell's error if it failed, and a
  note that extensions run with full access to the session.
- If the user turned extensions off in GNOME, a banner says so with
  **Turn On**. If extensions.gnome.org cannot be reached, the page says so
  with **Try Again**.

### 13.3 How it works

New files (masters in `patches/src/`):

| File | Role |
|---|---|
| `bz-gnome-extensions.{c,h}` | `BzGnomeExtension` (one extension) and `BzGnomeExtensions` (the model): extensions.gnome.org's JSON API over libsoup, GNOME Shell's D-Bus service over GDBus. No other Bazaar code. |
| `bz-gnome-extensions-page.{c,h,blp}` | the tab: search bar, sort menu (`page.sort` action on the `sort-name` property), topic pills, grid, paging, empty and error states |
| `bz-gnome-extension-dialog.{c,h,blp}` | the details dialog and its actions |

- **Listing:** `GET /extension-query/?search=…&sort=downloads|popularity|recent&page=N&n_per_page=24&shell_version=MAJOR`.
  Icons and screenshots are fetched as textures (screenshots only when the
  dialog opens). Nothing is fetched until the tab is first shown.
- **Installed view:** GNOME Shell's `ListExtensions`; each one's icon and
  link from `GET /extension-info/?uuid=…` (404 for extensions not on the
  site, e.g. system ones: they keep the generic icon).
- **Actions:** GNOME Shell's `InstallRemoteExtension` (Shell downloads the
  right version from extensions.gnome.org and asks the user),
  `EnableExtension`, `DisableExtension`, `UninstallExtension`,
  `OpenExtensionPrefs`, and the `UserExtensionsEnabled` property. Shell's
  `ExtensionStateChanged` signal keeps every card and dialog current, also
  for changes made elsewhere (the Extensions app, the website).
- Hooks: H1 (sources), H2 (UI files), H5 (translatable files), H6
  (Flatpak: `--talk-name=org.gnome.Shell.Extensions`), H12 (Indonesian),
  H16 (`bz-window.c` registers the page type), H17 (`bz-window.blp` adds
  the tab). See DIFF.md and API.md §3.8–§3.9.

### 13.4 Design decisions

- **Install through GNOME Shell, never by unpacking zips ourselves:** Shell
  picks the build for its version, verifies it, shows its own consent
  dialog and loads it without a re-login; the same path the website's
  browser connector uses. Warsi never writes to the extensions folder.
- **The same API as the website and GNOME Extension Manager:** public, no
  key. It is not formally versioned; all of it is in one file
  (`bz-gnome-extensions.c`) so a change on the site is one place to fix.
- **libsoup and json-glib directly, not Bazaar's network helpers:** keeps
  the model free of Bazaar code, so `ext-check` (E1) can test it alone.
  Both libraries are already Bazaar dependencies.
- **Tab hidden without GNOME Shell:** nothing in it could work there.
- **Remove only per-user extensions:** Shell cannot remove system ones
  (installed by packages); their switch still works.

### 13.5 Limitations

- Needs GNOME Shell (any desktop without `org.gnome.Shell.Extensions`
  hides the tab) and network access to extensions.gnome.org.
- Extension updates are left to GNOME Shell, which updates them itself at
  login; Warsi does not list them in the Library.
- No reviews or ratings, no version picker, no "Details" from metadata
  beyond what the site returns; descriptions are plain text.
- The preferences window opens without a parent (no xdg-foreign handle).
- Not exercised automatically: installing from the real GNOME Shell (it
  changes the user's session). TEST.md E3 is the manual test; E1 and E2
  use a fake Shell.

