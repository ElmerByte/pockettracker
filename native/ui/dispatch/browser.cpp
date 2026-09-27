// The file browser — opening, the cursor, loading, the file clipboard — and the QWERTY keyboard.

#include "ui/dispatch/dispatch_common.h"

#include "ui/groove_io.h"
#include "ui/scale_io.h"
#include "ui/std_filesystem.h"   // path_name / path_stem / path_extension / to_lower
#include "ui/theme_io.h"
#include "load_progress.h"       // load_cancelled

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── Opening and closing the browser ─────────────────────────────────────────────────────────────

std::string InputDispatcher::browser_dir(BrowserDir cat) {
    // A config.json override per category, else the built-in default. The FileSystem getters
    // create-on-first-use, so `def` is always a real, readable directory — which is what makes it a
    // safe answer whenever the override cannot be honoured.
    //
    // No debug gate: config.json ships on every platform's release from v0.9.4 (`ui/folder_config.h`).
    const std::optional<std::string>* ov = nullptr;
    std::string                       def;
    switch (cat) {
        case BrowserDir::SAMPLES:     ov = &s_.folderConfig.samples;     def = fs_.samples_directory();     break;
        case BrowserDir::SOUNDFONTS:  ov = &s_.folderConfig.soundfonts;  def = fs_.soundfonts_directory();  break;
        case BrowserDir::INSTRUMENTS: ov = &s_.folderConfig.instruments; def = fs_.instruments_directory(); break;
        case BrowserDir::PROJECTS:    ov = &s_.folderConfig.projects;    def = fs_.projects_directory();    break;
        case BrowserDir::THEMES:      ov = &s_.folderConfig.themes;      def = fs_.themes_directory();      break;
    }
    // ⚠️ The whole rule lives in `resolve_browse_dir`, NOT here: the value is root-relative unless it is
    // absolute, an override authored under another install's root is re-rooted onto ours, and one that
    // cannot be read on this platform falls back to `def`. Inlining any part of that here is how the
    // config and a project's sample paths would start disagreeing about where the app's folders are.
    return ov ? resolve_browse_dir(fs_, *ov, def) : def;
}

void InputDispatcher::open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                                        const std::vector<std::string>& extensions) {
    s_.previousScreen = s_.currentScreen;
    s_.browserPurpose = purpose;

    s_.fileBrowser.fileExtensions = extensions;
    s_.fileBrowser.mode           = BrowserMode::NORMAL;

    // D2a: for a SAMPLE load with FOLDER = REMEMBER, start at the folder the last sample came from.
    // Keyed off the requested start being the samples dir — which is EXACTLY the two sample-load
    // purposes (LOAD_SOURCE on a sampler, LOAD_SAMPLE_EDITOR) and nothing else, so soundfonts, presets,
    // projects and themes keep their own directories. Falls back to `directory` if the remembered path
    // is empty or gone (a deleted folder must not strand the browser on an empty listing).
    std::string start = directory;
    if (s_.settings.rememberFolder && directory == browser_dir(BrowserDir::SAMPLES) &&
        !s_.settings.lastSampleFolder.empty() && fs_.is_directory(s_.settings.lastSampleFolder)) {
        start = s_.settings.lastSampleFolder;
    }
    navigate_to_folder(s_.fileBrowser, fs_, start);

    s_.currentScreen = ScreenType::FILE_BROWSER;
}

void InputDispatcher::close_file_browser() {
    // The audition the user was scrolling through is over. Kotlin frees slot 255 here for the same
    // reason: a preview left resident is a megabyte of PCM nothing will ever play again.
    host_.clear_previews();
    s_.fileBrowser.selectionMode   = false;
    s_.fileBrowser.selectionAnchor = -1;
    s_.currentScreen = s_.previousScreen;
}

void InputDispatcher::refresh_browser() {
    FileBrowserState& b = s_.fileBrowser;

    // ⚠️ **A RE-LIST IS NOT A RE-READ UNLESS THE FILESYSTEM IS TOLD TO FORGET.** `SafFileSystem`
    // serves a cached listing and drops that cache only on this app's own writes, so without this
    // line a refresh hands back exactly what is already on screen — and a file that arrived from a
    // PC, a download or a file manager stays invisible until the next launch.
    //
    // Here rather than at the five call sites, so that adding a sixth cannot forget it. A no-op on
    // every implementation that caches nothing, which is all of them but Android's.
    fs_.forget_listing(b.currentDirectory);

    // Re-list in place and KEEP THE CURSOR where it was — a refresh is not a navigation. Deleting the
    // twentieth file in a folder should leave you on the twentieth row, not throw you back to the
    // first. It has to be CLAMPED, though: the list may have got shorter, and a cursor past the end of
    // it draws nothing highlighted — the row the user was on simply vanishes.
    rebuild_items(b, fs_);

    const int last = static_cast<int>(b.items.size()) - 1;
    b.cursor = std::min(std::max(b.cursor, 0), std::max(last, 0));

    if (b.cursor < b.scroll) b.scroll = b.cursor;
    if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    b.scroll = std::max(0, std::min(b.scroll, std::max(0, last - BROWSER_VISIBLE_ROWS + 1)));
}

void InputDispatcher::refresh_browser_on_foreground() {
    // ⚠️ Guarded on the SCREEN, not on the browser's state: `fileBrowser` keeps its directory and its
    // cursor after `close_file_browser`, so re-listing unconditionally would re-read a directory
    // nobody is looking at on every Home-and-back — and on Android that listing is a query per entry
    // over a content provider.
    if (s_.currentScreen != ScreenType::FILE_BROWSER) return;

    // ⚠️ NOT while a modal is up. The DELETE confirm names the row under the cursor, and a refresh can
    // move what is under the cursor — so a listing rebuilt behind the prompt would leave "DELETE X?"
    // on screen with the cursor now on Y, and A would delete Y. The browser is re-read on the next
    // gesture instead; a stale listing is a cosmetic problem, a mislabelled confirm is not.
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    refresh_browser();
}

// ─── The browser's cursor ────────────────────────────────────────────────────────────────────────

void InputDispatcher::browser_move_cursor(int delta, bool page) {
    FileBrowserState& b     = s_.fileBrowser;
    const int         total = static_cast<int>(b.items.size());
    if (total == 0) return;

    // ⚠️ UP/DOWN WRAP; the LEFT/RIGHT page jump CLAMPS. That asymmetry is Kotlin's and it is the right
    // one: wrapping a single step off the end of a list is a convenience, but a PAGE that wrapped would
    // fling you from the top of a 400-file directory to the bottom on one tap.
    if (page) {
        b.cursor = std::min(std::max(b.cursor + delta, 0), total - 1);
    } else {
        // ⚠️ Modulo TWICE, not `+ total` once: a page step is larger than 1, and one addition of
        // `total` only covers a step smaller than the whole list.
        b.cursor = ((b.cursor + delta) % total + total) % total;
    }

    // Keep the 19-row window around the cursor.
    if (b.cursor < b.scroll) {
        b.scroll = b.cursor;
    } else if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) {
        b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    }
}

// ─── A: open a folder, or LOAD the file ──────────────────────────────────────────────────────────

void InputDispatcher::browser_confirm() {
    FileBrowserState& b = s_.fileBrowser;

    // DELETE mode: A is the YES of "A=YES B=NO". This is the ONLY place the browser removes anything,
    // and it is two presses away from any accident — SELECT+B to arm, A to confirm.
    if (b.mode == BrowserMode::DELETE) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || item->is_pseudo()) return;

        const std::string name = item->displayName;
        if (fs_.delete_path(item->path)) {
            refresh_browser();
            b.statusMessage = "DELETED: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "DELETE FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    // SET_HOME mode: A is the YES of "A=YES B=NO", armed by SELECT+A on a granted tree.
    if (b.mode == BrowserMode::SET_HOME) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.set_home_directory(item->path)) {
            // ⚠️ **The two derived roots have to move WITH it, or the app looks in the new tree and
            // resolves media against the old one.** Both were read from the filesystem once, at boot
            // (`AppConfig::mediaBaseDir` and `SongcoreHost::set_app_root`), and neither is re-asked:
            // a project's RELATIVE sample paths join onto the first, and an absolute path authored
            // elsewhere re-roots onto the second. Derived here from an accessor rather than from the
            // row, so pt-ui never has to know what a platform's root string looks like.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "HOME FOLDER: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT SET HOME FOLDER";
            b.statusSuccess = false;
        }
        return;
    }

    // FORGET_ROOT mode: A is the YES of "A=YES B=NO", armed by SELECT+B on a granted tree.
    if (b.mode == BrowserMode::FORGET_ROOT) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.revoke_access(item->path)) {
            // ⚠️ The home may have been THIS tree, in which case the filesystem has just chosen another
            // — so the derived roots are re-asked here exactly as they are after a home change, and for
            // the same reason. An accessor answers the truth; a value read at boot does not.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "FORGOT: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT FORGET IT";
            b.statusSuccess = false;
        }
        return;
    }

    const BrowserItem* item = b.current();
    if (!item) return;

    if (item->is_parent()) { navigate_to_parent(b, fs_); return; }

    // An ACTION is not a place, it is a thing to do, and the filesystem owns what it means.
    //
    // ⚠️ **NO refresh on the way out, and that is not an omission.** `activate` starts something it
    // does not wait for — the one action there is opens Android's folder picker — so a listing rebuilt
    // here would be rebuilt from a world that has not changed yet. What catches up is
    // `refresh_browser_on_foreground()`, when the app comes back.
    if (item->kind == BrowserItem::Kind::ACTION) {
        const std::string label = item->displayName;
        if (!fs_.activate(item->path)) {
            b.statusMessage = label + " FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    if (item->kind == BrowserItem::Kind::FOLDER) { navigate_to_folder(b, fs_, item->path); return; }

    // ── It is a FILE, and what happens now is the whole reason the browser was opened ────────────
    const int         id   = s_.currentInstrument;
    const std::string ext  = to_lower(item->extension);
    const std::string path = item->path;
    const std::string stem = item->displayName;

    // Which slot a source load lands on. The SAMPLE EDITOR's own LOAD button targets the slot the
    // EDITOR is on, which is not necessarily the one the INSTRUMENT screen was last left showing.
    const int sourceId = (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR)
                             ? s_.sampleEditor.instrumentId
                             : id;
    // The name that slot is ABOUT to stop deserving — read before the load overwrites the path it comes
    // from. See the adopt rule below; on anything but a source load nobody looks at it.
    const std::string previousAutoName = instrument_auto_name(host_.project(), sourceId);

    // ⚠️ EVERY arm, not only the ones expected to be slow. Which loads are slow is a fact about the
    // FILE and the DEVICE, not about the menu item — a `.ptt` theme is bytes and a `.sf3` is half a
    // minute, and both come through here. The scope costs nothing on a fast one: below the delay
    // nothing is drawn and nothing is dimmed.
    const LoadScope loadScope(*this, now_ms_, stem);

    bool ok = false;
    switch (s_.browserPurpose) {
        case AppState::BrowserPurpose::LOAD_PRESET:
            ok = host_.load_instrument_preset(id, path);
            break;

        case AppState::BrowserPurpose::LOAD_SOURCE:
            // The extension decides, not the slot's current type: picking an .sf2 from a sampler slot
            // TURNS it into a SoundFont slot (load_instrument_soundfont sets the type), which is what
            // the user asked for by picking one. The browser's filter usually makes this moot — but the
            // user can navigate anywhere, and a folder full of both is not exotic.
            //
            // `.sf3` is here on the same footing as `.sf2` — same font, same float buffer at the end
            // of it, and the compressed one is the CHEAPER of the two in peak memory (audio-engine.h).
            if (is_soundfont_extension(ext)) {
                ok = host_.load_soundfont(id, path);
            } else {
                ok = host_.load_sample(id, path);
            }
            break;

        case AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR:
            // The editor's own LOAD button. Same load, but it returns to the EDITOR rather than to
            // INSTRUMENT — you came here to pick something to cut up, not to leave.
            ok = host_.load_sample(s_.sampleEditor.instrumentId, path);
            break;

        case AppState::BrowserPurpose::LOAD_PROJECT:
            // ⚠️ The WHOLE DOCUMENT, and it returns early — everything below this switch is about an
            // INSTRUMENT that just gained a source, and none of it applies. `load_project_file` is the
            // guard rail S4 paid 84.4% of a render for: parse → push → load_media → push_params, in one
            // call, so the obligation cannot be forgotten by a new caller. This is that new caller.
            if (!host_.load_project_file(path, fs_.samples_directory())) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            // ⚠️⚠️ **A CANCELLED PROJECT LOAD CANNOT BE LEFT WHERE IT STOPPED, AND THIS IS THE ONE
            // CANCEL THAT COSTS SOMETHING.** A single file that is stopped leaves the slot it was
            // going into untouched; a project is a whole document, already swapped in, with the
            // instruments after the stopping point pointing at audio the engine does not have — they
            // would look loaded on the screen and play silence. The honest state is the blank
            // document NEW PROJECT gives, and the message says which of the two happened.
            if (pt::load_cancelled()) {
                host_.new_project();
                host_.push_params();
                // The same settling `load_project_done` does — an empty path, because there is no
                // file this document came from, and the autosave cleared because the work that was
                // in it belonged to the project the user has just left.
                load_project_done("");
                s_.statusMessage = "LOAD CANCELLED";
                s_.statusSuccess = true;
                return;
            }
            load_project_done(path);
            return;

        case AppState::BrowserPurpose::LOAD_THEME:
            // ⚠️ A THEME IS NOT THE PROJECT AND NOT AN INSTRUMENT, so this returns early too — nothing
            // below applies. It touches no engine, no sample, no slot: a palette is pixels.
            //
            // ⚠️ The extension is re-checked even though the browser was opened filtered to "ptt", and
            // Kotlin re-checks it too (`item.file.extension.lowercase() == "ptt"`). The filter is not a
            // guarantee: the user can navigate OUT of the Themes folder into anywhere, and the D-pad
            // does not stop at a directory boundary. A failed parse must not blank the palette.
            if (ext != "ptt" || !load_theme_file(fs_, path, s_.theme)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            // Back to the editor that raised the browser — which `theme_row_action` CLOSED on the way
            // out, because the browser is a screen and would have been standing underneath it.
            close_file_browser();
            open_theme_editor();
            s_.statusMessage = "THEME LOADED";
            s_.statusSuccess = true;
            return;

        case AppState::BrowserPurpose::LOAD_SCALE: {
            // ⚠️ Loaded into a COPY and only committed once it parses, so a truncated or hand-mangled
            // file cannot leave a slot half-overwritten — and the copy is what keeps the slot's `id`,
            // which is *which of the sixteen this is* rather than anything the file gets a say in.
            //
            // ⚠️ The extension is re-checked even though the browser was opened filtered: the user can
            // walk out of the Scales folder, and the D-pad does not stop at a directory boundary.
            songcore::Scale loaded = host_.project().scales[static_cast<size_t>(s_.currentScale)];
            if (ext != SCALE_FILE_EXT || !load_scale_file(fs_, path, loaded)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            host_.edit_project().scales[static_cast<size_t>(s_.currentScale)] = loaded;
            mark_modified();
            close_file_browser();
            s_.statusMessage = "SCALE LOADED";
            s_.statusSuccess = true;
            return;
        }

        case AppState::BrowserPurpose::LOAD_GROOVE: {
            // The scale load's two guards, and for the same two reasons: a COPY so a mangled file
            // cannot leave a slot half-overwritten and so the slot keeps its `id` — which is what
            // `GRV` in a phrase names — and the extension re-checked because the D-pad can walk out
            // of the Grooves folder.
            songcore::Groove loaded = host_.project().grooves[static_cast<size_t>(s_.currentGroove)];
            if (ext != GROOVE_FILE_EXT || !load_groove_file(fs_, path, loaded)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)] = loaded;
            mark_modified();
            close_file_browser();
            s_.statusMessage = "GROOVE LOADED";
            s_.statusSuccess = true;
            return;
        }
    }

    if (!ok) {
        // ⚠️ A CANCEL IS NOT A FAILURE AND GETS NO RED LINE. The user pressed B; being told LOAD
        // FAILED afterwards reads as "and it would not have worked anyway", which is a claim about
        // the file that nothing here knows. It is asked first for that reason.
        if (host_.last_load_cancelled()) {
            b.statusMessage = "CANCELLED";
            b.statusSuccess = true;
            return;
        }
        // ⚠️ "LOAD FAILED" for a file the DEVICE cannot hold sends the user looking for a corrupt
        // file that is fine. The engine separates the two, and that separation is the whole message:
        // the file is sound, this machine cannot hold it, pick a smaller one. No free figure beside
        // it — the only decision the number could inform has already been made by the refusal, and a
        // megabyte count on a status line is a quantity the user has nothing to compare against.
        b.statusMessage = host_.last_load_ran_out_of_memory() ? "FILE TOO BIG" : "LOAD FAILED";
        b.statusSuccess = false;
        return;
    }

    // The slot adopts the file's name — unless the user TYPED the one it has. Two names are not the
    // user's: the default ("INST07"), and the one the slot took from the source it is replacing right
    // now. Keeping the second is what left a slot reading RHODES C4 after a different sample was
    // loaded onto it, which is a label that lies about what the slot plays.
    //
    // A typed name still survives a source swap — that is the whole reason the default-name test was
    // here — and it is `previousAutoName`, captured before the load, that can tell the two apart.
    //
    // Both source loads, not just the INSTRUMENT screen's: the editor's LOAD replaces the same slot's
    // sample, and a rule that held on one of the two doors would leave a name the other could no
    // longer correct (it would no longer match `previousAutoName`, and would look typed forever).
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        sourceId >= 0 && static_cast<size_t>(sourceId) < host_.project().instruments.size()) {
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(sourceId)];
        if (songcore::instrument_has_default_name(ins) ||
            (!previousAutoName.empty() && ins.name == previousAutoName)) {
            ins.name = adopted_name(stem);
        }
    }

    mark_modified();

    // D2a: remember the folder a SAMPLE was loaded from — `b.currentDirectory` IS that folder (the file
    // just loaded is an item in it). Only for the two sample-load purposes, and not for a SoundFont
    // picked out of a sampler slot — the row is about SAMPLE folders. Persisted on exit by
    // save_settings_if_changed (no dirty flag), so nothing here has to write the file.
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        !is_soundfont_extension(ext)) {
        s_.settings.lastSampleFolder = b.currentDirectory;
    }

    if (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) {
        // Re-enter the EDITOR on the new audio — not `previousScreen`, which is still the INSTRUMENT
        // screen the editor itself will return to. Everything the old sample's session knew is now false,
        // so the state is rebuilt rather than patched.
        host_.clear_previews();
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        s_.currentScreen = ScreenType::SAMPLE_EDITOR;
        init_sample_editor_state();
        return;
    }

    close_file_browser();
}

// ─── The multi-select and the file clipboard ─────────────────────────────────────────────────────

std::vector<std::string> InputDispatcher::browser_selected_paths() const {
    const FileBrowserState& b = s_.fileBrowser;
    std::vector<std::string> out;
    if (!b.selectionMode || b.selectionAnchor < 0) return out;

    const int lo = std::max(std::min(b.selectionAnchor, b.cursor), b.first_selectable());
    const int hi = std::max(b.selectionAnchor, b.cursor);
    for (int i = lo; i <= hi; ++i) {
        const BrowserItem* item = b.item_at(i);
        if (item && !item->is_pseudo()) out.push_back(item->path);
    }
    return out;
}

void InputDispatcher::browser_paste() {
    FileBrowserState& b = s_.fileBrowser;
    if (b.fileClipboard.empty()) return;

    const std::string& dest = b.currentDirectory;
    int done = 0, failed = 0;

    for (const std::string& src : b.fileClipboard) {
        if (!fs_.file_exists(src)) { ++failed; continue; }

        const std::string name = path_name(src);
        std::string       target = dest + "/" + name;
        if (target == src) { ++done; continue; }   // pasted back into the folder it was copied from

        // De-duplicate: "kick.wav" → "kick_2.wav" → "kick_3.wav". Never overwrite — a paste that
        // silently replaced a file of the same name would be a data-loss bug wearing a convenience hat.
        if (fs_.file_exists(target)) {
            const std::string ext  = path_extension(name);
            const std::string base = path_stem(name);
            for (int n = 2;; ++n) {
                target = dest + "/" + base + "_" + std::to_string(n) + (ext.empty() ? "" : "." + ext);
                if (!fs_.file_exists(target)) break;
            }
        }

        const bool ok = b.fileClipboardIsCut ? fs_.move_file(src, target) : fs_.copy_file(src, target);
        if (ok) ++done; else ++failed;
    }

    const bool  cut  = b.fileClipboardIsCut;
    const char* verb = cut ? "MOVED" : "COPIED";

    // A CUT clipboard is spent once pasted — the sources are gone. A COPY one survives, so the same
    // files can be pasted into several folders.
    if (cut) b.fileClipboard.clear();

    refresh_browser();
    b.statusMessage = failed == 0
                          ? std::string(verb) + " " + std::to_string(done) +
                                (done == 1 ? " FILE" : " FILES")
                          : std::string(verb) + " " + std::to_string(done) + ", FAILED " +
                                std::to_string(failed);
    b.statusSuccess = (failed == 0);
}

// ─── SELECT + A / B / R — the browser's file-management chords ───────────────────────────────────

void InputDispatcher::on_select_a() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⭐ **On a granted TREE the three file chords are all meaningless, so SELECT+A means the one thing
    // that row CAN offer: make this the folder the app keeps its own directories in.** Before this
    // there was no way to change it at all — Android's first grant won permanently, and a user who
    // granted the wrong folder had to clear the app's data to get out of it. Armed, never immediate:
    // A confirms, B cancels, exactly as SELECT+B's delete does.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::SET_HOME;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;   // ".." and ADD FOLDER… are not files and have no name

    const bool  dir   = (item->kind == BrowserItem::Kind::FOLDER);
    const std::string ext = to_lower(item->extension);
    const char* label = dir              ? "FOLDER NAME:"
                        : (ext == "wav") ? "SAMPLE NAME:"
                        : (ext == "ptp") ? "PROJECT NAME:"
                                         : "FILE NAME:";

    // A FOLDER's displayName is "[name]" — the brackets are decoration, and typing them back into the
    // rename box would make them part of the name.
    const std::string base = dir ? path_name(item->path) : item->displayName;
    open_qwerty(QwertyContext::FILE_RENAME, base, label, item->path);
}

void InputDispatcher::on_select_b() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⚠️ **On a granted TREE this is FORGET, not DELETE, and the difference is everything the row is.**
    // `delete_path("pt://<id>")` resolves to the granted folder's own document — the user's whole
    // PocketTracker directory. What SELECT+B can honestly offer here is handing the PERMISSION back,
    // which removes the row and touches not one file. It is also the only way to clear a folder that
    // has been deleted from under its grant: Android keeps such a grant for the life of the install.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::FORGET_ROOT;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;

    // ARM the confirm; never delete on this press. The top bar becomes "DELETE <name>? A=YES B=NO".
    s_.fileBrowser.mode          = BrowserMode::DELETE;
    s_.fileBrowser.statusMessage.clear();
    s_.fileBrowser.statusSuccess = true;
}

void InputDispatcher::on_select_r() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;
    open_qwerty(QwertyContext::FOLDER_CREATE, "NEW FOLDER", "FOLDER NAME:",
                s_.fileBrowser.currentDirectory);
}

// ─── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_qwerty(QwertyContext context, const std::string& initial_text,
                                  const std::string& field_label, const std::string& context_extra,
                                  int max_length, bool clear_on_first_b) {
    QwertyKeyboardState k{};
    k.isOpen        = true;
    k.text          = initial_text.substr(0, static_cast<size_t>(max_length));
    k.maxLength     = max_length;
    k.textCursor    = static_cast<int>(k.text.size());
    k.fieldLabel    = field_label;
    k.contextExtra  = context_extra;
    k.context       = context;
    k.clearOnFirstB = clear_on_first_b;
    k.insertBefore  = s_.settings.insertBefore;   // read at OPEN, so flipping the setting cannot change what
    s_.qwerty       = k;                 // the buttons mean under the user's thumb mid-word
}

void InputDispatcher::qwerty_apply() {
    const QwertyKeyboardState k    = s_.qwerty;   // by value: every arm below closes the keyboard
    const std::string         text = trimmed_text(k);
    s_.qwerty = QwertyKeyboardState{};

    switch (k.context) {
        case QwertyContext::FILE_RENAME: {
            // An empty field means "leave it alone", not "name it nothing" — Kotlin's `.ifEmpty { … }`.
            const std::string name = text.empty() ? path_stem(k.contextExtra) : text;
            if (fs_.rename_file(k.contextExtra, name)) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "RENAMED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "RENAME FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "CREATED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "CREATE FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::INSTRUMENT_NAME: {
            Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(s_.currentInstrument)];
            // A cleared name reverts to the default "INSTxx" rather than becoming blank — an unnamed
            // instrument still has to be identifiable in the pool.
            ins.name = text.empty() ? songcore::default_instrument_name(ins.id) : text;
            mark_modified();
            break;
        }

        case QwertyContext::PROJECT_NAME:
            // ⚠️ No empty-name fallback, unlike every other arm here. Kotlin's is a bare
            // `trackerController.project.name = typedText`, and it is ported as-is — but note what it
            // leads to: an empty name sanitizes to an empty filename, so SAVE writes `<Projects>/.ptp`
            // (hidden on a POSIX box, and not listed by the browser's own filter). The same is true on
            // Android today. Left bug-for-bug rather than quietly diverging; it wants a fix on BOTH
            // platforms, which makes it a finding, not a port decision.
            host_.edit_project().name = text;
            mark_modified();
            break;

        case QwertyContext::INSTRUMENT_SAVE: {
            const std::string name = text.empty() ? "PRESET" : text;
            const std::string path = k.contextExtra + "/" + name + ".pti";
            if (save_instrument_preset(host_, fs_, s_.currentInstrument, path)) {
                s_.statusMessage = "SAVED: " + name;
                s_.statusSuccess = true;
            } else {
                s_.statusMessage = "SAVE FAILED";
                s_.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::THEME_SAVE:
            // The whole arm is `save_theme_as` — see it for the two names one typed string turns into,
            // and for the error return Kotlin drops on the floor. ⚠️ `k.contextExtra`, not
            // `s_.qwerty.contextExtra`: the live keyboard was cleared at the top of this function.
            save_theme_as(k.contextExtra, text);
            break;

        case QwertyContext::SCALE_SAVE:
            // ⚠️ `k.contextExtra`, for the same reason the arm above it takes one: the live keyboard is
            // cleared before any of these run, so reading `s_.qwerty` here writes to the filesystem root.
            save_scale_as(k.contextExtra, text);
            break;

        case QwertyContext::GROOVE_SAVE:
            save_groove_as(k.contextExtra, text);
            break;

        case QwertyContext::SAMPLE_NAME: {
            // It renames BOTH the editor's sample and the INSTRUMENT holding it — they are the same
            // thing to the user, and the pool showing "INST05" for a slot you have just named "SNARE"
            // would be the app disagreeing with itself. An empty field keeps the current name.
            SampleEditorState& se = s_.sampleEditor;
            const std::string  name = text.empty() ? se.sampleName : text;
            se.sampleName = name;
            host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)].name = name;
            mark_modified();
            break;
        }

        case QwertyContext::SAMPLE_SAVE: {
            // SAVE-AS. The name is DE-DUPLICATED rather than overwritten: `SNARE.wav`, `SNARE_0001.wav`,
            // … The editor has an OVERWRITE button and this is not it — a save that silently replaced a
            // file you did not name would be the one destructive act with no confirm in front of it.
            const std::string base = text.empty() ? "SAMPLE" : text;
            std::string       path = k.contextExtra + "/" + base + ".wav";
            for (int n = 1; fs_.file_exists(path); ++n) {
                char suffix[16];   // 16, not 8: "_%04d" of an unbounded int is up to 12 bytes, and gcc says so (-Wformat-truncation). The counter never gets near it; the buffer now cannot be the reason.
                std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                path = k.contextExtra + "/" + base + suffix + ".wav";
            }
            save_sample_to(path, /*adopt_name=*/true);
            break;
        }

        case QwertyContext::RESAMPLE:
            // An empty field (the user cleared the pre-filled suggestion) auto-names Resample_NNNN;
            // anything typed is used as the base name. The selection is still live — opening the
            // keyboard never touched it — so resample_selection reads s_.selection itself.
            resample_selection(text);
            break;
    }
}

}  // namespace pt::ui
