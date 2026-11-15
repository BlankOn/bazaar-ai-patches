# DIFF: changes to existing upstream files

This file documents **every change the patch makes to files that already
exist upstream**: what changes, why it is needed, and how it connects to the
entirely new files in [`patches/`](patches/). Read [PLAN.md](PLAN.md) first.

- Base commit: `9560ad18cd60652aae10e81708ff4d36df775aa8` (see [API.md §1](API.md#1-base-the-codebase-the-patch-was-written-against)).
- Upstream files touched: **12** by hooks H1–H11 and H13 (about 100 added lines, 1
  changed line, nothing deleted), plus `po/id.po`, which hook H12 extends
  with a script (about 200 appended lines; one obsolete entry removed).
- Every hook except the JSON manifest carries the marker
  `DOWNSTREAM PATCH (APT system updates)`; find them with
  `git grep -n "DOWNSTREAM PATCH (APT system updates)"`.
- Then the **rebrand to Warsi** (step R, last) changes about 150 more files.
  It is a script, not hooks: see [R](#r-rebrand-to-warsi-script-no-diff).
  Its own code changes carry `DOWNSTREAM PATCH (Warsi rebrand)`.
- The hooks below show the **Bazaar** names (`io.github.kolunmi.Bazaar`,
  `bazaar-daemon`, `bazaar.desktop`), as they are applied to upstream. On a
  rebranded tree the same lines read `id.blankonlinux.warsi`,
  `warsi-daemon` and `warsi.desktop`.

## Overview: how upstream connects to the new files

```text
 src/meson.build ─────────(H1)──► compiles  src/bz-apt-updates.c
                                             src/bz-apt-updates-card.c
                           (H1)──► compiles  src/bz-apt-updates-card.blp ──► bz-apt-updates-card.ui
 src/bazaar.gresource.xml ─(H2)──► embeds    bz-apt-updates-card.ui at /io/github/kolunmi/Bazaar/
                                             (loaded by bz_apt_updates_card_class_init)
 src/bz-library-page.c ───(H3)──► #include "bz-apt-updates-card.h"
                                   g_type_ensure (BZ_TYPE_APT_UPDATES_CARD)
 src/bz-library-page.blp ─(H4)──► instantiates $BzAptUpdatesCard (×2)
                                       └─► bz_apt_updates_get_default ()  (bz-apt-updates.c)
 po/POTFILES.in ──────────(H5)──► translatable strings of the 4 new UI/C files
 build-aux/flatpak/…json ─(H6)──► D-Bus permissions used by bz-apt-updates.c at runtime
 src/meson.build ─────────(H1)──► compiles  src/bz-update-notifier.c
 src/update-worker.c ─────(H7)──► bz_update_notifier_run ()        (bz-update-notifier.c)
 src/bazaar-daemon.c ─────(H8)──► stays alive while update-notifications is on
 src/bz-application.c ────(H9)──► bz_update_notifier_add_app_actions () → app.open-library
 data/…gschema.xml ──────(H10)──► keys update-notifications, last-update-notification
 data/meson.build ───────(H11)──► installs data/bazaar-update-notifications.desktop
                                   as $sysconfdir/xdg/autostart/bazaar.desktop
 po/id.po ───────────────(H12)──► patches/po/id-apt-updates.po, merged by
                                   patches/po/merge-translations.py
 src/main.c ─────────────(H13)──► GSK_RENDERER=gl unless set (GTK renderer workaround)
```

| Hook | Upstream file | New file / symbol it connects to |
|---|---|---|
| H1 | `src/meson.build` | `bz-apt-updates.c`, `bz-apt-updates-card.c`, `bz-apt-updates-card.blp`, `bz-update-notifier.c` |
| H2 | `src/bazaar.gresource.xml` | `bz-apt-updates-card.ui` (compiled from the `.blp`) |
| H3 | `src/bz-library-page.c` | `bz-apt-updates-card.h` → `BZ_TYPE_APT_UPDATES_CARD` |
| H4 | `src/bz-library-page.blp` | `BzAptUpdatesCard` widget type |
| H5 | `po/POTFILES.in` | `_()` strings in the four files |
| H6 | `build-aux/flatpak/io.github.kolunmi.Bazaar.json` | D-Bus names called by `bz-apt-updates.c` (see API.md §3.1–§3.2) |
| H7 | `src/update-worker.c` | `bz_update_notifier_run ()` |
| H8 | `src/bazaar-daemon.c` | GSettings key `update-notifications` (H10) |
| H9 | `src/bz-application.c` | `bz_update_notifier_add_app_actions ()` |
| H10 | `data/io.github.kolunmi.Bazaar.gschema.xml` | keys read/written by `bz-update-notifier.c`, H8 and H11 |
| H11 | `data/meson.build` | `patches/data/bazaar-update-notifications.desktop` |
| H12 | `po/id.po` | `patches/po/id-apt-updates.po` (script, no diff) |
| H13 | `src/main.c` | none: makes GTK use its GL renderer (works around a GTK 4.23 Vulkan bug) |

Apply order: H1 → H13, then R. H1–H4 are required for the card; H7–H11 for the
notifications (H7, H9, H10 together; H8 and H11 make them arrive without
upstream's auto-update); H5 and H12 for translations; H6 only matters for
Flatpak builds.

---

## H1: `src/meson.build`: compile the new sources

**Why:** Meson only compiles files listed explicitly. Without this the new C
files are not linked into `bazaar` and the Blueprint is never turned into a
`.ui` file.

**Change 1, C sources.** Anchor: the `bz_sources = files(` list (alphabetical),
line `'bz-apps-page.c',`. Insert after it:

```meson
  # DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md
  'bz-apt-updates.c',
  'bz-apt-updates-card.c',
  # DOWNSTREAM PATCH (APT system updates): notifications
  'bz-update-notifier.c',
```

(`bz_sources` is also what the `bazaar` executable, which runs the update
worker, is built from; `bz-update-notifier.c` needs `flatpak`, `gtk4` and
`libadwaita`, all already in `bz_deps`.)

**Change 2, Blueprint.** Anchor: the `blueprints = custom_target('blueprints',`
input list, line `'bz-apps-page.blp',`. Insert after it:

```meson
    # DOWNSTREAM PATCH (APT system updates)
    'bz-apt-updates-card.blp',
```

**Connection:** produces `bz-apt-updates.o`, `bz-apt-updates-card.o` and
`bz-apt-updates-card.ui` from `patches/src/`. No new `dependency()` is
needed: the new code uses only GLib/GIO, GTK and libadwaita, which upstream
already links.

**If the anchor is gone:** find wherever upstream lists `bz-library-page.c`
(C sources) and `bz-library-page.blp` (Blueprints) and add the files there.
If upstream stops compiling Blueprints in Meson, compile
`bz-apt-updates-card.blp` the same way as `bz-library-page.blp`.

---

## H2: `src/bazaar.gresource.xml`: embed the UI file

**Why:** `bz_apt_updates_card_class_init ()` loads its template with
`gtk_widget_class_set_template_from_resource (…, "/io/github/kolunmi/Bazaar/bz-apt-updates-card.ui")`.
The file must be in the compiled GResource bundle or the card aborts on creation.

**Change.** Anchor: `<file preprocess="xml-stripblanks">bz-apps-page.ui</file>`.
Insert after it:

```xml
    <!-- DOWNSTREAM PATCH (APT system updates) -->
    <file preprocess="xml-stripblanks">bz-apt-updates-card.ui</file>
```

**Connection:** the `.ui` produced by H1 → resource path used in
`patches/src/bz-apt-updates-card.c`.

**If the anchor is gone:** add it next to `bz-library-page.ui` or
`bz-updates-card.ui`. If upstream changes the `<gresource prefix=…>`, update
the path string in `gtk_widget_class_set_template_from_resource ()` in
`patches/src/bz-apt-updates-card.c` (and in the harness `build.sh` `res.xml`).

---

## H3: `src/bz-library-page.c`: register the widget type

**Why:** GtkBuilder can only instantiate `$BzAptUpdatesCard` from the
Library page template if the GType is registered before the template is
parsed. Upstream does the same for its own `BzUpdatesCard`.

**Change 1, include.** Anchor: the block of `#include "bz-….h"` lines after
`#include <bzvala.h>`. Insert before `#include "bz-entry-group.h"`:

```c
/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
#include "bz-apt-updates-card.h"
```

**Change 2, type registration.** Anchor: in `bz_library_page_class_init ()`,
the line `g_type_ensure (BZ_TYPE_UPDATES_CARD);` (just before
`gtk_widget_class_set_template_from_resource`). Insert after it:

```c
  /* DOWNSTREAM PATCH (APT system updates) */
  g_type_ensure (BZ_TYPE_APT_UPDATES_CARD);
```

**Connection:** `BZ_TYPE_APT_UPDATES_CARD` is declared in
`patches/src/bz-apt-updates-card.h`.

**If the anchor is gone:** put the `g_type_ensure` in the `class_init` of
whatever widget's template contains `$BzAptUpdatesCard` (see H4), before its
`gtk_widget_class_set_template_from_resource ()` call.

---

## H4: `src/bz-library-page.blp`: place the card (twice)

**Why:** this is where the user sees the feature. Two placements are needed
because upstream swaps the **whole** content for a "No Apps Found" status
page when the user has no Flatpak apps. A Debian user with zero Flatpaks
must still reach system updates. Both instances share the
`bz_apt_updates_get_default ()` singleton, so they always show the same state.

**Change 1, normal content.** Anchor: the box
`Box non_search_content { orientation: vertical; visible: bind $is_empty_string(search_bar.text) as <bool>; … }`,
whose first child is the "Pending Updates" `Revealer` that contains
`$BzUpdatesCard`. Make the card the **first child**, before that `Revealer`:

```blueprint
                // DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md
                $BzAptUpdatesCard {}

```

Result: "System Updates" (APT) appears directly above "Pending Updates"
(Flatpak), hidden while searching (the box is hidden during search) and
hidden when PackageKit is unavailable (the card hides itself).

**Change 2, empty state.** Anchor: `Adw.ViewStackPage { name: "empty"; child: Adw.StatusPage { icon-name: "library-symbolic"; title: _("No Apps Found"); description: ""; } }`.
Give the `Adw.StatusPage` a child:

```blueprint

          // DOWNSTREAM PATCH (APT system updates): keep system updates reachable with zero Flatpak apps
          child: Adw.Clamp {
            maximum-size: 650;

            child: $BzAptUpdatesCard {};
          };
```

(`maximum-size: 650` matches the clamp upstream uses for the content page.)

**Connection:** `$BzAptUpdatesCard` → `patches/src/bz-apt-updates-card.{c,blp}`
→ `bz_apt_updates_get_default ()` in `patches/src/bz-apt-updates.c`.

**If the Library page was redesigned:** place one `$BzAptUpdatesCard {}`
wherever upstream now shows Flatpak "Pending Updates", and make sure one is
also reachable when there are zero Flatpak apps. Extra placements are
harmless (shared singleton; hidden without PackageKit). Remember H3 for the
widget whose template you edit.

---

## H5: `po/POTFILES.in`: make the new strings translatable

**Why:** gettext only extracts strings from files listed here. Without it
the UI works but is English-only.

**Change.** Anchor: the line `src/bz-apps-page.c` (the file is alphabetical).
Insert after it:

```text
# DOWNSTREAM PATCH (APT system updates)
src/bz-apt-updates.c
src/bz-apt-updates-card.blp
src/bz-apt-updates-card.c
src/bz-update-notifier.c
```

**Connection:** the `_()` / `ngettext ()` / Blueprint `_("…")` strings in
the three new files. The `.h` files hold no strings.

**If the anchor is gone:** insert the three lines in alphabetical position.

---

## H6: `build-aux/flatpak/io.github.kolunmi.Bazaar.json`: sandbox permissions

**Why:** inside the Flatpak sandbox, D-Bus names are blocked unless listed
in `finish-args`. `bz-apt-updates.c` calls PackageKit (system bus), logind
(system bus, reboot fallback) and GNOME SessionManager (session bus, reboot).
Native (non-Flatpak) builds do not need this.

**Change.** Anchor: the `"finish-args"` array; its last element at the
base commit is `"--system-talk-name=org.freedesktop.Accounts"`. Append
(add a comma to the previous last element):

```json
    "--system-talk-name=org.freedesktop.PackageKit",
    "--system-talk-name=org.freedesktop.login1",
    "--talk-name=org.gnome.SessionManager"
```

Validate:

```bash
python3 -m json.tool build-aux/flatpak/io.github.kolunmi.Bazaar.json >/dev/null && echo valid
```

**Connection:** the bus names in API.md §3.1 and §3.2. Without them, in a
sandbox, `available` stays FALSE and the card stays hidden (safe failure).

**If the manifest moved or was renamed:** apply to whatever Flatpak
manifest upstream uses (`git ls-files '*.json' '*.yml' '*.yaml' | grep -i flatpak`).

---

## H7: `src/update-worker.c`: run the notifier

**Why:** `run_update_worker ()` is upstream's hourly background job
(spawned by `bazaar-daemon` as `bazaar --run-update-worker`). It returns
at once when automatic updates are off, so the notifier must run before
that check.

**Change 1, include.** Anchor: `#include "update-worker.h"`. Insert after it:

```c
/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
#include "bz-update-notifier.h"
```

**Change 2, call.** Anchor: in `run_update_worker ()`, the line
`auto_update_notifications = g_settings_get_boolean (settings, "auto-update-notifications");`
followed by `if (!auto_update)`. Insert between them:

```c
  /* DOWNSTREAM PATCH (APT system updates): "updates available" notification
   * for APT, and for Flatpak when upstream does not auto-update it */
  bz_update_notifier_run (settings, auto_update);
```

**Connection:** `bz_update_notifier_run ()` in
`patches/src/bz-update-notifier.c`; `settings` is upstream's
`g_settings_new (APPLICATION_ID)`, `auto_update` its `auto-update` key.

**If the anchor is gone:** call `bz_update_notifier_run ()` once per worker
run, before any early return, with a `GSettings` for `APPLICATION_ID` and
whether upstream itself installs Flatpak updates in that run. If upstream
drops the worker, the notifier needs another periodic caller (PLAN.md §4).

---

## H8: `src/bazaar-daemon.c`: stay alive for notifications

**Why:** the daemon exits after 5 idle seconds unless `auto-update` is on
(`on_idle_timeout ()`), and only a living daemon runs the hourly worker.
The daemon is plain C (sd-event, libdbus, no GLib) and reads settings by
running `gsettings`, so the hook does the same for `update-notifications`.

**Change 1, helper.** Anchor: `static void cancel_idle_timer (void)`.
Insert before it a function `update_notifications_enabled ()`: a copy of
upstream's `auto_update_enabled ()` that runs
`gsettings get io.github.kolunmi.Bazaar update-notifications 2>/dev/null`
(see the unified diff for the exact code).

**Change 2, condition.** Anchor: in `on_idle_timeout ()`, the block ending
`log_msg ("Idle, but auto update is on so staying alive"); return 0; }`.
Insert after it:

```c
  /* DOWNSTREAM PATCH (APT system updates) */
  if (update_notifications_enabled ())
    {
      log_msg ("Idle, but update notifications are on so staying alive");
      return 0;
    }
```

**Connection:** key `update-notifications` from H10.

**If the anchor is gone:** wherever upstream decides that the daemon may
exit because automatic updates are off, also keep it alive while
`update-notifications` is true.

---

## H9: `src/bz-application.c`: add `app.open-library`

**Why:** the notification's click and button activate `app.open-library`
on `io.github.kolunmi.Bazaar`. Upstream has no app action that opens the
Library page (only the window action `window.open-library`).

**Change 1, include.** Anchor: `#include "bz-application.h"`. Insert after it:

```c
/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
#include "bz-update-notifier.h"
```

**Change 2, registration.** Anchor: in `bz_application_init ()`, the
`g_action_map_add_action_entries (G_ACTION_MAP (self), app_actions, G_N_ELEMENTS (app_actions), self);`
call. Insert after it:

```c
  /* DOWNSTREAM PATCH (APT system updates): app.open-library, used by the
   * "updates available" notification */
  bz_update_notifier_add_app_actions (G_APPLICATION (self));
```

**Connection:** `bz_update_notifier_add_app_actions ()` and
`open_library_action ()` in `patches/src/bz-update-notifier.c`, which use
upstream's `window.open-library` action and the `installed` page of the
window's `AdwViewStack`.

**If the anchor is gone:** register the action wherever upstream now adds
its app actions; it must exist before `g_application_run ()` handles
D-Bus `ActivateAction`.

---

## H10: `data/io.github.kolunmi.Bazaar.gschema.xml`: two keys

**Why:** an on/off switch for the notifications (also the autostart
condition, H11) and the time of the last notification (once a day).

**Change.** Anchor: the last `<key>` of the `io.github.kolunmi.Bazaar`
schema, `last-stale-notification`. Insert after its `</key>`:

```xml
    <!-- DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md -->
    <key name="update-notifications" type="b">
      <default>true</default>
      <summary>Update Notifications</summary>
      <description>Show a daily notification when app (Flatpak) or system (APT) updates are available. Also keeps Bazaar's background service running after login.</description>
    </key>
    <key name="last-update-notification" type="x">
      <default>0</default>
      <summary>timestamp of the last "updates available" notification</summary>
    </key>
```

**Connection:** read in `bz-update-notifier.c`, H8 and H11; written in
`bz-update-notifier.c`.

**If the anchor is gone:** add both keys to the schema with path
`/io/github/kolunmi/Bazaar/`; if upstream adds keys with these names, rename
ours (code: `bz-update-notifier.c`, H8, `patches/data/*.desktop`).

---

## H11: `data/meson.build`: install the system autostart file

**Why:** something must start `bazaar-daemon` at login. Upstream only
writes `~/.config/autostart/bazaar.desktop` when the user turns on automatic
updates in Preferences.

**Change.** Anchor: `subdir('icons')`. Insert before it:

```meson
# DOWNSTREAM PATCH (APT system updates): start bazaar-daemon at login while
# update notifications are enabled. Named bazaar.desktop like the per-user
# autostart file upstream writes for automatic updates, which overrides it.
install_data('bazaar-update-notifications.desktop',
  rename: 'bazaar.desktop',
  install_dir: get_option('sysconfdir') / 'xdg' / 'autostart'
)
```

**Connection:** `patches/data/bazaar-update-notifications.desktop` (runs
`bazaar-daemon --no-window`, with
`X-GNOME-Autostart-Condition=GSettings io.github.kolunmi.Bazaar update-notifications`).
With `--prefix=/usr` Meson's `sysconfdir` is `/etc`.

**If the anchor is gone:** install the file from any `meson.build` under
`data/`. If upstream renames its per-user autostart file
(`AUTOSTART_DESKTOP_FILE_NAME` in `src/bz-preferences-dialog.c`), rename
ours to match, or the daemon could start twice (the second start forwards
to the first, so this is harmless but wasteful).

---

## H12: `po/id.po`: Indonesian translations

**Why:** the patch's strings are new, so upstream's `po/id.po` has no
translation for them.

**Change:** not a diff. From the repository root:

```bash
python3 ai-patches/patches/po/merge-translations.py
```

It appends every entry of `patches/po/id-apt-updates.po` whose msgid
`po/id.po` does not already translate (entries with a `msgctxt` are
different messages and do not count), removes an obsolete `#~` entry with
the same msgid (gettext rejects the duplicate), and checks the result with
`msgfmt -c`. Existing translations are never changed, so upstream's win,
and running it again adds nothing. TEST.md T1 checks it was run.

**Connection:** the `_()`/`ngettext ()` strings of the four files listed in
H5. Adding a translatable string to the patch means adding its translation
to `patches/po/id-apt-updates.po`. A fragment for another language is a
file `patches/po/<lang>-apt-updates.po`; the script picks it up.

---

## H13: `src/main.c`: use GTK's GL renderer

**Why:** with GTK **4.23.4** (BlankOn sinambung's GTK, as of 2026-11-15),
GTK's default **Vulkan** renderer draws the Search page without text and
with blank category tiles. Upstream Bazaar has the same problem; it is a GTK
bug, not Bazaar's or this patch's. Diagnosed on 2026-11-14 (AMD Radeon
Cezanne, Wayland): the same render tree draws correctly with the GL and
Cairo renderers. It was seen again on 2026-11-15 in a VirtualBox VM running
the `warsi` package. The user confirmed `GSK_RENDERER=gl` fixes it.

**Anchor:** in `main ()`, the GUI branch (not the refresh/update worker
branches), right before `/* Init Bazaar GTK Extensions (see ../bge/) */`
and `bge_init ();`, which is before GTK initializes.

**Change:**

```c
      /* DOWNSTREAM PATCH (APT system updates): GTK renderer workaround, see
       * ai-patches/DIFF.md hook H13. GTK 4.23's default Vulkan renderer draws
       * the Search page without text and with blank tiles; the GL renderer
       * draws it correctly. Must run before GTK initializes. Setting
       * GSK_RENDERER yourself (e.g. GSK_RENDERER=vulkan) still wins. */
      if (g_getenv ("GSK_RENDERER") == NULL)
        g_setenv ("GSK_RENDERER", "gl", FALSE);
```

**Connection:** none to the new files. GTK reads `GSK_RENDERER` when it
creates a window's renderer; every way Bazaar starts (the menu, the daemon,
D-Bus activation, a notification click) runs `main ()` first.

**If the anchor moved:** put it anywhere in the GUI path before
`g_application_run ()`. **When to drop it:** once the distribution's GTK
draws the Search page correctly with `GSK_RENDERER=vulkan` (TEST.md T10).

---

## R: rebrand to Warsi (script, no diff)

**Why:** the user decided on 2026-11-15 to ship the patched Bazaar as Warsi
(`id.blankonlinux.warsi`). See PLAN.md §10.

**Change:** not a diff. After H1–H12, from the repository root:

```bash
python3 ai-patches/patches/rebrand/rebrand.py
```

`--check` exits 1 and lists the files it would still change. The script
changes only `meson.build`, `src/`, `data/` and `po/`:

| Step | Files | Change |
|---|---|---|
| rename | 17 files named `io.github.kolunmi.Bazaar.*` (data files, D-Bus services, icons, login icons) | `id.blankonlinux.warsi.*` |
| tokens | every text file | `io.github.kolunmi.Bazaar` → `id.blankonlinux.warsi`, `/io/github/kolunmi/Bazaar` → `/id/blankonlinux/warsi`, `bazaar-daemon` → `warsi-daemon` (not `bazaar-daemon.c`), `bazaar-dl-worker`, `bazaar-host-bwrap`, `bazaar-login-page` → `warsi-…`; translator comments name both brands |
| `meson.build` | project, gettext domain, programs | `project('warsi'`, `GETTEXT_PACKAGE 'warsi'`, `bazaar_bin_name = 'warsi'`, `$libdir/warsi`, `$datadir/warsi` |
| `po/meson.build` | gettext domain | `i18n.gettext('warsi'` |
| `src/meson.build`, `po/POTFILES.in` | new source | `bz-rebrand.c` after `bz-application.c` |
| `src/bz-application.c` | About dialog | `#include "bz-rebrand.h"`; `"application-name", "Warsi"`; `bz_rebrand_about_dialog (ADW_ABOUT_DIALOG (dialog));` before `adw_dialog_present`; the Flathub dialog names Warsi |
| `src/bz-window.blp`, `src/bz-window.c`, `src/shortcuts-dialog.blp`, `src/bz-bundle-install-dialog.blp`, `src/bz-preferences-dialog.c`, `src/bz-inspector.c` | strings naming the app | "Bazaar" → "Warsi" (window title, About/Quit menu items, "new version" banner, removal guard, shortcut, software-source dialog, background reason, inspector page) |
| `src/bazaar.gresource.xml`, `src/bz-preferences-dialog.c`, `src/bazaar-autostart.desktop` | per-user autostart | `warsi.desktop`, `Name=Warsi` |
| `data/meson.build`, `data/bazaar-update-notifications.desktop` | system autostart (H11) | installed as `warsi.desktop`, `Name=Warsi` |
| `data/id.blankonlinux.warsi.desktop.in` | desktop file | `Name=Warsi`; `Keywords` gains `Warung Aplikasi;Bazaar;GNOME Software;` (search words for Praya and GNOME Shell) |
| `data/id.blankonlinux.warsi.metainfo.xml.in` | AppStream | `<name>Warsi</name>`, gettext domain `warsi`, one more description paragraph |
| `po/*.po` | translations | the renamed msgids and their translations (PLAN.md §10.3 step 4); every translation of the desktop `Keywords` gets `Software;Store;Warung Aplikasi;Bazaar;GNOME Software;` appended; `po/id.po` also gets `patches/rebrand/po/id-rebrand.po` |
| new | `src/bz-rebrand.c`, `src/bz-rebrand.h`, the three app icons | from `patches/rebrand/src/` and `patches/rebrand/icons/` |
| removed | `data/icons/id.blankonlinux.warsi.Source.svg` | Bazaar's logo source; not installed |

Each edit names the exact upstream text it replaces (`EDITS` in the
script). When that text is missing, the script stops and names it: update
the edit to the new upstream text, and if the string is translated, the
matching entry in `RENAMED`.

**Connection:** `bz_rebrand_about_dialog ()` (`bz-rebrand.c`) is the only
new code. Everything else is renaming, which also renames the names the
APT hooks use (H8's `gsettings get`, H10's schema, H11's autostart file).

---

## Full unified diff (for `git apply`)

This is the exact `git diff` of the twelve files of hooks H1–H11 and H13 against
the base commit (H12 is the script above). It is
the only `diff`-fenced block in this file, so it can be extracted and applied
mechanically:

```bash
awk '/^```diff$/{f=1;next} /^```$/{f=0} f' ai-patches/DIFF.md > /tmp/apt-hooks.diff && git apply --3way /tmp/apt-hooks.diff
```

If `git apply` fails, apply hooks H1–H6 by hand as described above. The
diff does **not** include the new files; copy those from `patches/src/`
(PLAN.md §5.2).

```diff
diff --git a/build-aux/flatpak/io.github.kolunmi.Bazaar.json b/build-aux/flatpak/io.github.kolunmi.Bazaar.json
index c7ee7da1..1c005d1f 100644
--- a/build-aux/flatpak/io.github.kolunmi.Bazaar.json
+++ b/build-aux/flatpak/io.github.kolunmi.Bazaar.json
@@ -23,7 +23,10 @@
     "--system-talk-name=org.freedesktop.Flatpak.SystemHelper",
     "--filesystem=/var/tmp",
     "--filesystem=~/.var/app",
-    "--system-talk-name=org.freedesktop.Accounts"
+    "--system-talk-name=org.freedesktop.Accounts",
+    "--system-talk-name=org.freedesktop.PackageKit",
+    "--system-talk-name=org.freedesktop.login1",
+    "--talk-name=org.gnome.SessionManager"
   ],
   "cleanup": [
     "/share/zsh",
diff --git a/data/io.github.kolunmi.Bazaar.gschema.xml b/data/io.github.kolunmi.Bazaar.gschema.xml
index 62860bad..cf8ab2b6 100644
--- a/data/io.github.kolunmi.Bazaar.gschema.xml
+++ b/data/io.github.kolunmi.Bazaar.gschema.xml
@@ -94,5 +94,15 @@
       <default>0</default>
       <summary>timestamp of the last "updates out of date" notification</summary>
     </key>
+    <!-- DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md -->
+    <key name="update-notifications" type="b">
+      <default>true</default>
+      <summary>Update Notifications</summary>
+      <description>Show a daily notification when app (Flatpak) or system (APT) updates are available. Also keeps Bazaar's background service running after login.</description>
+    </key>
+    <key name="last-update-notification" type="x">
+      <default>0</default>
+      <summary>timestamp of the last "updates available" notification</summary>
+    </key>
   </schema>
 </schemalist>
diff --git a/data/meson.build b/data/meson.build
index 315bef7f..164c3011 100644
--- a/data/meson.build
+++ b/data/meson.build
@@ -51,5 +51,13 @@ configure_file(
   install_dir: get_option('datadir') / 'gnome-shell' / 'search-providers'
 )
 
+# DOWNSTREAM PATCH (APT system updates): start bazaar-daemon at login while
+# update notifications are enabled. Named bazaar.desktop like the per-user
+# autostart file upstream writes for automatic updates, which overrides it.
+install_data('bazaar-update-notifications.desktop',
+  rename: 'bazaar.desktop',
+  install_dir: get_option('sysconfdir') / 'xdg' / 'autostart'
+)
+
 subdir('icons')
 subdir('dbus')
diff --git a/po/POTFILES.in b/po/POTFILES.in
index 85f4751c..ab8ed999 100644
--- a/po/POTFILES.in
+++ b/po/POTFILES.in
@@ -19,6 +19,11 @@ src/bz-application-map-factory.c
 src/bz-application.c
 src/bz-apps-page.blp
 src/bz-apps-page.c
+# DOWNSTREAM PATCH (APT system updates)
+src/bz-apt-updates.c
+src/bz-apt-updates-card.blp
+src/bz-apt-updates-card.c
+src/bz-update-notifier.c
 src/bz-appstream-parser.c
 src/bz-article-list-view.c
 src/bz-article-tile.blp
diff --git a/src/bazaar-daemon.c b/src/bazaar-daemon.c
index 7c860b80..da2c2ae5 100644
--- a/src/bazaar-daemon.c
+++ b/src/bazaar-daemon.c
@@ -404,6 +404,28 @@ auto_update_enabled (void)
   return enabled;
 }
 
+/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md. The
+ * "updates available" notifications come from the hourly update worker, so
+ * the daemon has to stay alive for them too. */
+static int
+update_notifications_enabled (void)
+{
+  FILE *fp      = NULL;
+  char  buf[8]  = { 0 };
+  int   enabled = 0;
+
+  fp = popen ("gsettings get io.github.kolunmi.Bazaar update-notifications 2>/dev/null", "r");
+  if (fp == NULL)
+    return 0;
+
+  if (fgets (buf, sizeof (buf), fp) != NULL)
+    enabled = (strncmp (buf, "true", 4) == 0);
+
+  pclose (fp);
+
+  return enabled;
+}
+
 static void
 cancel_idle_timer (void)
 {
@@ -448,6 +470,13 @@ on_idle_timeout (sd_event_source *s,
       return 0;
     }
 
+  /* DOWNSTREAM PATCH (APT system updates) */
+  if (update_notifications_enabled ())
+    {
+      log_msg ("Idle, but update notifications are on so staying alive");
+      return 0;
+    }
+
   log_msg ("Idle timeout reached, closing down");
   sd_event_exit (event, 0);
   return 0;
diff --git a/src/bazaar.gresource.xml b/src/bazaar.gresource.xml
index 69589645..12390c65 100644
--- a/src/bazaar.gresource.xml
+++ b/src/bazaar.gresource.xml
@@ -26,6 +26,8 @@
     <file preprocess="xml-stripblanks">bz-app-size-dialog.ui</file>
     <file preprocess="xml-stripblanks">bz-app-tile.ui</file>
     <file preprocess="xml-stripblanks">bz-apps-page.ui</file>
+    <!-- DOWNSTREAM PATCH (APT system updates) -->
+    <file preprocess="xml-stripblanks">bz-apt-updates-card.ui</file>
     <file preprocess="xml-stripblanks">bz-appstream-description-render.ui</file>
     <file preprocess="xml-stripblanks">bz-article.ui</file>
     <file preprocess="xml-stripblanks">bz-article-tile.ui</file>
diff --git a/src/bz-application.c b/src/bz-application.c
index dc185979..84f497d4 100644
--- a/src/bz-application.c
+++ b/src/bz-application.c
@@ -35,6 +35,8 @@
 
 #include "bz-application-map-factory.h"
 #include "bz-application.h"
+/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
+#include "bz-update-notifier.h"
 #include "bz-appstream-parser.h"
 #include "bz-auth-state.h"
 #include <bzvala.h>
@@ -1069,6 +1071,9 @@ bz_application_init (BzApplication *self)
       app_actions,
       G_N_ELEMENTS (app_actions),
       self);
+  /* DOWNSTREAM PATCH (APT system updates): app.open-library, used by the
+   * "updates available" notification */
+  bz_update_notifier_add_app_actions (G_APPLICATION (self));
   gtk_application_set_accels_for_action (
       GTK_APPLICATION (self),
       "app.quit",
diff --git a/src/bz-library-page.blp b/src/bz-library-page.blp
index 92475aea..2d27602c 100644
--- a/src/bz-library-page.blp
+++ b/src/bz-library-page.blp
@@ -31,6 +31,13 @@ template $BzLibraryPage: Adw.Bin {
           icon-name: "library-symbolic";
           title: _("No Apps Found");
           description: "";
+
+          // DOWNSTREAM PATCH (APT system updates): keep system updates reachable with zero Flatpak apps
+          child: Adw.Clamp {
+            maximum-size: 650;
+
+            child: $BzAptUpdatesCard {};
+          };
         };
       }
 
@@ -73,6 +80,9 @@ template $BzLibraryPage: Adw.Bin {
                 orientation: vertical;
                 visible: bind $is_empty_string(search_bar.text) as <bool>;
 
+                // DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md
+                $BzAptUpdatesCard {}
+
                 Revealer {
                   reveal-child: bind $invert_boolean($is_empty(template.state as <$BzStateInfo>.available-updates) as <bool>) as <bool>;
                   transition-type: slide_down;
diff --git a/src/bz-library-page.c b/src/bz-library-page.c
index 904e6e49..01ace80d 100644
--- a/src/bz-library-page.c
+++ b/src/bz-library-page.c
@@ -21,6 +21,8 @@
 #include <glib/gi18n.h>
 #include <bzvala.h>
 
+/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
+#include "bz-apt-updates-card.h"
 #include "bz-entry-group.h"
 #include "bz-installed-tile.h"
 #include "bz-library-page.h"
@@ -442,6 +444,8 @@ bz_library_page_class_init (BzLibraryPageClass *klass)
   g_type_ensure (BZ_TYPE_INSTALLED_TILE);
   g_type_ensure (BZ_TYPE_TRANSACTION_TILE);
   g_type_ensure (BZ_TYPE_UPDATES_CARD);
+  /* DOWNSTREAM PATCH (APT system updates) */
+  g_type_ensure (BZ_TYPE_APT_UPDATES_CARD);
 
   gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kolunmi/Bazaar/bz-library-page.ui");
   bz_widget_class_bind_all_util_callbacks (widget_class);
diff --git a/src/meson.build b/src/meson.build
index 7cbd7f24..d971af49 100644
--- a/src/meson.build
+++ b/src/meson.build
@@ -67,6 +67,11 @@ bz_sources = files(
   'bz-application-map-factory.c',
   'bz-application.c',
   'bz-apps-page.c',
+  # DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md
+  'bz-apt-updates.c',
+  'bz-apt-updates-card.c',
+  # DOWNSTREAM PATCH (APT system updates): notifications
+  'bz-update-notifier.c',
   'bz-appstream-description-render.c',
   'bz-appstream-parser.c',
   'bz-article-list-view.c',
@@ -201,6 +206,8 @@ blueprints = custom_target('blueprints',
     'bz-app-size-dialog.blp',
     'bz-app-tile.blp',
     'bz-apps-page.blp',
+    # DOWNSTREAM PATCH (APT system updates)
+    'bz-apt-updates-card.blp',
     'bz-appstream-description-render.blp',
     'bz-article.blp',
     'bz-article-tile.blp',
diff --git a/src/update-worker.c b/src/update-worker.c
index b8c798c1..0788843c 100644
--- a/src/update-worker.c
+++ b/src/update-worker.c
@@ -30,6 +30,8 @@
 #include <bzvala.h>
 #include "bz-flatpak-private.h"
 #include "update-worker.h"
+/* DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md */
+#include "bz-update-notifier.h"
 
 #define DAILY_WINDOW_START_HOUR    6
 #define DAILY_WINDOW_SPREAD_HOURS  6
@@ -78,6 +80,10 @@ run_update_worker (int   argc,
   auto_update               = g_settings_get_boolean (settings, "auto-update");
   auto_update_notifications = g_settings_get_boolean (settings, "auto-update-notifications");
 
+  /* DOWNSTREAM PATCH (APT system updates): "updates available" notification
+   * for APT, and for Flatpak when upstream does not auto-update it */
+  bz_update_notifier_run (settings, auto_update);
+
   if (!auto_update)
     {
       g_debug ("Skipping update check: auto-update is disabled\n");
diff --git a/src/main.c b/src/main.c
--- a/src/main.c
+++ b/src/main.c
@@ -56,6 +56,14 @@
     {
       g_autoptr (BzApplication) app = NULL;
 
+      /* DOWNSTREAM PATCH (APT system updates): GTK renderer workaround, see
+       * ai-patches/DIFF.md hook H13. GTK 4.23's default Vulkan renderer draws
+       * the Search page without text and with blank tiles; the GL renderer
+       * draws it correctly. Must run before GTK initializes. Setting
+       * GSK_RENDERER yourself (e.g. GSK_RENDERER=vulkan) still wins. */
+      if (g_getenv ("GSK_RENDERER") == NULL)
+        g_setenv ("GSK_RENDERER", "gl", FALSE);
+
       /* Init Bazaar GTK Extensions (see ../bge/) */
       bge_init ();
 
```
