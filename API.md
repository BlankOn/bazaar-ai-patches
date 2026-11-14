# API: everything the APT system-updates patch depends on

This file lists **every external interface** the patch relies on, so a
future attempt can check, before applying, whether upstream Bazaar or the
platform changed any of them. Read [PLAN.md](PLAN.md) first for context.

---

## 1. Base: the codebase the patch was written against

| Item | Value |
|---|---|
| Upstream repository | `https://gitlab.gnome.org/World/bazaar` (`git@ssh.gitlab.gnome.org:World/bazaar.git`) |
| Branch | `main` |
| **Base commit** | **`9560ad18cd60652aae10e81708ff4d36df775aa8`** |
| Commit date | 2026-11-14 13:03:56 +0000 |
| Commit subject | `Merge branch 'update-translation-fi-from-main-831b4fbe-…' into 'main'` |
| `git describe` | `v0.10.0-29-g9560ad18` |
| Bazaar version (`version.sh get-version`) | `0.10.1` |
| Patch written / verified | 2026-11-14 |

Platform versions on the verification machine (BlankOn, Debian-based):

| Component | Version |
|---|---|
| GLib / GIO | 2.89.4 |
| GTK 4 | 4.23.4 (upstream requires ≥ 4.22.1) |
| libadwaita | 1.9.2 (upstream requires ≥ 1.8) |
| blueprint-compiler | 0.20.4 |
| PackageKit | 1.3.6, backend `apt` ("APT") |
| Meson / Ninja / GCC | 1.12.0 / 1.13.2 / 16.2.0 |

---

## 2. Stability check procedure

Run these against the **target** tree (the newer upstream checkout) before
applying. Each check names the section below that explains it.

### 2.1 What changed upstream since the base commit (Bazaar-internal APIs, §3.6)

```bash
git fetch origin && git log --oneline 9560ad18cd60652aae10e81708ff4d36df775aa8..origin/main -- src/bz-library-page.c src/bz-library-page.blp src/meson.build src/bazaar.gresource.xml po/POTFILES.in build-aux/flatpak/io.github.kolunmi.Bazaar.json
```

```bash
git diff 9560ad18cd60652aae10e81708ff4d36df775aa8 origin/main -- src/bz-library-page.c src/bz-library-page.blp src/meson.build src/bazaar.gresource.xml po/POTFILES.in build-aux/flatpak/io.github.kolunmi.Bazaar.json
```

Empty output means every hook in DIFF.md applies unchanged. Otherwise read
the diff with DIFF.md open and confirm each hook's anchor still exists.

Also check the conventions the new files rely on:

```bash
grep -n "GETTEXT_PACKAGE" meson.build src/meson.build; grep -n 'prefix="/io/github/kolunmi/Bazaar' src/bazaar.gresource.xml; grep -n "software-update-available-symbolic\|user-trash-symbolic" src/bazaar.gresource.xml; grep -n "dependency('libadwaita-1'\|dependency('gtk4'" src/meson.build
```

Expected: `GETTEXT_PACKAGE` is defined into `config.h`, the GResource prefix
is unchanged, both icons are still bundled, and libadwaita is still ≥ 1.5.

### 2.2 PackageKit D-Bus API (§3.1)

On any machine with PackageKit:

```bash
busctl --system introspect org.freedesktop.PackageKit /org/freedesktop/PackageKit
```

```bash
python3 -c "import gi; gi.require_version('PackageKitGlib','1.0'); from gi.repository import PackageKitGlib as pk; print(int(pk.InfoEnum.SECURITY), int(pk.InfoEnum.BLOCKED), int(pk.InfoEnum.CRITICAL), int(pk.InfoEnum.INSTALL), int(pk.InfoEnum.REMOVE), int(pk.InfoEnum.OBSOLETE), int(pk.RoleEnum.REFRESH_CACHE), pk.filter_bitfield_from_string('none'), pk.transaction_flag_bitfield_from_string('only-trusted'), pk.transaction_flag_bitfield_from_string('only-trusted;only-download'), int(pk.ExitEnum.SUCCESS), int(pk.ExitEnum.CANCELLED), int(pk.ExitEnum.NEED_UNTRUSTED), int(pk.RestartEnum.SYSTEM))"
```

Expected output: `8 9 26 27 28 29 13 2 2 10 1 3 8 4`. (Needs `gir1.2-packagekitglib-1.0`.)
Any difference means the constants at the top of
`patches/src/bz-apt-updates.c` must be updated.

To inspect a transaction object's interface, create one (harmless; it
expires unused):

```bash
busctl --system introspect org.freedesktop.PackageKit "$(busctl --system call org.freedesktop.PackageKit /org/freedesktop/PackageKit org.freedesktop.PackageKit CreateTransaction | awk '{print $2}' | tr -d '"')"
```

### 2.3 Toolkit APIs (§3.3–§3.5)

The fastest and most complete check is to compile the patch sources
standalone with `-Werror` against the target's GTK/libadwaita:

```bash
BZ_APT_SRC=ai-patches/patches/src ai-patches/patches/harness/build.sh
```

A removed or renamed symbol fails to compile; a deprecated one fails because
of `-Werror` (deprecation warnings). For libadwaita availability macros:

```bash
grep -h -B3 -E "adw_alert_dialog_choose|adw_preferences_row_set_use_markup|adw_action_row_set_subtitle_selectable" /usr/include/libadwaita-1/*.h | grep -oE "ADW_(AVAILABLE_IN_[0-9_]+|DEPRECATED[A-Z_0-9]*)"
```

### 2.4 Blueprint syntax (§3.7)

`blueprint-compiler compile` on `patches/src/bz-apt-updates-card.blp` and on
the hooked `src/bz-library-page.blp` must succeed (the harness build does the
first; the full build does both).

---

## 3. API inventory

### 3.1 PackageKit D-Bus API (system bus): the core dependency

Well-known name `org.freedesktop.PackageKit`, object
`/org/freedesktop/PackageKit`. Stability: PackageKit's D-Bus API has been
stable since 1.0 (2014); enums are append-only. Verified on 1.3.6.

**Interface `org.freedesktop.PackageKit`** (object `/org/freedesktop/PackageKit`)

| Member | Kind | Signature | Used in `bz-apt-updates.c` for |
|---|---|---|---|
| `BackendName` | property | `s` | availability check (`""`/`"dummy"` → unavailable) |
| `BackendDescription` | property | `s` | label "Provided by %s" (e.g. `"APT"`) |
| `CreateTransaction` | method | `() → o` | start of every operation |
| `GetTimeSinceAction` | method | `(u role) → u seconds` | daily auto refresh; role 13 = refresh-cache; `G_MAXUINT32` = never |
| `UpdatesChanged` | signal | `()` | queue a background `GetUpdates` |

Read via `org.freedesktop.DBus.Properties.GetAll ("org.freedesktop.PackageKit")`;
this call also D-Bus-activates the daemon.

**Interface `org.freedesktop.PackageKit.Transaction`** (object path from `CreateTransaction`)

| Member | Kind | Signature | Use |
|---|---|---|---|
| `SetHints` | method | `(as)` | `interactive=true\|false`, `background=true\|false`, `locale=<lang>` |
| `GetUpdates` | method | `(t filter)` | list updates; filter `none` = `1<<1` = **2** |
| `RefreshCache` | method | `(b force)` | `apt update`; called with `false` |
| `UpdatePackages` | method | `(t transaction_flags, as package_ids)` | `only-trusted` = **2** (live); `only-trusted\|only-download` = **10** (offline prepare) |
| `Cancel` | method | `()` | cancel button |
| `Package` | signal | `(u info, s package_id, s summary)` | one result |
| `Packages` | signal | `(a(uss))` | batched results (PackageKit ≥ 1.3); de-duplicated with `Package` |
| `ErrorCode` | signal | `(u code, s details)` | `details` shown to the user |
| `RequireRestart` | signal | `(u type, s package_id)` | "Restart Required" row |
| `Finished` | signal | `(u exit, u runtime_ms)` | end of transaction |
| `Destroy` | signal | `()` | transaction object gone; failure if no `Finished` seen |
| `Percentage` | property (via `PropertiesChanged`) | `u` | progress; **101 = unknown** |
| `Status` | property (via `PropertiesChanged`) | `u` | status text |
| `AllowCancel` | property (via `PropertiesChanged`) | `b` | Cancel button sensitivity |

`package_id` format: `name;version;arch;data` (e.g.
`apache2;2.4.71-1;amd64;blankon-sinambung-main`). The patch shows `name` and
`version`.

**Interface `org.freedesktop.PackageKit.Offline`** (object `/org/freedesktop/PackageKit`)

| Member | Kind | Signature | Use |
|---|---|---|---|
| `Trigger` | method | `(s action)` | after a successful download-only update; action `"reboot"` |
| `Cancel` | method | `()` | "Cancel Update" on the ready row |
| `UpdateTriggered` | property | `b` | "Updates Ready to Install" row (initial `GetAll` + `PropertiesChanged`) |

**Enum values hard-coded in `bz-apt-updates.c`** (from PackageKit `pk-enum.h`;
bitfields are `1 << value`):

| Constant in patch | Value | PackageKit enum |
|---|---|---|
| `PK_FILTER_NONE` | `1<<1` = 2 | `PK_FILTER_ENUM_NONE` = 1 |
| `PK_TRANSACTION_FLAG_ONLY_TRUSTED` | `1<<1` = 2 | `PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED` = 1 |
| `PK_TRANSACTION_FLAG_ONLY_DOWNLOAD` | `1<<3` = 8 | `PK_TRANSACTION_FLAG_ENUM_ONLY_DOWNLOAD` = 3 |
| `PK_ROLE_REFRESH_CACHE` | 13 | `PK_ROLE_ENUM_REFRESH_CACHE` |
| `PK_INFO_SECURITY` | 8 | `PK_INFO_ENUM_SECURITY` |
| `PK_INFO_BLOCKED` | 9 | `PK_INFO_ENUM_BLOCKED` (excluded from the list) |
| `PK_INFO_CRITICAL` | 26 | `PK_INFO_ENUM_CRITICAL` (counted as security) |
| `PK_INFO_INSTALL` | 27 | `PK_INFO_ENUM_INSTALL`: `GetUpdates` lists a package the upgrade newly installs (counted as "new", not as an update) |
| `PK_INFO_REMOVE` | 28 | `PK_INFO_ENUM_REMOVE`: `GetUpdates` lists a package the upgrade removes (counted as a removal) |
| `PK_INFO_OBSOLETE` | 29 | `PK_INFO_ENUM_OBSOLETE` (treated as a removal) |
| `PK_EXIT_SUCCESS` / `FAILED` / `CANCELLED` | 1 / 2 / 3 | `PK_EXIT_ENUM_*` |
| `PK_EXIT_NEED_UNTRUSTED` | 8 | `PK_EXIT_ENUM_NEED_UNTRUSTED` |
| `PK_EXIT_CANCELLED_PRIORITY` | 9 | `PK_EXIT_ENUM_CANCELLED_PRIORITY` |
| `PK_RESTART_SYSTEM` / `SECURITY_SYSTEM` | 4 / 6 | `PK_RESTART_ENUM_*` |
| `PK_PERCENTAGE_UNKNOWN` | 101 | documented sentinel |

`status_to_string ()` maps `PkStatusEnum` values: 1 wait, 2 setup,
3 running, 4 query, 5 info, 7 refresh-cache, 8 download, 9 install,
10 update, 11 cleanup, 13 dep-resolve, 14 sig-check, 15 test-commit,
16 commit, 18 finished, 19 cancel, 20–25 download-repository/packagelist/
filelist/changelog/group/updateinfo, 27 loading-cache, 30 waiting-for-lock,
31 waiting-for-auth, 36 run-hook. Unknown values keep the previous text, so
new enum values are harmless.

**polkit actions involved** (shipped by `packagekit`, file
`/usr/share/polkit-1/actions/org.freedesktop.packagekit.policy`):
`org.freedesktop.packagekit.system-sources-refresh` (refresh),
`org.freedesktop.packagekit.system-update` (update),
`org.freedesktop.packagekit.trigger-offline-update` (Trigger),
`org.freedesktop.packagekit.clear-offline-update` (Offline.Cancel),
`org.freedesktop.packagekit.cancel-foreign`.

**systemd unit:** `packagekit-offline-update.service` installs the prepared
update at boot. Check with `systemctl cat packagekit-offline-update.service`.

### 3.2 Reboot APIs

| Bus | Name / object / interface | Method | Notes |
|---|---|---|---|
| session | `org.gnome.SessionManager` `/org/gnome/SessionManager` `org.gnome.SessionManager` | `Reboot ()` | tried first; GNOME shows its own confirmation dialog |
| system | `org.freedesktop.login1` `/org/freedesktop/login1` `org.freedesktop.login1.Manager` | `Reboot (b interactive=true)` | fallback; called with `G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION` |

### 3.3 GLib / GIO (no explicit minimum in upstream; GTK ≥ 4.22 implies a recent GLib)

Used in `bz-apt-updates.c` / `bz-apt-updates-card.c`; the newest
requirement is GLib **2.70** (`G_DEFINE_FINAL_TYPE`).

| API | Since | Notes |
|---|---|---|
| `G_DECLARE_FINAL_TYPE`, `G_DEFINE_FINAL_TYPE` | 2.44 / 2.70 | type boilerplate |
| `g_bus_get`, `g_bus_get_finish`, `g_bus_get_sync` | 2.26 | system and session bus |
| `g_dbus_connection_call`, `g_dbus_connection_call_finish` | 2.26 | all D-Bus calls |
| `G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION` | 2.46 | polkit prompts for user actions |
| `g_dbus_connection_signal_subscribe` / `_unsubscribe` | 2.26 | sender `NULL`, matched by path + interface |
| `g_dbus_error_strip_remote_error` | 2.26 | readable error text |
| `g_variant_new`, `g_variant_get`, `g_variant_lookup`, `g_variant_iter_next`, `GVariantBuilder` (`init/add/end`) | 2.24–2.28 | `g_variant_new ("(as)", &builder)` form |
| `g_list_store_new`, `g_list_store_splice`, `g_list_model_get_item/n_items` | 2.44 | package list model |
| `g_param_spec_*`, `g_object_class_install_properties`, `g_object_notify_by_pspec` | 2.26 | properties |
| `g_clear_handle_id` | 2.56 | |
| `g_hash_table_add` (returns `gboolean`) | 2.32 (bool return 2.40) | de-dup |
| `g_queue_*`, `g_ptr_array_*`, `g_atomic_int_inc/dec_and_test`, `g_get_language_names`, `g_strsplit`, `g_strdup_printf`, `g_timeout_add` | old | |
| `GSimpleActionGroup`, `g_action_map_add_action_entries`, `g_simple_action_set_enabled` | 2.32 | card actions |
| `ngettext`, `_()` via `<glib/gi18n.h>` + `GETTEXT_PACKAGE` | n/a | translations |

### 3.4 GTK 4

| API | Notes |
|---|---|
| `gtk_widget_class_set_template_from_resource`, `gtk_widget_class_bind_template_child`, `gtk_widget_class_bind_template_callback`, `gtk_widget_init_template` | composite template |
| `gtk_widget_insert_action_group` | action group `apt-updates` |
| `gtk_widget_set_visible/sensitive/tooltip_text`, `gtk_widget_add_css_class` | |
| `gtk_image_new_from_icon_name` | security icon `security-low-symbolic` (Adwaita icon theme); removal icon `user-trash-symbolic` (bundled by upstream in `src/icons`) |
| `GtkProgressBar`: `gtk_progress_bar_set_fraction`, `gtk_progress_bar_pulse` | progress row |
| `GtkListBox` with style `boxed-list`, `GtkLabel` with styles `heading`, `h4` | same styling as upstream "Pending Updates" |
| Harness only: `gtk_widget_paintable_new`, `gdk_paintable_snapshot`, `gtk_snapshot_free_to_node`, `gsk_renderer_render_texture`, `gdk_texture_save_to_png`, `gtk_window_list_toplevels`, `gtk_widget_activate_action` | screenshots (`harness.c`, `app-shot.c`) |
| `app-shot.c` only: `gsk_cairo_renderer_new`, `gsk_renderer_realize_for_display`, `gsk_renderer_unrealize` | the window's own GPU renderer rendered only the background of Bazaar's window offscreen |

### 3.5 libadwaita (patch needs ≥ 1.5; upstream requires ≥ 1.8)

| API | Since |
|---|---|
| `AdwBin` (parent class of the card) | 1.0 |
| `AdwActionRow`: `adw_action_row_new`, `adw_action_row_add_suffix`, `adw_action_row_set_subtitle` | 1.0 |
| `AdwActionRow:subtitle-selectable` (blueprint, error row) | 1.3 |
| `AdwPreferencesRow`: `adw_preferences_row_set_title` | 1.0 |
| `adw_preferences_row_set_use_markup` (`use-markup: false` everywhere package text is shown) | 1.2 |
| `AdwExpanderRow`: `adw_expander_row_add_row`, `adw_expander_row_remove`, `adw_expander_row_set_subtitle`, `adw_expander_row_get/set_enable_expansion`, `adw_expander_row_set_expanded` | 1.0 |
| `AdwSplitButton` (`label`, `use-underline`, `menu-model`, `clicked`) | 1.0 |
| `AdwAlertDialog`: `adw_alert_dialog_new`, `add_responses`, `set_response_appearance`, `set_default_response`, `set_close_response`, `choose`, `choose_finish`; `ADW_RESPONSE_SUGGESTED` | **1.5** (binding minimum) |
| `AdwClamp` (hook H4, `maximum-size`) | 1.0 |
| `AdwStatusPage:child` (hook H4) | 1.0 |

**Behaviour the code depends on:** `adw_expander_row_set_enable_expansion (TRUE)`
also *expands* the row. `sync_ui ()` therefore calls
`adw_expander_row_set_expanded (FALSE)` right after enabling, and only when
the value actually changes. If libadwaita changes this, the workaround is
harmless.

### 3.6 Bazaar-internal APIs and conventions (the most likely to change)

| Item | Where (base commit) | Relied on by | Check |
|---|---|---|---|
| `config.h` defines `GETTEXT_PACKAGE` | generated by `meson.build` | both new `.c` files (`#include "config.h"`, `_()`) | §2.1 grep |
| GResource prefix `/io/github/kolunmi/Bazaar/` (base: `<gresource prefix="/io/github/kolunmi/Bazaar/">`, line 3) | `src/bazaar.gresource.xml` | `gtk_widget_class_set_template_from_resource (…"/io/github/kolunmi/Bazaar/bz-apt-updates-card.ui")` | §2.1 grep |
| Blueprint `.blp` → `.ui` via `custom_target('blueprints', …)` input list | `src/meson.build` | hook H1 | DIFF.md H1 |
| C sources list `bz_sources = files(…)` | `src/meson.build` | hook H1 | DIFF.md H1 |
| Icon `software-update-available-symbolic` bundled at `icons/scalable/actions/` | `src/bazaar.gresource.xml` line ~149 | card expander prefix icon | §2.1 grep; fallback: any Adwaita icon |
| `BzLibraryPage` template with `Box non_search_content` holding the "Pending Updates" `Revealer` + `$BzUpdatesCard` | `src/bz-library-page.blp` | hook H4 placement | DIFF.md H4 |
| `BzLibraryPage` `Adw.ViewStackPage { name: "empty"; child: Adw.StatusPage { title: _("No Apps Found") } }` | `src/bz-library-page.blp` | hook H4 second placement | DIFF.md H4 |
| `bz_library_page_class_init` calls `g_type_ensure (BZ_TYPE_UPDATES_CARD)` before `gtk_widget_class_set_template_from_resource` | `src/bz-library-page.c` | hook H3 | DIFF.md H3 |
| Template callbacks receive the template object only when the Blueprint handler is written `$cb(template)` (swapped) | Blueprint/GtkBuilder convention, used across upstream | all `clicked => $…(template)` in the card | compile + click test |
| `po/POTFILES.in` alphabetical list | `po/POTFILES.in` | hook H5 | DIFF.md H5 |
| Flatpak manifest `finish-args` | `build-aux/flatpak/io.github.kolunmi.Bazaar.json` | hook H6 | DIFF.md H6 |
| Action `window.open-library` (Ctrl+D) | `src/bz-window.c` | **testing only** (screenshot shim) | `grep -n open-library src/bz-window.c` |
| Upstream build/run quirks: refresh worker spawned as `bazaar` from `PATH`; `scripts/countries.py` needs `python3-babel` | `src/bz-application.c`, `src/meson.build` | **verification only** | TEST.md §0, T6 |

The patch does **not** use `BzBackend`, `BzEntry`, `BzStateInfo`,
`BzTransactionManager`, libdex, libflatpak or any Vala-generated API. Changes
there do not affect it.

### 3.7 Blueprint language features used

`using Gtk 4.0; using Adw 1;`, `template $BzAptUpdatesCard: Adw.Bin`,
extern types `$BzAptUpdatesCard {}` in the Library page, `[prefix]` /
`[suffix]` child types, `styles [...]`, `_("…")` translatable strings,
signal handlers `clicked => $name(template);`, and a top-level
`menu update_menu { section { item { label: …; action: "apt-updates.…"; } } }`.
Verified with blueprint-compiler 0.20.4.
