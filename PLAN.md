# PLAN: APT system updates for Bazaar

This is the **main entry point** for the downstream patch. It explains what
the patch achieves, how it is built, and how to (re-)apply it to any version
of the Bazaar codebase. It is written for humans **and** AI agents, on any
machine, at any later time.

| Document | Read it for |
|---|---|
| `PLAN.md` (this file) | goal, design, step-by-step procedure, verification |
| [`API.md`](API.md) | every external API the patch relies on, the base commit, and how to check each API for breaking changes upstream |
| [`DIFF.md`](DIFF.md) | every change to **existing** upstream files: what, why, and how it connects to the new files |
| [`TEST.md`](TEST.md) | how to test the patch once applied: automated tests, screenshots, manual tests, running the app |
| `patches/` | the **master copies** of every entirely new file written by the patch |

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
existing Flatpak updates.

## 2. What the user sees

A **"System Updates"** card at the top of the **Library** page, directly
above upstream's Flatpak "Pending Updates" section
(reference screenshot: [`patches/harness/expected-library-page.png`](patches/harness/expected-library-page.png)):

- **"N System Package Updates"**, where N counts **upgrades only** (equal to
  `apt list --upgradable`). Subtitle: "Includes M security updates", then
  "· K new packages" and "· R packages will be removed" when the upgrade
  installs or removes packages; with none of those, "Provided by APT".
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

## 3. Scope

**In scope:** detect APT through PackageKit, list updates, refresh lists,
apply all updates (offline by default, live optionally), progress, cancel,
errors, reboot.

**Out of scope (do NOT add when re-applying):**

- Browsing, searching, installing or removing APT packages; users do that
  in a terminal.
- Showing APT packages as Bazaar app entries or app pages.
- Implementing Bazaar's `BzBackend` interface, or touching its transaction
  manager or the background `bazaar-daemon`.
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

**The new files depend on nothing inside Bazaar** except two conventions:
`config.h` defines `GETTEXT_PACKAGE`, and the GResource prefix is
`/io/github/kolunmi/Bazaar/`. So upstream refactors rarely touch them; only
the small hooks in DIFF.md need attention.

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

Exact methods, signatures and enum values are in [API.md](API.md).

## 5. Procedure

### 5.0 Inputs

- A Bazaar source tree (any version) = **the target**.
- This `ai-patches/` directory, copied into the target root (it is portable;
  nothing in it depends on its location except `patches/harness/build.sh`,
  which expects to live at `<repo>/ai-patches/patches/harness/`).

### 5.1 Check API stability first

Follow [API.md §2 "Stability check procedure"](API.md#2-stability-check-procedure).
It compares the target against the **base commit** the patch was written
for and lists exactly what to look at. If an API changed, adapt the code in
`patches/src/` (the master copies) **before** copying it.

### 5.2 Copy the new files

```bash
cp ai-patches/patches/src/bz-apt-updates.h ai-patches/patches/src/bz-apt-updates.c ai-patches/patches/src/bz-apt-updates-card.h ai-patches/patches/src/bz-apt-updates-card.c ai-patches/patches/src/bz-apt-updates-card.blp src/
```

`patches/src/` mirrors the repository layout: every file there goes to the
same relative path in the target. `patches/harness/` is **test-only** and is
never copied into `src/`.

### 5.3 Apply the hooks

Apply every hook in [DIFF.md](DIFF.md), in its order (H1 → H6). Each hook
has an *anchor* to find, the exact change, the *reason*, and what to do if
the anchor no longer exists. Try `git apply --3way` with the unified diff in
DIFF.md first; fall back to manual edits per hook.

### 5.4 Build and verify

Follow [TEST.md](TEST.md) (summary in [§6 Verification](#6-verification)).
Minimum bar: `ai-patches/patches/harness/run-tests.sh` passes T1–T6 and the
screenshots match the reference.

### 5.5 Record the attempt

Append a row to [§10 Attempt log](#10-attempt-log) (date, upstream commit,
result, notes). If you changed anything in `patches/src/`, keep the master
copies and the applied copies identical:

```bash
for f in ai-patches/patches/src/*; do cmp "$f" "src/$(basename "$f")" || echo "OUT OF SYNC: $f"; done
```

If you had to change a hook, update DIFF.md. If an API changed, update
API.md, including the base commit. **Keep the three documents true.**

## 6. Verification

Testing is described step by step in **[TEST.md](TEST.md)**. In short:

1. Install the build and test dependencies (TEST.md §0). `pkcon` is not
   needed; the count check uses PackageKit's Python bindings
   (`gir1.2-packagekitglib-1.0`).
2. Run the automated, read-only tests from the repository root:

   ```bash
   ai-patches/patches/harness/run-tests.sh
   ```

   | Test | Checks |
   |---|---|
   | T1 | applied copies identical to `patches/src/`, every hook marker present, manifest valid JSON |
   | T2 | harness (`patches/harness/build.sh`) compiles the patch with `-Werror` |
   | T3 | full Bazaar build, no warnings from the patch's files |
   | T4 | `check-counts.py`: card's upgrades/security/new/removals equal PackageKit's, and upgrades equal `apt list --upgradable` |
   | T5 | harness screenshots, collapsed and expanded |
   | T6 | `app-shot.sh`: screenshots of the **real app's** Library page |

3. T7: compare the screenshots in `build/apt-tests/` with
   [`patches/harness/expected-library-page.png`](patches/harness/expected-library-page.png).
4. T8: manual tests of buttons and error paths. "Restart & Update" and
   "Install Now" really upgrade the machine: only with the owner's consent.
5. T9, the final step: run the patched Bazaar from the build directory and
   use it.

Test tooling lives in `patches/harness/` and is never copied into `src/`:

| File | Purpose |
|---|---|
| `build.sh`, `harness.c` | standalone window with only the card |
| `check-counts.py` | T4 count comparison |
| `app-shot.sh`, `app-shot.c` | `LD_PRELOAD` shim that screenshots the real app |
| `run-tests.sh` | runs T1–T6 and prints a summary |
| `expected-library-page.png` | reference screenshot of the real Library page |

## 7. Runtime requirements and packaging

| Requirement | Debian package |
|---|---|
| PackageKit daemon (apt backend) | `packagekit` |
| `packagekit-offline-update.service` (offline install at boot) | `packagekit` |
| polkit agent in the desktop session | desktop default |

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

## 9. Known limitations

- `bazaar-daemon` (background notifications) does not know about APT updates.
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

## 10. Attempt log

Append one row per attempt. Never delete rows.

| Date | Upstream commit (target) | Bazaar version | Result | Notes |
|---|---|---|---|---|
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Applied, full build clean, card verified in the real app | Original implementation. BlankOn (Debian-based), GTK 4.23.4, libadwaita 1.9.2, GLib 2.89.4, PackageKit 1.3.6 (apt). Offline/live install buttons not exercised (would upgrade the dev machine). |
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Re-applied from `../bazaar-ai-patches` onto clean tree; `git apply --3way` clean, harness + full build with no warnings, harness card shows 1457 updates | Same base commit, so API check trivial. Install buttons not exercised. |
| 2026-11-14 | `9560ad18cd60652aae10e81708ff4d36df775aa8` | 0.10.1 (`v0.10.0-29-g9560ad18`) | ✅ Count fix + TEST.md; `run-tests.sh` T1–T6 all PASS, real app screenshot taken | Card said 1457 updates vs apt's 1393: PackageKit's `GetUpdates` also lists 61 new and 3 removed packages. Title now counts upgrades only (1393 = apt), subtitle/list show new and removed packages. Added TEST.md, `run-tests.sh`, `check-counts.py`, `app-shot.{sh,c}`, harness `HARNESS_EXPAND`/`HARNESS_HEIGHT`; refreshed `expected-library-page.png`. `pkcon` unavailable on BlankOn, replaced by PackageKit Python bindings. Install buttons not exercised. |
