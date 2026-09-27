// The THEME, SCALE and GROOVE screens: what A does on their action rows, and saving to a file.

#include "ui/dispatch/dispatch_common.h"

#include "ui/groove_io.h"        // .ptg — save_groove_file / load_groove_file / the factory seed
#include "ui/scale_io.h"         // .pts — save_scale_file / load_scale_file / the factory seed
#include "ui/theme_io.h"         // .ptt — save_theme_file / load_theme_file

#include <algorithm>
#include <string>

namespace pt::ui {

// ─── The THEME EDITOR ─────────────────────────────────────────────────────────────────────────────

void InputDispatcher::theme_move_cursor(int d_row, int d_channel) {
    // ⚠️ BOTH AXES WRAP, and neither clamps — which makes this the only cursor in the app that wraps in
    // BOTH directions. The rows are the two header rows plus one per colour, and the argument is the
    // mixer's row 0 again: a list of colours is a ring you scroll, not a document you reach the end
    // of. The panel SCROLLS to follow the row (only 16 rows fit), so wrapping from the last row to
    // row 0 also scrolls the list back to the top.
    if (d_row != 0) {
        const int row = s_.themeEditor.cursorRow;
        const int max = ThemeEditorModule::max_row();
        s_.themeEditor.cursorRow = (d_row < 0) ? (row > 0 ? row - 1 : max)
                                               : (row < max ? row + 1 : 0);
    }
    // ⚠️ THE RING'S SIZE IS A FUNCTION OF THE ROW — RANDOMIZE carries two cells where the others
    // carry three — and it is read AFTER the row has moved, so stepping onto a shorter row from a
    // colour row's B channel cannot leave the cursor on a cell that row does not have.
    const int last = theme_channel_count(s_.themeEditor.cursorRow) - 1;
    if (d_channel != 0) {
        const int ch = s_.themeEditor.cursorChannel;
        s_.themeEditor.cursorChannel = (d_channel < 0) ? (ch > 0 ? ch - 1 : last)
                                                       : (ch < last ? ch + 1 : 0);
    }
    if (s_.themeEditor.cursorChannel > last) s_.themeEditor.cursorChannel = last;

    theme_refresh_message();
}

/**
 * A+DPAD in the theme editor, which means three different things depending on the cell.
 *
 * ⚠️ WRITTEN ONCE AND CALLED FROM ALL FOUR DIRECTIONS, because the THEME row grew a cell: an arm per
 * direction meant four places to remember that channel 1 is now the style, and the cost of missing
 * one is a gesture that works on three edges and silently does nothing on the fourth.
 */
void InputDispatcher::theme_dpad_edit(int cycleDelta, int nudge) {
    ThemeEditorState& es = s_.themeEditor;

    const int color = theme_color_index(es.cursorRow);
    if (color >= 0) {
        theme_adjust_color(s_.theme, color + 1, es.cursorChannel, nudge);
        theme_refresh_message();
        return;
    }
    if (es.cursorRow == THEME_ROW_RANDOM) {
        // The scheme cell, a ring like every other value in the app. ROLL is a button and there is
        // nothing on it to dial.
        if (es.cursorChannel == 0) {
            const int cur = static_cast<int>(es.scheme);
            es.scheme = static_cast<ThemeScheme>(
                ((cur + cycleDelta) % THEME_SCHEME_COUNT + THEME_SCHEME_COUNT) % THEME_SCHEME_COUNT);
        }
        return;
    }
    // The THEME row: the NAME cell steps the built-in palettes; SAVE and LOAD are buttons.
    if (es.cursorChannel == 0) theme_cycle_builtin(s_.theme, cycleDelta);
}

/**
 * Drop a failed roll's message.
 *
 * ⚠️ IT ONLY CLEARS. The clash line is derived in the draw from the palette and the cursor, so the
 * one thing input owes it is not to leave a stale EVENT standing in front of it.
 */
void InputDispatcher::theme_refresh_message() { s_.themeEditor.message.clear(); }

/**
 * Roll the palette. `rowOnly` re-rolls one row with the other eighteen held, which is the same
 * solver with far more of its inputs fixed — and therefore far likelier to fail.
 */
void InputDispatcher::theme_roll_palette(bool rowOnly) {
    ThemeEditorState& es = s_.themeEditor;

    ThemeLocks locks = es.locks;
    if (rowOnly) {
        // ⚠️ "This row only" is expressed as "everything else is locked", so there is ONE solver and
        // one place the rules live. A second code path for a single row is a second set of rules.
        const int target = theme_color_index(es.cursorRow);
        for (size_t i = 0; i < locks.row.size(); ++i) locks.row[i] = (static_cast<int>(i) != target);
    }

    es.seed = es.seed * 1664525u + 1013904223u;
    const ThemeRollResult r = theme_roll(s_.theme, locks, es.scheme, es.seed);

    if (!r.ok) {
        // ⚠️ THE PALETTE IS LEFT ALONE. A roll that cannot satisfy the rules must not hand back its
        // best attempt — a half-legal palette the user did not ask for is worse than no change, and
        // they would have no way to tell the two apart.
        es.message = rowOnly ? "ROW UNSOLVABLE" : "LOCKS UNSOLVABLE";
        return;
    }

    const std::string keepName = s_.theme.name;
    s_.theme      = r.theme;
    s_.theme.name = keepName;
    theme_refresh_message();
}

/**
 * Kotlin's `name.replace(Regex("[^a-zA-Z0-9_]"), "_")` — a theme name, as a FILENAME.
 *
 * ⚠️ It is NOT `.ifEmpty` on its own: the fallback is applied by the callers, and both of them apply it
 * (`"THEME"`). That is S7's dotfile bug in waiting — an all-punctuation name sanitizes to underscores,
 * but an EMPTY one sanitizes to an empty string, and `<Themes>/.ptt` is a dotfile the browser SKIPS.
 * Kotlin already guards it here (unlike `saveProject`, which did not until S7 fixed it), so this is the
 * one save path in the app that was never broken. Kept explicit so it stays that way.
 */
static std::string sanitize_theme_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

void InputDispatcher::theme_row_action() {
    if (s_.themeEditor.cursorRow == THEME_ROW_RANDOM) {
        // ROLL — a whole palette, in the scheme the cell beside it is showing.
        if (s_.themeEditor.cursorChannel == 1) theme_roll_palette(/*rowOnly=*/false);
        return;
    }
    switch (s_.themeEditor.cursorChannel) {
        case 1: {   // SAVE — name it, then write it
            // ⚠️ The keyboard opens WITHOUT closing the editor, which is why every handler tests
            // `qwerty_open()` before `theme_open()`. It seeds with the SANITIZED current name, so what
            // you are shown is what the file will be called.
            const std::string seed = sanitize_theme_filename(s_.theme.name);
            open_qwerty(QwertyContext::THEME_SAVE, seed.empty() ? "THEME" : seed, "SAVE THEME:",
                        fs_.themes_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case 2: {   // LOAD — browse the Themes folder for a .ptt
            // ⚠️ This one CLOSES the editor first, where SAVE does not. The browser is a SCREEN, not an
            // overlay — it takes over `currentScreen` — so leaving the editor open would leave a modal
            // standing on top of a screen it was never raised from, swallowing the browser's own D-pad.
            // `browser_confirm` re-opens the editor when a theme lands.
            close_theme_editor();
            open_file_browser(AppState::BrowserPurpose::LOAD_THEME, browser_dir(BrowserDir::THEMES),
                              {"ptt"});
            break;
        }
        default:    // column 0 is the NAME, and a bare A on it does nothing. Kotlin's `when` has no arm.
            break;
    }
}

// ─── The SCALE screen's NAME row ─────────────────────────────────────────────────────────────────

void InputDispatcher::scale_row_action() {
    const songcore::Scale& scale =
        host_.project().scales[static_cast<size_t>(s_.currentScale)];

    switch (scale_name_action(s_.scaleCursorRow, s_.scaleCursorColumn)) {
        case ScaleNameAction::SAVE: {
            // Seeded with the SANITIZED name the row is showing, so what you are shown is what the file
            // will be called — and the row shows a name even for a slot that stores none, which is why
            // it is `scale_display_name` and not `scale.name`.
            const std::string seed = sanitize_scale_filename(songcore::scale_display_name(scale));
            open_qwerty(QwertyContext::SCALE_SAVE, seed.empty() ? "SCALE" : seed, "SAVE SCALE:",
                        fs_.scales_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case ScaleNameAction::LOAD:
            // ⚠️ Unlike the theme's, nothing has to be closed first: SCALE is a SCREEN, so the browser
            // simply replaces it and `previousScreen` brings the user back. The theme editor is an
            // overlay and would have been left standing underneath.
            //
            // ⚠️ And unlike every other load, this browser starts at the built-in folder rather than at
            // a config.json override — `folders` names five categories and scales is not one of them.
            // Adding a sixth key means changing the template every user has already been seeded with,
            // which is a decision about config.json rather than about scales.
            open_file_browser(AppState::BrowserPurpose::LOAD_SCALE, fs_.scales_directory(),
                              {SCALE_FILE_EXT});
            break;
        case ScaleNameAction::NONE:
            break;
    }
}

void InputDispatcher::save_scale_as(const std::string& dir, const std::string& typed_text) {
    // The theme save's two-names rule, on a scale: the FILENAME is sanitized so it survives a FAT32
    // card, the name IN the file is what was typed. An empty field keeps the name the row was showing
    // rather than blanking it, and falls back to "SCALE" for the file — never `.pts`, which is a
    // dotfile the browser does not list.
    const std::string safe = sanitize_scale_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("SCALE") : safe) + ".pts";

    songcore::Scale& slot = host_.edit_project().scales[static_cast<size_t>(s_.currentScale)];

    // ⚠️ THE SLOT ADOPTS THE NAME IT WAS SAVED UNDER, where the THEME row deliberately does not. That
    // divergence is on purpose: the theme's is a Kotlin wart kept for parity, and here the name is the
    // only thing on screen that says which file this slot is. Adopting it is also what clears the `*` —
    // the slot now matches the shape it is named after, because that shape is the one just written.
    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::scale_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_scale_file(fs_, dir + "/" + file, slot);
    s_.statusMessage = ok ? "SCALE SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
}

// ─── The GROOVE screen's SAVE / LOAD cells ───────────────────────────────────────────────────────

void InputDispatcher::groove_row_action() {
    const songcore::Groove& groove =
        host_.project().grooves[static_cast<size_t>(s_.currentGroove)];

    switch (groove_file_action(s_.groovePanelRow, s_.groovePanelColumn)) {
        case GrooveFileAction::SAVE: {
            // Seeded with the SANITIZED name the panel is showing, so what you are shown is what the
            // file will be called — and the panel shows a name even for a slot that stores none,
            // which is why it is `groove_display_name` and not `groove.name`.
            const std::string seed = sanitize_groove_filename(songcore::groove_display_name(groove));
            open_qwerty(QwertyContext::GROOVE_SAVE, seed.empty() ? "GROOVE" : seed, "SAVE GROOVE:",
                        fs_.grooves_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case GrooveFileAction::LOAD:
            // GROOVE is a SCREEN, so the browser simply replaces it and `previousScreen` brings the
            // user back — nothing has to be closed first, as it does for the theme editor's overlay.
            // And like the scale browser, this one starts at the built-in folder: `folders` in
            // config.json names five categories and grooves is not one of them.
            open_file_browser(AppState::BrowserPurpose::LOAD_GROOVE, fs_.grooves_directory(),
                              {GROOVE_FILE_EXT});
            break;
        case GrooveFileAction::NONE:
            break;
    }
}

void InputDispatcher::save_groove_as(const std::string& dir, const std::string& typed_text) {
    // The scale save's two-names rule, on a groove: the FILENAME is sanitized so it survives a FAT32
    // card, the name IN the file is what was typed. An empty field keeps the name the panel was
    // showing rather than blanking it, and falls back to "GROOVE" for the file — never `.ptg`, which
    // is a dotfile the browser does not list.
    const std::string safe = sanitize_groove_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("GROOVE") : safe) + ".ptg";

    songcore::Groove& slot = host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)];

    // ⚠️ THE SLOT ADOPTS THE NAME IT WAS SAVED UNDER, as the scale slot does. The name is the only
    // thing on the panel that says which file this slot is, and adopting it is also what clears the
    // `*`: the slot now matches the shape it is named after, because that shape is the one just
    // written.
    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::groove_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_groove_file(fs_, dir + "/" + file, slot);
    s_.statusMessage = ok ? "GROOVE SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
}

void InputDispatcher::save_theme_as(const std::string& dir, const std::string& typed_text) {
    // ⚠️ TWO DIFFERENT NAMES COME OUT OF ONE TYPED STRING, and mixing them up is the whole trap here:
    //
    //   the FILENAME is SANITIZED  → "My Theme!"  becomes  My_Theme_.ptt
    //   the NAME IN THE FILE is RAW → "My Theme!"  stays    "name": "My Theme!"
    //
    // That is deliberate on Android and it is right: the filename must survive a FAT32 SD card, and the
    // display name must survive the user's taste. An empty field keeps the theme's CURRENT name rather
    // than blanking it, and falls back to "THEME" for the file (never `.ptt`, which the browser hides —
    // S7's dotfile bug, which this path has always been guarded against).
    const std::string safe = sanitize_theme_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("THEME") : safe) + ".ptt";
    const std::string path = dir + "/" + file;

    Theme to_save = s_.theme;
    if (!typed_text.empty()) to_save.name = typed_text;

    // ⚠️ THE LIVE THEME DOES NOT ADOPT THE NAME IT WAS JUST SAVED UNDER, and that is Kotlin's, kept.
    // `themeToSave` is a COPY (`appTheme.copy(name = …)`); `appTheme` itself is never reassigned. So you
    // save your palette as SUNSET and the THEME row still reads CLASSIC. It is cosmetic — the FILE is
    // correct, and loading it back does set the name — and it is left alone rather than "fixed", because
    // the name is what the built-in cycle keys off (`theme_cycle_builtin`) and adopting a custom name
    // would silently change which palette A+RIGHT lands on. A divergence with a behavioural tail is not a
    // tidy-up. Stated here rather than smuggled in; a parity-ledger entry, not a port decision.
    const bool ok = save_theme_file(fs_, path, to_save);

    // ⚠️ …but a save that FAILS must say so, and Kotlin's does not: it discards `writeFile`'s Boolean
    // outright, so a full SD card or a read-only mount closes the keyboard and reports success by
    // silence. That is S7's own headline, one screen later — "a SAVE that reports nothing is
    // indistinguishable from a SAVE that failed" — and it is a dropped error return, not a missing
    // nicety. Zone B, so it is fixed on Android too (AppInputDispatcher, QwertyContext.THEME_SAVE).
    s_.statusMessage = ok ? "THEME SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;

    open_theme_editor();
}

}  // namespace pt::ui
