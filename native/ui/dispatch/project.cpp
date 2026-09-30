// The confirm dialog, NEW and LOAD, the RENDER dialog, export and resample, and the PROJECT and
// SETTINGS screens' buttons.

#include "ui/dispatch/dispatch_common.h"

#include "ui/lifecycle.h"        // the crash-recovery autosave — write / clear / load
#include "ui/navigation.h"
#include "ui/song_pointer.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {


void InputDispatcher::confirm_accept() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    // ⚠️ `arg` is read out HERE, beside the kind, because `close()` resets it — and it is the direction
    // the CHANGE_TYPE arm below needs. Reading it after the close would have silently turned every
    // confirmed A+LEFT into an A+RIGHT, which is a wrong answer that still looks like the cell working.
    const int argv = s_.confirm.arg;
    s_.confirm.close();   // FIRST — every arm below can re-open a dialog, and none should be stacked

    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:
            host_.clean_seq();
            mark_modified();
            s_.statusMessage = "SEQ CLEANED";
            s_.statusSuccess = true;
            break;

        case ConfirmDialogState::Kind::CLEAN_INST:
            // ⚠️ `clean_inst` also RELOADS the media and re-pushes the params, and both are needed:
            // compacting emptied the unused instrument slots in the DOCUMENT, but their sample and
            // SoundFont buffers are still in the engine — so without the reload the RAM would not drop
            // until the project was saved and opened again. See SongcoreHost::clean_inst.
            host_.clean_inst(fs_.samples_directory());
            mark_modified();
            {
                // The reload can fail the same way a project load can — see load_project_done.
                const int failed = host_.last_media_load().failed;
                s_.statusMessage =
                    failed > 0 ? "CLEANED: " + std::to_string(failed) + " MISSING" : "INST CLEANED";
                s_.statusSuccess = (failed == 0);
            }
            break;

        case ConfirmDialogState::Kind::NEW_PROJECT:
            start_new_project();
            break;

        case ConfirmDialogState::Kind::CHANGE_TYPE:
            // ⚠️ S4 built the TYPE toggle and then REFUSED to fire it on a slot with a source loaded,
            // because switching a sampler to a SoundFont frees its sample and there was no dialog to
            // ask first. This is that dialog. `toggle_instrument_type` is unchanged; what changed is
            // that it can now be reached destructively, having asked.
            //
            // ⚠️ The DIRECTION comes back out of the dialog (`arg`), not from a default: A+LEFT on a
            // loaded slot must still step backwards after the user says yes, and with three types
            // that is a different answer from A+RIGHT's.
            toggle_instrument_type(argv);
            break;

        case ConfirmDialogState::Kind::EXIT:
            // ⚠️ **A CONFIRMED EXIT IS A CLEAN EXIT, SO IT LEAVES NOTHING TO RECOVER.** The user was
            // shown their unsaved work, and said quit anyway — that is a decision, and the autosave has
            // to honour it. Leave the file behind and the next launch offers to restore precisely the
            // work they just chose to abandon.
            //
            // ⚠️ Which is also why the dialog SURVIVES the autosave rather than being made redundant by
            // it. The obvious "there is an autosave now, so EXIT can stop asking" is wrong twice over:
            // it removes the only way to deliberately discard a session, and it makes quitting silently
            // preserve a document the user thought they were throwing away. So EXIT still asks (S7),
            // and its YES is the app's one *clean* death.
            //
            // Everything that is NOT this — SIGTERM from a launcher's menu, a flat battery, a crash, the
            // F10 escape hatch — is an UNCLEAN death, and those are the ones `flush_autosave()` catches
            // on the way out of the frame loop. The user was never asked; the work is kept.
            autosave_clear(fs_);
            s_.shouldQuit = true;
            break;

        case ConfirmDialogState::Kind::RECOVER:
            // A = recover. The document comes back DIRTY and the file STAYS — see recover_from_autosave
            // for why both of those are deliberate. A failure has already dropped the file (below), so
            // there is nothing to clean up here.
            if (!recover_from_autosave()) autosave_clear(fs_);
            break;

        case ConfirmDialogState::Kind::NONE:
            break;
    }
}

void InputDispatcher::confirm_cancel() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    s_.confirm.close();

    // ⚠️ The ONE question whose NO is an ACTION. For the other five, "no" means the world is exactly as
    // it was and closing the box is the whole of it. Here it means *discard my unsaved work* — and a
    // discard that leaves the file on disk is not a discard: the prompt would return on the next launch,
    // and the next, about work the user has already refused. That is how a safety prompt teaches people
    // to dismiss it without reading. (Kotlin: `showRecoveryDialog = false; fileController.clearAutosave()`.)
    if (kind == ConfirmDialogState::Kind::RECOVER) autosave_clear(fs_);
}

/**
 * The editing context, back to zero — TrackerController.resetEditingContext.
 *
 * Shared by NEW and LOAD, and it is not cosmetic: leaving `currentInstrument` at 0x40 after opening a
 * document whose instruments are all empty would put INSTRUMENT on a slot the user never chose, and
 * leaving `lastEditedChain` at 0x2F would have A,A on SONG insert a chain from the PREVIOUS song.
 */
void InputDispatcher::reset_editing_context() {
    sequenceUndo_.reset(host_.project());
    s_.currentPhrase = s_.currentChain = s_.currentInstrument = 0;
    s_.currentTable  = s_.currentGroove = 0;
    s_.lastEditedPhrase = s_.lastEditedChain = s_.lastEditedTable = 0;
    s_.lastEditedInstrument = s_.lastEditedTranspose = 0;
    s_.lastEditedNote   = songcore::Note::C4();
    s_.lastEditedVolume = 0x7F;

    // …and EVERY secondary screen's own cursor, exactly the set Kotlin resets (:286–300). Resetting
    // a subset left INSTRUMENT / MIXER / EFFECTS / TABLE / GROOVE / MODS / PROJECT — and the
    // REMEMBER slots below — pointing into the PREVIOUS song after a LOAD (parity audit, finding 4).
    s_.cursorRow = 0; s_.cursorColumn = 1;
    s_.songScrollPosition = 0;
    s_.instrumentCursorRow = 0; s_.instrumentCursorColumn = 1;
    // ⚠️ BOTH halves of the mixer cursor, and the row is not optional. Resetting the column alone left
    // the pair at (OTT, track 0) or (LIM, track 0) — rows that exist only in the master strip, over a
    // column that has no such row. Nothing draws highlighted there and no edit dispatches: load a
    // project with the cursor down the master strip and the mixer came back with no cursor on it.
    s_.mixerCursorColumn = 0;
    s_.mixerMasterRow    = 0;
    s_.effectsCursorRow  = 0;
    s_.tableCursorRow = 0; s_.tableCursorColumn = 1;
    s_.grooveCursorRow = 0; s_.grooveCursorColumn = GROOVE_COL_TICK;
    s_.groovePanelRow = 0;  s_.groovePanelColumn  = 0;
    // ⚠️ The quantize pointer resets with the project, and that is the whole of its persistence — it
    // is an editing aid, not a setting, and a song that arrives with one armed would edit differently
    // from the same song opened fresh.
    s_.grooveQuantize = 0;
    s_.modCursorRow = 0; s_.modCursorPair = 0; s_.modCursorSide = 0;
    s_.projectCursorRow = 0; s_.projectCursorColumn = 1;

    // The three REMEMBER slots (Kotlin's resetCursorRememberPositions) — or REMEMBER mode restores
    // a cursor that was saved inside the previous song.
    s_.songCursorRow = 0;   s_.songCursorColumn = 1;
    s_.chainCursorRow = 0;  s_.chainCursorColumn = 1;
    s_.phraseCursorRow = 0; s_.phraseCursorColumn = 1;

    // ⚠️ NOT the SETTINGS cursor and NOT poolCursorColumn: both persist, and both survive it — the
    // pool's ROW is currentInstrument, which IS reset above, and SETTINGS has a visibility guard of
    // its own on entry. The mixer ROW is reset with its column because those two are one address.
    s_.selection = Selection{};

    // ⚠️ …AND UNDER NAV = SONG THE THREE REMEMBER SLOTS ABOVE ARE THE POINTER, so "reset to 0" aims it
    // at song row 0 / track 1 — a cell the document just loaded need not have. The entry gate then
    // refuses CHAIN, correctly and silently, and the user cannot leave SONG until they find a filled
    // cell themselves. So it is DERIVED from the arrangement here, below every reset, rather than
    // remembered. A no-op under NAV = POOL, where these are only a convenience.
    clamp_song_pointer(s_);
}

void InputDispatcher::start_new_project() {
    host_.new_project();

    // Blank document: nothing is unsaved and nothing has a path. Both matter — without the version
    // reset the very next NEW or EXIT would ask "unsaved work?" about a project nobody has touched.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();

    // …and therefore nothing to recover. A clean transition DELETES the autosave, and the deletions are
    // as load-bearing as the writes: leave the file behind here and the next launch offers to restore a
    // song the user deliberately started over from. (TrackerController.newProject: "fresh project,
    // nothing to recover".) The pending deadline goes with it — it would otherwise fire three seconds
    // from now and write the blank document straight back out.
    autosavePending_ = false;
    autosave_clear(fs_);

    reset_editing_context();

    s_.statusMessage = "NEW PROJECT";
    s_.statusSuccess = true;
}

/** A .ptp just replaced the document. Leave the browser, and forget everything about the last one. */
void InputDispatcher::load_project_done(const std::string& path) {
    // A freshly LOADED project is CLEAN — it is exactly what is on disk, so there is nothing unsaved and
    // nothing to recover. (Kotlin aligns the two versions AND clears the autosave on load, for the two
    // halves of the same reason.) ⚠️ The one load path that deliberately does NEITHER is autosave
    // recovery — see recover_from_autosave, which is the exception this comment used to promise.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath         = path;

    autosavePending_ = false;   // …or it fires 3 s from now and re-creates the file this just deleted
    autosave_clear(fs_);

    reset_editing_context();

    close_file_browser();

    // ⚠️ **A SAMPLE THAT DID NOT LOAD LEAVES ITS INSTRUMENT SILENT AND SAYS NOTHING OTHERWISE.** The
    // launch path prints a warning to stderr; the two platforms this port is aimed at have no console
    // to print it to, so without this the only symptom is a track that does not sound. It is also
    // where an out-of-memory sample lands on a 512 MB device — a path that handles the failure
    // correctly and then had nowhere to report it.
    const int failed = host_.last_media_load().failed;
    s_.statusMessage = failed > 0 ? "LOADED: " + std::to_string(failed) + " MISSING" : "LOADED";
    s_.statusSuccess = (failed == 0);
}

// ─── The RENDER dialog ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_render_dialog(RenderDialogState::Output output) {
    if (s_.isRendering) return;

    RenderDialogState& rd = s_.renderDialog;
    rd.isOpen    = true;
    rd.output    = output;
    rd.cursorRow = static_cast<int>(RenderRow::SONG_START);
    // ⚠️ The SONG screen's SAVED cursor, not the live one: this is raised from PROJECT, and the live
    // `cursorRow` is PROJECT's own row by then (go_to_screen parks the song cursor on the way out).
    rd.startRow = songcore::song_section_start(host_.project(), s_.songCursorRow);
    rd.endRow   = -1;   // AUTO — follow the section, however it is edited between now and the render
}

void InputDispatcher::render_dialog_move_cursor(int delta) {
    const int last = static_cast<int>(RenderRow::COUNT) - 1;
    s_.renderDialog.cursorRow = std::max(0, std::min(last, s_.renderDialog.cursorRow + delta));
}

void InputDispatcher::render_dialog_edit(int delta) {
    RenderDialogState& rd = s_.renderDialog;
    if (s_.isRendering) return;

    switch (static_cast<RenderRow>(rd.cursorRow)) {
        case RenderRow::SONG_START: {
            rd.startRow = std::max(0, std::min(255, rd.startRow + delta));
            // A start that has walked past a hand-typed end drags the end with it, rather than
            // leaving the panel describing a range that runs backwards.
            if (rd.endRow >= 0 && rd.endRow < rd.startRow) rd.endRow = rd.startRow;
            break;
        }
        case RenderRow::SONG_END: {
            // ⚠️ AUTO SITS BELOW THE SMALLEST NUMBER, which is SONG START — an end above the start is
            // the only end that means anything, so that is where the value column runs out and the
            // rule takes over. Stepping UP off AUTO lands on the row AUTO was resolving to, so the
            // number the panel was already showing is the number you start dialling from.
            if (rd.endRow < 0) {
                if (delta > 0) rd.endRow = render_dialog_end_row(rd, host_.project());
                break;
            }
            const int next = rd.endRow + delta;
            rd.endRow = (next < rd.startRow) ? -1 : std::min(255, next);
            break;
        }
        case RenderRow::REPEAT: {
            // Same shape: OFF sits below 2. There is no "×1" — that is what OFF says.
            const int next = rd.repeat + delta;
            rd.repeat = next < 2 ? 1 : std::min(RENDER_REPEAT_MAX, next);
            break;
        }
        case RenderRow::RENDER:
        case RenderRow::COUNT:
            break;
    }
}

void InputDispatcher::render_dialog_step_section(int delta) {
    if (s_.isRendering) return;
    RenderDialogState& rd = s_.renderDialog;
    // ⚠️ IT MOVES THE WHOLE RANGE, from any row of the panel. The gesture names a PART of the song,
    // and a part is a start and an end together — so the end goes back to AUTO rather than staying
    // pointed at a row in the section you just left.
    rd.startRow = songcore::adjacent_section_start(host_.project(), rd.startRow, delta);
    rd.endRow   = -1;
}

void InputDispatcher::render_dialog_fire() {
    if (s_.isRendering) return;
    export_song(s_.renderDialog.output == RenderDialogState::Output::STEMS);
    // ⚠️ CLOSED ON THE WAY OUT, whether it worked or not: the answer ("EXPORTED!", "STEMS: 2 OF 5")
    // is on the status line, and the status line is under this panel's dim.
    s_.renderDialog.isOpen = false;
}

void InputDispatcher::export_song(bool stems) {
    if (s_.isRendering) return;   // a second press while one runs is a mis-press, not a request

    host_.stop();                                    // the session ends; a render is not playback
    if (render_.suspend_audio) render_.suspend_audio(true);

    s_.isRendering    = true;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = stems ? "RENDERING STEMS..." : "RENDERING...";
    s_.statusSuccess  = true;
    if (render_.repaint) render_.repaint();          // …so the message is on screen before we block

    const auto progress = [this](float p) {
        s_.renderProgress = p;
        if (render_.repaint) render_.repaint();      // the EXPORT row's "43%" — a readout, not a decoration
    };

    // The panel's rows, with AUTO resolved against the project as it stands right now.
    RenderRange range;
    range.startRow = s_.renderDialog.startRow;
    range.endRow   = render_dialog_end_row(s_.renderDialog, host_.project());
    range.repeat   = s_.renderDialog.repeat;

    const ActionResult r = stems ? render_stems(host_, fs_, s_, range, progress)
                                 : render_mix(host_, fs_, s_, range, progress);

    s_.isRendering    = false;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = r.message;
    s_.statusSuccess  = r.ok;

    if (render_.suspend_audio) render_.suspend_audio(false);
}

void InputDispatcher::resample_selection(const std::string& customBaseName) {
    if (s_.isRendering) return;         // a render is already running — a second APPLY is a mis-press
    if (!s_.selection.active) return;   // the selection lapsed between opening the keyboard and APPLY

    // The selected TRACKS: SONG columns are 1-indexed (col 1 = track 0), so column − 1 is the track id,
    // clamped to the eight that exist. Kotlin: `(topLeftColumn-1..bottomRightColumn-1).filter { 0..7 }`.
    const SelectionBounds b = s_.selection.bounds();
    std::set<int> tracks;
    for (int c = b.topLeftColumn - 1; c <= b.bottomRightColumn - 1; ++c)
        if (c >= 0 && c <= 7) tracks.insert(c);
    if (tracks.empty()) return;

    // The same synchronous shape as export_song: stop the session, hand the device to the render, and
    // put the "RESAMPLING..." line on screen before we block.
    host_.stop();
    if (render_.suspend_audio) render_.suspend_audio(true);

    s_.isRendering    = true;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = "RESAMPLING...";
    s_.statusSuccess  = true;
    if (render_.repaint) render_.repaint();

    const auto progress = [this](float p) {
        s_.renderProgress = p;
        if (render_.repaint) render_.repaint();
    };

    std::string        outPath;
    const ActionResult r = render_resample(host_, fs_, b.topLeftRow, b.bottomRightRow, tracks,
                                           customBaseName, outPath, progress);

    s_.isRendering    = false;
    s_.renderProgress = 0.0f;

    if (r.ok) {
        const int instId = create_resampled_instrument(host_, outPath);
        if (instId >= 0) {
            char msg[40];
            // Kotlin: "RESAMPLED → INST 05". ASCII arrow, because the status line's 5×5 font has no
            // U+2192 — a → would draw as tofu on the very screen this is meant to reassure.
            std::snprintf(msg, sizeof(msg), "RESAMPLED -> INST %02X", instId);
            s_.statusMessage = msg;
            s_.statusSuccess = true;
            mark_modified();   // Kotlin's projectVersion++ — a new instrument is unsaved work
        } else {
            // The WAV rendered but no slot could take it (pool full, or the fresh file will not reload).
            // Kotlin sets its own text inside createResampledInstrument; one line is enough here.
            s_.statusMessage = "NO FREE INSTRUMENT";
            s_.statusSuccess = false;
        }
    } else {
        s_.statusMessage = r.message;   // "RESAMPLE FAILED"
        s_.statusSuccess = false;
    }

    if (render_.suspend_audio) render_.suspend_audio(false);
}

void InputDispatcher::project_action() {
    switch (static_cast<ProjectRow>(s_.projectCursorRow)) {
        case ProjectRow::NAME:
            // ⚠️ Nothing, and that is not a gap. A on the NAME row opens the KEYBOARD — but it is one of
            // the six DEFERRED cells, so `open_sub_screen_at_cursor` has already handled it and returned
            // before `on_button_a` ever reached this switch. The arm stays, empty and named, because a
            // silent `default:` here is how the row would quietly acquire a second, divergent opener.
            break;

        case ProjectRow::PROJECT:
            switch (s_.projectCursorColumn) {
                case 1: {   // SAVE
                    const ActionResult r = save_project(host_, fs_, s_);
                    s_.statusMessage = r.message;
                    s_.statusSuccess = r.ok;
                    break;
                }
                case 2:     // LOAD
                    open_file_browser(AppState::BrowserPurpose::LOAD_PROJECT,
                                      browser_dir(BrowserDir::PROJECTS), {"ptp"});
                    break;
                case 3:     // NEW
                    // ⚠️ Only ASK if there is something to lose. A clean project has nothing to
                    // confirm, and a dialog that always appears is a dialog nobody reads.
                    if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::NEW_PROJECT);
                    else                    start_new_project();
                    break;
                default: break;
            }
            break;

        case ProjectRow::EXPORT:
            // ⚠️ NEITHER BUTTON RENDERS ANY MORE — they open the RENDER panel, which asks WHICH rows
            // and fires from its own row. Which button was pressed is still what picks stereo WAV or
            // stems; it is simply answered on the way in rather than on the way out.
            if (s_.projectCursorColumn == 1)      open_render_dialog(RenderDialogState::Output::MIX);
            else if (s_.projectCursorColumn == 2) open_render_dialog(RenderDialogState::Output::STEMS);
            break;

        case ProjectRow::COMPACT:
            if (s_.projectCursorColumn == 1)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_SEQ);
            else if (s_.projectCursorColumn == 2)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_INST);
            break;

        case ProjectRow::SYSTEM: {
            // A shortcut INTO a screen the nav grid can also reach (SETTINGS is one of the twelve).
            // It keeps the column it came from — SETTINGS owns none, exactly as PROJECT owns none — so
            // R+UP out of it later returns to the main-row screen you were on, not to a fixed default.
            //
            // …and B's way out is a SECOND, dedicated target, captured here exactly as Kotlin captures it
            // on this same gesture (`settingsReturnScreen = currentScreen`, AppInputDispatcher.kt:1666).
            // See AppState::settingsReturnScreen for why it cannot just be `previousScreen`.
            s_.settingsReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::SETTINGS;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::MIDI: {
            // ⚠️ The row is not drawn where the build hides the MIDI surfaces, and this guard is what
            // makes that a real gate rather than a cosmetic one: PROJECT is the MIDI screen's ONLY
            // door (it is not one of the twelve R+DPAD cells), so anything that reaches this case with
            // the cap off — a stale cursor, a future caller — must be turned back here.
            if (!s_.caps.midi) break;

            // The same shortcut shape as SYSTEM above, minus the nav grid. B is the only way back.
            //
            // ⚠️ THE ENUMERATION HAPPENS HERE, ON THE WAY IN. See refresh_midi_devices(): a port list is
            // only true at the moment it is read, and this is the moment the user is about to read it.
            // ⚠️ BOTH lists, since E3 — the INPUT row is as hot-pluggable as the OUTPUT one, and a
            // screen that refreshed one of them would show a stale answer on the other for as long as
            // the user stayed on it.
            refresh_midi_devices();
            refresh_midi_in_devices();
            s_.midiStatusText.clear();   // last visit's "TEST SENT" is not this visit's news
            s_.midiReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::MIDI;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::EXIT:
            // ⚠️ The shell only — and gated on the same question NEW asks. It still asks, now that S10
            // has built the autosave, and that is deliberate: the dialog is the app's ONE way to
            // deliberately throw a session away, and its YES is the app's one clean death (which is why
            // confirm_accept's EXIT arm deletes the autosave). Everything else — the launcher's kill, a
            // flat battery, F10 — is unclean, and the work is kept.
            if (!s_.caps.appExit) break;
            if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::EXIT);
            else                    s_.shouldQuit = true;
            break;

        // TAP — the TEMPO row's second cell, and A on it alone.
        //
        // ⚠️⚠️ **THE COLUMN GUARD IS THE FEATURE, NOT A TIDY-UP.** This arm first ran on the whole
        // row, and it counted every A+UP the user pressed to nudge the BPM: the mapper fires the
        // plain-A handler on A's OWN PRESS, so a modifier held down to edit a value is also a bare A
        // as far as this switch can tell. Reported from the device — "even when i just want to edit
        // bpm by A+DPAD it changes tempo". The tap needs a cell nothing else is aimed at.
        case ProjectRow::TEMPO:
            if (s_.projectCursorColumn == 2) tap_tempo();
            break;

        // TRANSPOSE is an A+DPAD cell. Plain A does nothing on it, as it does nothing on any other
        // value cell in the app.
        default:
            break;
    }
}

void InputDispatcher::tap_tempo() {
    const long long now = now_ms_;

    // A gap this long is a pause, not a beat — start counting again from this tap.
    if (tapTempoLastMs_ == 0 || now - tapTempoLastMs_ > TAP_TEMPO_TIMEOUT_MS) {
        tapTempoLastMs_ = now;
        tapTempoCount_  = 0;
        return;
    }

    const long long gap = now - tapTempoLastMs_;
    // A bounce, or two fingers on one press. Keep the anchor where it was so the NEXT tap still
    // measures from the last real one rather than from the bounce.
    if (gap < TAP_TEMPO_MIN_MS) return;
    tapTempoLastMs_ = now;

    // Shift the ring, newest last. Four entries is small enough that moving them beats the arithmetic
    // of a write cursor, and it keeps the average a plain sum over `tapTempoCount_`.
    for (int i = TAP_TEMPO_KEEP - 1; i > 0; --i) tapTempoGaps_[i] = tapTempoGaps_[i - 1];
    tapTempoGaps_[0] = gap;
    if (tapTempoCount_ < TAP_TEMPO_KEEP) ++tapTempoCount_;

    long long sum = 0;
    for (int i = 0; i < tapTempoCount_; ++i) sum += tapTempoGaps_[i];
    const long long meanMs = sum / tapTempoCount_;
    if (meanMs <= 0) return;

    // Rounded, not truncated: 120 BPM taps in as a mean of 500 ms and must come out as 120, and a
    // gap one millisecond either side of that must not read as 119.
    const int bpm = static_cast<int>((60000 + meanMs / 2) / meanMs);

    Project& p = host_.edit_project();
    const int clamped = std::min(999, std::max(20, bpm));
    if (p.tempo == clamped) return;   // no edit, so no dirty bump and no lookahead rollback
    p.tempo = clamped;
    mark_modified();
}

void InputDispatcher::settings_action() {
    switch (static_cast<SettingsRow>(s_.settingsCursorRow)) {
        case SettingsRow::THEME:
            // The arrow S7 drew as a promise. A opens the editor (S9) — the last screen in the Kotlin
            // dispatcher the port had not reached.
            open_theme_editor();
            break;

        case SettingsRow::TEMPLATE: {
            if (s_.settingsCursorColumn != 1 && s_.settingsCursorColumn != 2) break;
            const ActionResult r = (s_.settingsCursorColumn == 1) ? save_template(host_, fs_)
                                                                  : clear_template(fs_);
            s_.statusMessage = r.message;
            s_.statusSuccess = r.ok;
            break;
        }

        // Every other row is a VALUE, and a value changes with A+DPAD. Kotlin says so in a comment at
        // the top of SettingsModule ("Single A is reserved for actions only"), and it is why this
        // switch has exactly two arms.
        default:
            break;
    }
}

}  // namespace pt::ui
