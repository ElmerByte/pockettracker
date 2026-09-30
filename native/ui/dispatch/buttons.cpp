// The plain buttons — A, B, SELECT, START — and LIVE mode's two chords.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/navigation.h"
#include "ui/std_filesystem.h"

#include <algorithm>
#include <string>

namespace pt::ui {

namespace {

/** Chain.isEmpty(row) — the row holds no phrase. */
bool chain_row_empty(const Chain& c, int row) { return c.phraseRefs[static_cast<size_t>(row)] == -1; }

}  // namespace

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_button_a() {
    // Every layer below is ARMED — A means something on each of them, which is why this call swallows
    // only the FX helper. It is still here so that a layer added tomorrow is INERT on A rather than
    // inserting a chain on the screen hidden behind it.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::QWERTY | Overlay::THEME | Overlay::EQ |
                         Overlay::BROWSER | Overlay::RENDER)) return;

    // A on the RENDER dialog fires it — from the RENDER row alone. The three rows above are dialled
    // with A+DPAD and have nothing for a bare A to confirm.
    if (render_dialog_open()) {
        if (s_.renderDialog.is_on(RenderRow::RENDER)) render_dialog_fire();
        return;
    }

    // ⚠️ THE CONFIRM DIALOG IS CHECKED FIRST, ahead of the keyboard and the browser both. It is the
    // topmost modal — drawn last, over everything — and a dialog owns the buttons of whatever it is
    // covering: pressing A to answer "CLEAN INST?" must not also fire whatever the cursor is parked on
    // underneath.
    if (confirm_open()) { confirm_accept(); return; }

    // A on the KEYBOARD types the key under the cursor — unless the cursor is on the action row, where
    // the two buttons ARE the answer: ABORT (col 0) and APPLY (col 1).
    if (qwerty_open()) {
        if (s_.qwerty.is_on_action_row()) {
            if (s_.qwerty.keyCursorCol == 0) qwerty_cancel();
            else                             qwerty_apply();
        } else {
            insert_current_key(s_.qwerty);
        }
        return;
    }

    // A on the BROWSER opens a folder, goes up, or loads the file — see browser_confirm.
    if (on_browser()) { browser_confirm(); return; }

    // ⚠️ A on the SAMPLE EDITOR's confirm dialog is YES: discard the unsaved edits and leave. It is
    // checked FIRST, because a dialog owns the buttons of the screen it is covering — pressing A to
    // answer "ARE YOU SURE?" must not also fire whatever op the cursor happens to be parked on.
    if (on_sample_editor() && s_.sampleEditor.showConfirmClose) {
        s_.sampleEditor.showConfirmClose = false;
        close_sample_editor();
        return;
    }

    // ⚠️ A in the THEME EDITOR is meaningful on exactly ONE row and TWO of its three columns — the THEME
    // row's SAVE and LOAD. Everywhere else (the name itself, and all seventeen colour rows) it does
    // NOTHING, because a colour channel is dialled with A+DPAD and has nothing for a bare A to confirm.
    //
    // Like the EQ arm below it, this must RETURN rather than fall through: `currentScreen` is still
    // SETTINGS underneath, and SETTINGS' own A is `settings_action()` — whose THEME row (9) is the very
    // cell the cursor is parked on. Fall through and A would RE-OPEN the editor that is already open,
    // resetting the cursor to row 0 under the user's thumb.
    if (theme_open()) {
        if (theme_color_index(s_.themeEditor.cursorRow) < 0) theme_row_action();
        return;
    }

    // ⚠️ A PLAIN A DOES NOTHING IN THE EQ EDITOR, and that is Kotlin (`AppInputDispatcher:1300`), not an
    // omission. The editor's vocabulary is A+DPAD (dial the band value), A+B (reset it), B+DPAD (change
    // slot), and B or SELECT (close). There is no cell here for a bare A to insert into or confirm.
    //
    // It must RETURN rather than fall through, and that is the load-bearing half: `currentScreen` is
    // still the screen underneath, so without this line an A in the editor would fire a sample-editor op
    // or insert a chain on a screen the user cannot even see.
    if (eq_open()) return;

    if (s_.currentScreen == ScreenType::ENVELOPE_EDITOR) return;

    // A on a cell that OPENS a sub-screen — the two NAME rows and all five EQ cells. Runs BEFORE the
    // per-screen arms below, exactly as Kotlin's `openSubScreenAtCursor(peek = false)` does, because
    // those cells have nothing to insert and the sample editor's EQ cell would otherwise run its FX
    // APPLY instead of opening the editor.
    if (open_sub_screen_at_cursor(/*peek=*/false)) return;

    if (on_sample_editor()) { sample_editor_confirm(); return; }

    // A on INSTRUMENT's LOAD / SAVE / EDIT buttons and the pool's empty NAME slot. NOT deferred to
    // release (they are read-only cells with no A+DPAD to protect), which is why they are not part of
    // `open_sub_screen_at_cursor` — Kotlin splits them the same way (`handleConfirmAInstrument`).
    if (instrument_open_at_cursor()) return;

    // A on the SCALE screen's SAVE / LOAD cells. Like the two arms above it this returns rather than
    // falling through, and unlike them it is a SCREEN rather than an overlay — so it is placed here, in
    // the run of "A on a button", and not up among the modal guards.
    if (s_.currentScreen == ScreenType::SCALE) {
        scale_row_action();
        return;
    }

    // A on the GROOVE screen's panel. ⚠️ It must NOT return for the tick grid — a bare A there lays a
    // step down on an empty row, which is the insert arm further below.
    if (s_.currentScreen == ScreenType::GROOVE && s_.grooveCursorColumn == GROOVE_COL_PANEL) {
        groove_row_action();
        return;
    }

    // A on an EMPTY cell inserts the item you last edited. That is what makes A,A meaningful: press
    // A once to lay down the last chain again, press it twice to get a fresh one.
    Project& p = host_.edit_project();
    hasInsertPos_ = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE: {
            if (s_.cursorColumn != 1 || s_.selection.active) return;
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            PhraseEditorState ps{ph};
            ps.cursorRow    = s_.cursorRow;
            ps.cursorColumn = s_.cursorColumn;
            // A on a note that is already there inserts nothing, but holding it is how you listen.
            if (!phrase_.cursor_context(ps).capabilities.isEmpty) {
                preview_held_note();
                return;
            }

            songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            step.note       = s_.lastEditedNote;
            step.instrument = s_.lastEditedInstrument;
            step.volume     = s_.lastEditedVolume;
            mark_modified();

            // Arm A,A (D1): a second press on this same note cell keeps the note the first A just laid
            // down and re-points it at the next FREE instrument — mirroring chain/song A,A, which
            // advance the chain/phrase ref. Only the NOTE column (col 1) arms.
            hasInsertPos_ = true;
            insertScreen_ = ScreenType::PHRASE;
            insertRow_    = s_.cursorRow;
            insertCol_    = s_.cursorColumn;

            preview_held_note();
            break;
        }

        case ScreenType::CHAIN: {
            Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];
            if (chain_row_empty(chain, s_.cursorRow)) {
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = s_.lastEditedPhrase;
                chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
                mark_modified();
                hasInsertPos_ = true;   // arm A,A — a second press here inserts the next UNUSED phrase
                insertScreen_ = ScreenType::CHAIN;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        case ScreenType::SONG: {
            if (s_.selection.active) return;
            if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
            songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
            while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow)
                track.chainRefs.push_back(-1);

            if (track.chainRefs[static_cast<size_t>(s_.cursorRow)] == -1) {
                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedChain;
                mark_modified();
                hasInsertPos_ = true;
                insertScreen_ = ScreenType::SONG;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        // A on the end-of-pattern marker lays a step down at the default tick count — the very insert
        // the cell already declares, reached by the bare press as well as by A+RIGHT. GROOVE has one
        // editable column and nothing for a plain A to open or confirm, so that is all it can mean.
        //
        // ⚠️ Guarded on EMPTY, and that guard is the whole arm: without it a bare A on a tick that is
        // already there would STEP it, and the press that lays a groove down would also nudge it.
        case ScreenType::GROOVE:
            if (cursor_context().capabilities.isEmpty) generic_input(pt::ui::increment);
            break;

        // The two screens whose rows are BUTTONS. Nothing to insert — A *is* the action.
        case ScreenType::PROJECT:  project_action();  break;
        case ScreenType::SETTINGS: settings_action(); break;
        case ScreenType::MIDI:     midi_action();     break;
        case ScreenType::MIDI_MAP: midi_map_action(); break;

        default:
            break;
    }
}

void InputDispatcher::on_button_b() {
    // Every layer below is ARMED — B closes or answers on each of them. Here for the same reason as
    // on_button_a's: a layer added tomorrow is inert on B rather than closing the browser behind it.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::QWERTY | Overlay::THEME | Overlay::EQ |
                         Overlay::BROWSER | Overlay::RENDER)) return;

    // B closes the RENDER dialog, and there is nothing to lose by it — the panel writes nothing into
    // the project. ⚠️ Not while a render RUNS: the frame loop is inside the render, so the press
    // cannot arrive until it is over and the dialog has closed itself.
    if (render_dialog_open()) { s_.renderDialog.isOpen = false; return; }

    // B is the NO of "A=YES  B=NO", and it is checked first for the same reason A's accept is: the
    // dialog owns the buttons of the screen underneath it.
    if (confirm_open()) { confirm_cancel(); return; }

    if (qwerty_open()) { delete_char(s_.qwerty); return; }

    // ⚠️ B CLOSES THE THEME EDITOR, and there is no "are you sure" — the live theme IS the applied theme
    // (every module already draws from it; there is no apply step), so closing loses nothing that a save
    // was needed to keep. The palette survives the close, the app and — since S9 — the QUIT.
    if (theme_open()) { close_theme_editor(); return; }

    // ⚠️ B CLOSES THE EQ EDITOR — but the MAPPER holds this press until B is RELEASED
    // (`defer_b_to_release`), and cancels it outright if a B+DPAD fires in between. Without that latch
    // the slot cycle would be unreachable: B+LEFT would close the editor on B's own press and the LEFT
    // would land on the mixer behind it.
    if (eq_open()) { close_eq_editor(); return; }

    if (s_.currentScreen == ScreenType::ENVELOPE_EDITOR) {
        s_.currentScreen = ScreenType::INSTRUMENT;
        return;
    }

    if (on_browser()) {
        FileBrowserState& fb = s_.fileBrowser;

        // B is the NO of "A=YES B=NO" — it disarms whatever is armed rather than leaving the browser,
        // which is what makes SELECT+B and SELECT+A safe to press by accident. Written against the
        // MODE and not against DELETE, so a third one cannot be added and left with no way out.
        if (fb.mode != BrowserMode::NORMAL) { fb.mode = BrowserMode::NORMAL; return; }

        // Inside a file selection, B COPIES it — the same gesture as B over a grid selection below.
        if (fb.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (!files.empty()) {
                const size_t n = files.size();
                fb.fileClipboard      = std::move(files);
                fb.fileClipboardIsCut = false;
                fb.statusMessage = "CPY " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
                fb.statusSuccess = true;
            }
            fb.selectionMode   = false;
            fb.selectionAnchor = -1;
            return;
        }

        close_file_browser();
        return;
    }

    // ⚠️ B LEAVES SETTINGS — the port had no such arm until Phase 4, so the screen could only be left by
    // R+DPAD, which is not a way out any other full-screen destination makes you use.
    //
    // Its POSITION is the specification, and it is Kotlin's (AppInputDispatcher.kt:2057):
    //   • AFTER the modals. The THEME EDITOR is raised FROM this screen (SETTINGS' own A, row 9), and the
    //     EQ editor and the keyboard can be over it too — while one is up it owns B, or closing SETTINGS
    //     would yank the screen out from under it.
    //   • BEFORE the selection arm below. B inside a selection COPIES, and `return`s. Put this after it
    //     and B on SETTINGS with a live selection copies nothing (SETTINGS has no clipboard arm) and never
    //     reaches here — the screen would be stuck exactly as it was, only intermittently. Kotlin's own
    //     `exitSelectionMode()` on this path is the tell that the two CAN overlap.
    if (s_.currentScreen == ScreenType::SETTINGS) {
        s_.selection.exit();   // Kotlin: trackerController.inputController.exitSelectionMode()
        NavResult nav;
        nav.screen = s_.settingsReturnScreen;
        nav.column = s_.previousColumn;   // SETTINGS owns no column — the way out keeps the one it came in with
        go_to_screen(s_, nav);            // …and NOT a bare assignment: the port's cursors are saved/restored here
        return;
    }

    // MIDI leaves the same way and for the same reasons — and B is its ONLY way out, since it is not on
    // the R+DPAD grid. Placed beside SETTINGS so the two stay in the same position relative to the
    // modals above and the selection arm below; the argument in the comment on that block is verbatim
    // this one's.
    if (s_.currentScreen == ScreenType::MIDI) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.midiReturnScreen;
        nav.column = s_.previousColumn;
        go_to_screen(s_, nav);
        return;
    }

    // …and the mapping list leaves the same way, one level further in. It is reached only from MIDI,
    // so its way back is MIDI — but it is stored rather than written down, for the reason the two
    // blocks above store theirs.
    if (s_.currentScreen == ScreenType::MIDI_MAP) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.midiMapReturnScreen;
        nav.column = s_.previousColumn;
        go_to_screen(s_, nav);
        return;
    }

    // ⚠️ EFFECTS' TIME row: B toggles DELAY SYNC — free-running milliseconds ↔ note divisions. It has to
    // be a gesture of its OWN, because the cell's VALUE means two different things on either side of it
    // (0x40 is a delay length; 4 is a 1/16 note) and no amount of A+DPAD can express "change which of
    // those you mean".
    //
    // ⚠️ B IS COMPLETELY FREE ON THIS SCREEN, which is what makes it the right home: EFFECTS is in no arm
    // of `cycle_current_item`, `on_b_up`/`on_b_down` act only on SONG and INST_POOL, and `on_l_b` will
    // not start a selection here — so there is no B+DPAD to protect and the press needs no release latch.
    //
    // Re-clamping delayTime into 0..B on the way IN is not optional: a free time of 0xF0 is not a
    // subdivision, and an unclamped one would index past the end of the name list.
    if (s_.currentScreen == ScreenType::EFFECTS &&
        s_.effectsCursorRow == EffectModule::ROW_DLY_TIME) {
        songcore::Project& p = host_.edit_project();
        p.delaySync = !p.delaySync;
        if (p.delaySync) p.delayTime = std::min(std::max(p.delayTime, 0), 11);
        mark_modified();
        return;
    }

    // ⚠️ B on the SAMPLE EDITOR is BACK — but it asks first if there is anything to lose. The editor's
    // edits live in the ENGINE's buffer, not in the project, so leaving without saving is the one
    // gesture in the app that can silently destroy work. Three states, in order: the dialog is up (B is
    // NO — stay), the sample is modified (arm the dialog), or it is clean (just go).
    if (on_sample_editor()) {
        SampleEditorState& se = s_.sampleEditor;
        if (se.showConfirmClose)  { se.showConfirmClose = false; return; }
        if (se.isModified)        { se.showConfirmClose = true;  return; }
        close_sample_editor();
        return;
    }

    // B inside a selection COPIES it and exits — the tracker's copy gesture. Outside one, B on these
    // five screens does nothing (they are the main row; there is nowhere to go back to).
    if (!s_.selection.active) return;

    const SelectionBounds b = s_.selection.bounds();
    const Project&        p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            clip_.copy_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                    b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::CHAIN:
            clip_.copy_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::SONG:
            clip_.copy_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                  b.bottomRightColumn);
            break;
        case ScreenType::TABLE:
            clip_.copy_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        default:
            break;
    }
    s_.selection.exit();
}

void InputDispatcher::on_select() {
    // ⚠️ BARE SELECT IS HELP, AND THE KEYBOARD'S ABORT. That is the whole list, and the emptiness
    // everywhere else was kept for years to make this possible: every action a cell could want from
    // SELECT is already on the A or the B sitting on that same cell, so nothing had to be taken back
    // off users when help landed.
    //
    // ⚠️ **IT ARRIVES ON THE RELEASE, NOT THE PRESS** (ui/button_mapper.h), and that is what keeps the
    // three browser chords whole: SELECT is a MODIFIER there, so its press only arms SELECT+A/B/R and
    // any other button going down during it cancels this handler outright.
    //
    // ⚠️ NOT TO BE CONFUSED WITH THE A-DEFERRAL. `defer_a_to_release` holds A on the cells that open a
    // sub-screen so that a held A+DPAD can still dial the value underneath. That mechanism is required,
    // it is what makes those cells editable at all, and it has nothing to do with this handler.
    //
    // ⚠️ THE TWO IN-PLACE OVERLAYS ARE NAMED HERE, and that is what puts help on them: the EQ editor
    // and the theme editor stand in the module's place and leave the oscilloscope strip drawn, so the
    // panel has somewhere to go. They are also the two screens whose cell names — EQ FILL, Q, MTR BG —
    // say least on their own. SELECT does nothing else on either, so nothing is taken back.
    //
    // ⚠️⚠️ **THE BROWSER IS NAMED TOO, and it is the one arm here that can break a working gesture.**
    // SELECT is the modifier of its rename, delete and new-folder chords. That stays safe only because
    // this handler runs on a RELEASE no other press interrupted — see the comment above.
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;

    // The keyboard's ABORT — the chord alias for the button on its own action row, and the one bare
    // SELECT that duplicates nothing: B backspaces here, so without it the only way to abandon a rename
    // is to walk the cursor onto ABORT and press A.
    if (qwerty_open()) { qwerty_cancel(); return; }

    // ── HELP ─────────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ The editor's "ARE YOU SURE?" is NOT an `Overlay`, so the modal rule did not see it — and
    // SELECT is the one button `button_mapper.h` does not dismiss help on. This is the only way help
    // could come up over that dialog, so it is refused here rather than guarded again when drawing.
    if (on_sample_editor() && s_.sampleEditor.showConfirmClose) return;

    // SETTINGS > HELP picks the one form SELECT raises. FULL is the overlay everywhere; SHORT is the
    // compact panel, and a second SELECT puts it away.
    //
    // ⚠️ THE FILE BROWSER HAS NO BOX FOR THE COMPACT PANEL — nineteen file rows and two status bars fill
    // all 640×480 — so under SHORT it shows nothing. The SAMPLE EDITOR is full-screen too but does have
    // one: its WAVEFORM panel is the strip's width, and the panel stands in its place.
    switch (static_cast<HelpMode>(s_.settings.helpMode)) {
        case HelpMode::OFF:
            return;
        case HelpMode::FULL:
            s_.helpOpen = false;
            s_.helpFull = true;
            return;
        case HelpMode::SHORT:
            if (on_browser()) return;
            s_.helpOpen = !s_.helpOpen;
            return;
    }
}

void InputDispatcher::on_help_dismiss() {
    // ⚠️ **FOR THE COMPACT PANEL THE PRESS IS NOT CONSUMED — it closes help and then does its normal
    // job.** That panel stands in a box that holds no cell — the visualizer strip, or the sample
    // editor's waveform — so there is nothing under it to protect from a stray press: swallowing the
    // press would cost a button on every gesture and buy nothing.
    //
    // ⚠️⚠️ **THE FULL OVERLAY IS THE OPPOSITE**, and that half is not decided here: it covers cells, so
    // the mapper consumes the press that closes it. Clearing both flags in one place is what keeps the
    // two from ever being up together.
    s_.helpOpen = false;
    s_.helpFull = false;
}

void InputDispatcher::on_stop_preview() {
    // ⚠️ THE ONE HANDLER A CONFIRM DOES NOT OWN, and it is ARMED here to say so: a dialog raised over
    // an INSTRUMENT audition must not leave the note hanging, and silencing a note is not an edit. The
    // FX helper and the browser are armed for the same reason — the screen behind them is the one that
    // started the preview, and `previewScreen` below is what decides whether there is one to stop.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::EQ | Overlay::FX_HELPER |
                         Overlay::BROWSER)) return;

    // Only the screens that can START an audition can stop one — `stopActivePreview()`. On PHRASE it
    // is gated on the setting, because with previews off there is nothing to silence. The three
    // instrument screens always can: their START *is* an audition, and it rings out until stopped, so
    // "press any button to silence it" is the only way to end it.
    //
    // ⚠️ The BROWSER is on this list too, and it has to be: its START auditions the file under the
    // cursor and the sample rings out (no timed kill). Moving the cursor to the next file must silence
    // the last one, or scrolling a folder of kicks stacks them on top of each other.
    //
    // ⚠️ And so is the EQ EDITOR — but ONLY when it was opened over an INSTRUMENT. That is the case
    // where a preview can be ringing underneath it (the editor lets START through precisely so that it
    // can be), and its band edits sweep that held note live. Opened over the MIXER or EFFECTS there is
    // no audition to silence, and claiming otherwise would stop a preview nobody started.
    const bool eqOverInstrument =
        eq_open() && s_.eq.caller.kind == EqCallerContext::Kind::INSTRUMENT;

    const bool previewScreen = (s_.currentScreen == ScreenType::TABLE) || on_browser() ||
                               on_instrument_screen() || eqOverInstrument ||
                               (s_.currentScreen == ScreenType::PHRASE && s_.settings.notePreviewEnabled);
    if (previewScreen) host_.stop_preview();
}

void InputDispatcher::on_start() {
    // ⚠️ THE THEME AND EQ EDITORS ARE ARMED HERE TO LET START THROUGH, not to serve it: both partial
    // overlays keep the transport of the screen underneath, which is the only way to HEAR what an edit
    // is doing while you dial it (ui/app_state.h).
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
    // START is the keyboard's APPLY — the chord alias for the button on its action row.
    if (qwerty_open()) { qwerty_apply(); return; }

    // ⚠️ START on the BROWSER is not the transport either: it AUDITIONS the file under the cursor, on
    // the preview lane, decoded straight into slot 255. This is what a sample browser is FOR — hearing
    // a file before committing a slot to it — and it is the one note in the port that does not go
    // through `plan_note_on`, because a file being auditioned has no instrument to derive from
    // (songcore::preview_sample_file).
    //
    // ⚠️ A SECOND DELIBERATE DIVERGENCE FROM ANDROID, stated rather than transcribed (parity audit,
    // finding 8). Kotlin gates ONLY the `.wav` arm on where the browser was opened from
    // (`previousScreen ∈ {INSTRUMENT, INST_POOL}`, AppInputDispatcher.kt:2364) — while the very next
    // arms preview mp3/flac/ogg/opus from ANY browser. So on Android the project-LOAD browser plays
    // an .mp3 and sits silent on the .wav beside it, an asymmetry no user could predict. The port
    // previews every audible extension from every browser context: the coherent superset, kept on
    // purpose rather than ported bug-for-bug.
    if (on_browser()) {
        const BrowserItem* item = s_.fileBrowser.current();
        if (!item || item->kind != BrowserItem::Kind::FILE) return;

        const std::string ext = to_lower(item->extension);
        const bool audible = std::find(sample_extensions().begin(), sample_extensions().end(), ext) !=
                             sample_extensions().end();
        if (!audible) return;   // a .pti or an .sf2 has no waveform to play

        // A file on disk has no song cell behind it — neutral gain, and never the channel an earlier
        // audition left pointed at.
        host_.set_preview_track(-1);
        // ⚠️ An audition is a full DECODE, so a four-minute mp3 costs here exactly what it costs on a
        // real load — and this is the one the user presses casually, walking a folder. Same strip, same
        // B to stop it.
        const LoadScope previewScope(*this, now_ms_, item->displayName);
        if (!host_.preview_file(item->path)) {
            if (host_.last_load_cancelled()) return;   // stopped on purpose; nothing failed
            s_.fileBrowser.statusMessage = "PREVIEW FAILED";
            s_.fileBrowser.statusSuccess = false;
        }
        return;
    }

    // ⚠️ START on the SAMPLE EDITOR is an audition too — but it is the only one in the app that TOGGLES.
    //
    // Everywhere else a second START retriggers. Here it STOPS, because the editor is the one screen you
    // audition a four-minute loop from, and a preview with no timed kill and no way to stop it is a
    // sample you have to leave the screen to silence. (The "press any button to silence a preview" rule
    // that covers the other screens deliberately exempts this one — you are pressing buttons constantly
    // in here, and every one of them would cut the sample you are trying to listen to.)
    //
    // The toggle only engages while the TRANSPORT IS STOPPED: `playbackPosition` also tracks song voices
    // playing the same sample, so during playback START keeps its retrigger meaning rather than reading a
    // song voice as "the preview is running".
    if (on_sample_editor()) {
        if (s_.sampleEditor.showConfirmClose) return;

        if (s_.sampleEditor.playbackPosition >= 0.0f && !host_.is_playing()) {
            host_.stop_preview();
            s_.sampleEditor.playbackPosition = -1.0f;
            return;
        }

        // ⚠️ The rapid double-START guard. A pending restore from the PREVIOUS preview must land before
        // this one arms anything, or it would land in the middle of this audition and take its EQ,
        // sends and modulation back off — the dry preview undone a fifth of a second after it started.
        run_due_sample_preview_restore(/*force=*/true);

        SampleEditorState& se = s_.sampleEditor;

        previewRestoreInst_    = se.instrumentId;
        previewRestorePending_ = true;
        previewRestoreAtMs_    = now_ms_ + 100;   // Kotlin's `delay(100)`

        // The FX row is auditioned by APPLYING it for real and putting the clean audio back afterwards —
        // there is no dry/wet path through a destructive DSP chain. The backup is what makes that safe.
        // (EQ has no amount, so it always previews; the other three need a nonzero one.)
        const bool hasFxPreview = (se.fxType == SampleEditorModule::FX_EQ) ||
                                  (se.fxType <= SampleEditorModule::FX_DRIVE && se.fxValue > 0);
        host_.restore_fx_preview_backup();
        if (hasFxPreview) {
            host_.save_fx_preview_backup(se.instrumentId);
            host_.apply_sample_fx(se.instrumentId, se.fxType, se.fxValue);
        }

        host_.set_preview_track(-1);   // a waveform being edited is not in the arrangement either
        host_.preview_sample_editor(se.instrumentId, se.sourceMode, se.selectionStart, se.selectionEnd,
                                    se.totalFrames, se.pitchSemitones);
        return;
    }

    // ⚠️ **START IS NOT ALWAYS THE TRANSPORT.** On the four screens that edit a SOUND rather than an
    // arrangement — INSTRUMENT, INST.POOL, MODS and TABLE — it AUDITIONS the instrument at its own
    // root, on the preview lane, and the note rings until the next plain button press silences it. That
    // is the whole point of sitting on one of them: you are dialling a sound in and listening to it.
    //
    // It does not consult `is_playing()`, and that is deliberate: auditioning an instrument OVER a
    // running song is exactly what you want while you fit it into the mix, and the preview lane is a
    // ninth voice — it steals nothing from the eight the song is using.
    //
    // ⚠️ TABLE auditions **through the table it is showing** (`previewInstrumentWithTable(currentTable,
    // currentTable)` — the instrument id and the table id are the same number, because instrument N
    // owns table N). Without the override you would hear the instrument's own table instead of the
    // automation on the screen in front of you, which is the one thing the audition exists to check.
    //
    // ⚠️ …AND IT IS NO LONGER AT UNITY GAIN. The lane borrows the fader of the song cell you came
    // through, so a pad you can only hear through its sends auditions where it actually sits. It is
    // still a ninth voice, and still steals nothing.
    if (on_instrument_screen()) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentInstrument);
        return;
    }
    if (s_.currentScreen == ScreenType::TABLE) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentTable, /*tableIdOverride=*/s_.currentTable);
        return;
    }

    // ⚠️ **IN LIVE MODE, START ON SONG IS NOT THE TRANSPORT AT ALL — IT QUEUES.** It sits ahead of the
    // play/stop toggle rather than inside it because that is the whole difference between the two
    // modes: here the button launches the cell under the cursor, and stopping is R+START for one
    // channel or the STOP button for everything.
    if (live_song_gesture()) {
        const int track = s_.cursorColumn - 1;   // on SONG the cursor column IS the track, 1-based
        if (!host_.is_playing()) {
            // Nothing is running, so there is no boundary to wait for: this channel starts NOW and
            // the transport starts with it. The other seven begin silent, waiting to be launched.
            host_.play_song_live(s_.cursorRow, 1 << track);
            return;
        }
        // ⭐ LGPT's two-press launch, and it needs no second chord and no state of its own: the first
        // press queues for the end of the playing chain, and a second on the SAME cell promotes that
        // queue to the next phrase boundary. The slot already says which press this is.
        //
        // ⚠️ **ONLY WHILE IT IS STILL ARMED.** A launch the sequencer has already committed to a frame
        // goes on showing as queued until it is heard, which is right for the marker and wrong here:
        // promoting it then pulls it earlier and cuts short a chain the player is still listening to.
        const songcore::LiveSlot q = host_.live_queue(track);
        const bool               immediate = q.armed() && !q.stop && q.targetRow == s_.cursorRow;
        host_.queue_live(track, s_.cursorRow, immediate);
        return;
    }

    // Everywhere else START is the transport.
    if (host_.is_playing()) {
        host_.stop();
        return;
    }
    switch (s_.currentScreen) {
        // ⚠️ SONG starts at the CURSOR ROW, not at row 0 — "play from here" is the gesture, and on a
        // 200-row arrangement starting from the top every time makes the screen unusable.
        case ScreenType::SONG:  host_.play_song(s_.cursorRow); break;

        // ⚠️ …AND CHAIN PLAYS ON THE TRACK IT BELONGS TO, not on channel 1. A track is a fader, a
        // mute, a peak meter, a voice slot and every per-track FX the phrase carries — a chain
        // auditioned on track 0 is heard at another channel's level, with a VTR inside it moving
        // another channel's fader. The remembered song cell only breaks a tie between the tracks that
        // already hold this chain; the arrangement is what answers.
        case ScreenType::CHAIN:
            host_.play_chain(s_.currentChain,
                             songcore::track_of_chain(*s_.project, s_.currentChain,
                                                      remembered_song_track()));
            break;

        // ⚠️ …and the four screens with NO song cursor play the SONG, from the top. This is a bug fixed
        // in S5, not a new behaviour: the S3 comment already said "MIXER, EFFECTS, PROJECT, SETTINGS
        // start at 0" — while the code below it dropped them into the `default` and played the current
        // PHRASE. It went unnoticed because all four were placeholder screens (you could stand on one
        // and press START, and something plausible happened). What you want on the mixer is the MIX.
        case ScreenType::MIXER:
        case ScreenType::EFFECTS:
        case ScreenType::PROJECT:
        case ScreenType::SETTINGS:
        // ⭐ MIDI JOINS THE FOUR, AND IT IS THE ONE THAT MOST NEEDS TO. This screen is where the user
        // picks the cable and sets the OFFSET, and both are dialled BY EAR against a song that is
        // playing — a MIDI screen you had to leave to start the transport would make its own OFFSET row
        // untunable.
        // ⭐ …and the mapping list for the same reason, doubled: the VAL column only moves while
        // something is making sound, so a range is dialled against a playing song or not at all.
        case ScreenType::MIDI:
        case ScreenType::MIDI_MAP: host_.play_song(0); break;

        // PHRASE, GROOVE, SCALE… — Kotlin's `togglePlayback()` else-arm. The phrase is asked through
        // the chain on screen first: the same phrase may sit in five chains, and the one you are
        // inside is the only one with a gesture behind it.
        default:
            host_.play_phrase(s_.currentPhrase,
                              songcore::track_of_phrase(*s_.project, s_.currentPhrase, s_.currentChain,
                                                        remembered_song_track()));
            break;
    }
}

// ─── LIVE mode's two chords ──────────────────────────────────────────────────────────────────────
//
// ⚠️ **BOTH ARE RESERVED CHORDS TAKING ON A MEANING, NOT NEW ONES.** `button_mapper.h` has consumed
// L+START and R+START since the matrix was written — *"reserved — START must not toggle playback
// here"* — so on every screen but SONG, and in every mode but LIVE, they still do exactly nothing.
// The gate is what keeps that true: the mapper's two arms are GLOBAL, so without it L+START would
// queue a song row from inside the sample editor.

int InputDispatcher::live_row_mask(int songRow) const {
    int mask = 0;
    for (int t = 0; t < 8; ++t) {
        // chainRefs is a GROWING list, so a row past a track's end is empty rather than out of bounds.
        const std::vector<int>& refs = s_.project->tracks[static_cast<size_t>(t)].chainRefs;
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;
        if (chainId >= 0 && chainId < 256) mask |= 1 << t;
    }
    return mask;
}

bool InputDispatcher::live_row_armed(int songRow) const {
    for (int t = 0; t < 8; ++t) {
        const songcore::LiveSlot q = host_.live_queue(t);
        if (!q.stop && q.targetRow == songRow) return true;
    }
    return false;
}

void InputDispatcher::on_l_start() {
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) {
        // From a standing start the whole row launches together, on one downbeat. A cell with no
        // chain in it starts silent — the row sounds the way it looks.
        host_.play_song_live(s_.cursorRow, live_row_mask(s_.cursorRow));
        return;
    }
    host_.queue_live_row(s_.cursorRow, live_row_armed(s_.cursorRow));
}

void InputDispatcher::on_r_start() {
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) return;   // nothing is sounding, so there is nothing to queue a stop for
    const int track = s_.cursorColumn - 1;
    // The same two-press promotion the launch has: queued for the chain end, pressed again for the
    // next phrase boundary.
    const songcore::LiveSlot sq = host_.live_queue(track);
    host_.queue_live_stop(track, sq.armed() && sq.stop);
}

}  // namespace pt::ui
