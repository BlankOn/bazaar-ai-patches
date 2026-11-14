# DIFF: changes to existing upstream files

This file documents **every change the patch makes to files that already
exist upstream**: what changes, why it is needed, and how it connects to the
entirely new files in [`patches/`](patches/). Read [PLAN.md](PLAN.md) first.

- Base commit: `9560ad18cd60652aae10e81708ff4d36df775aa8` (see [API.md §1](API.md#1-base-the-codebase-the-patch-was-written-against)).
- Upstream files touched: **6**, about 30 added lines, 1 changed line, nothing deleted.
- Every hook except the JSON manifest carries the marker
  `DOWNSTREAM PATCH (APT system updates)`; find them with
  `git grep -n "DOWNSTREAM PATCH (APT system updates)"`.

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
 po/POTFILES.in ──────────(H5)──► translatable strings of the 3 new UI/C files
 build-aux/flatpak/…json ─(H6)──► D-Bus permissions used by bz-apt-updates.c at runtime
```

| Hook | Upstream file | New file / symbol it connects to |
|---|---|---|
| H1 | `src/meson.build` | `bz-apt-updates.c`, `bz-apt-updates-card.c`, `bz-apt-updates-card.blp` |
| H2 | `src/bazaar.gresource.xml` | `bz-apt-updates-card.ui` (compiled from the `.blp`) |
| H3 | `src/bz-library-page.c` | `bz-apt-updates-card.h` → `BZ_TYPE_APT_UPDATES_CARD` |
| H4 | `src/bz-library-page.blp` | `BzAptUpdatesCard` widget type |
| H5 | `po/POTFILES.in` | `_()` strings in the three files |
| H6 | `build-aux/flatpak/io.github.kolunmi.Bazaar.json` | D-Bus names called by `bz-apt-updates.c` (see API.md §3.1–§3.2) |

Apply order: H1 → H6. H1–H4 are required for the feature to work; H5 is
needed for translations; H6 only matters for Flatpak builds.

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
```

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

## Full unified diff (for `git apply`)

This is the exact `git diff` of the six files against the base commit. It is
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
diff --git a/po/POTFILES.in b/po/POTFILES.in
index 85f4751c..845468da 100644
--- a/po/POTFILES.in
+++ b/po/POTFILES.in
@@ -19,6 +19,10 @@ src/bz-application-map-factory.c
 src/bz-application.c
 src/bz-apps-page.blp
 src/bz-apps-page.c
+# DOWNSTREAM PATCH (APT system updates)
+src/bz-apt-updates.c
+src/bz-apt-updates-card.blp
+src/bz-apt-updates-card.c
 src/bz-appstream-parser.c
 src/bz-article-list-view.c
 src/bz-article-tile.blp
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
index 7cbd7f24..e8410dfe 100644
--- a/src/meson.build
+++ b/src/meson.build
@@ -67,6 +67,9 @@ bz_sources = files(
   'bz-application-map-factory.c',
   'bz-application.c',
   'bz-apps-page.c',
+  # DOWNSTREAM PATCH (APT system updates): see ai-patches/DIFF.md
+  'bz-apt-updates.c',
+  'bz-apt-updates-card.c',
   'bz-appstream-description-render.c',
   'bz-appstream-parser.c',
   'bz-article-list-view.c',
@@ -201,6 +204,8 @@ blueprints = custom_target('blueprints',
     'bz-app-size-dialog.blp',
     'bz-app-tile.blp',
     'bz-apps-page.blp',
+    # DOWNSTREAM PATCH (APT system updates)
+    'bz-apt-updates-card.blp',
     'bz-appstream-description-render.blp',
     'bz-article.blp',
     'bz-article-tile.blp',
```
