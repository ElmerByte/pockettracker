// The held-button chords: A, B, R and L with the D-pad, delete, insert, clone, MUTE and SOLO.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/navigation.h"
#include "ui/song_pointer.h"     // NAV = SONG — the pointer, the entry gate and the load-time clamp

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

/** A phrase nobody has written a note into. */
bool phrase_is_blank(const Phrase& p) {
    for (const songcore::PhraseStep& s : p.steps)
        if (!songcore::step_is_empty(s)) return false;
    return true;
}

/** A chain that references no phrase. */
bool chain_is_blank(const Chain& c) {
    for (const int ref : c.phraseRefs)
        if (ref != -1) return false;
    return true;
}

/**
 * Kotlin's `((start..255) + (0 until start)).firstOrNull { pred }` — search forward from `start`,
 * wrapping once. Returns −1 when the pool is full.
 *
 * The wrap is the point: inserting the next unused phrase should hand you one NEAR the one you were
 * just editing, not slot 0 every time. Starting at `lastEdited + 1` and wrapping is what does that.
 */
template <typename Pred>
int first_from_wrapping(int start, int count, Pred pred) {
    for (int i = start; i < count; ++i)
        if (pred(i)) return i;
    for (int i = 0; i < start && i < count; ++i)
        if (pred(i)) return i;
    return -1;
}

/** Phrase IDs any chain references — "used" even when blank (a silent spacer inside a pad chain). */
std::set<int> used_phrase_ids(const Project& p) {
    std::set<int> used;
    for (const Chain& c : p.chains)
        for (const int ref : c.phraseRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

/** Chain IDs any song track references — same "used even if blank" reasoning. */
std::set<int> used_chain_ids(const Project& p) {
    std::set<int> used;
    for (const songcore::Track& t : p.tracks)
        for (const int ref : t.chainRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

}  // namespace

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ `on_a_b` below shares a NAME with the free cursor.h handler it calls, and inside a member
// function unqualified lookup finds the MEMBER first — `generic_input(on_a_b)` would try to pass the
// method to itself. The `pt::ui::` qualification forces namespace-scope lookup and is load-bearing,
// not decoration. The other four free handlers are named for the STEP (`increment` / `decrement` /
// `increment_fast` / `decrement_fast`), so nothing shadows them — but they are qualified alongside
// `on_a_b` so the four arms of one gesture read the same way.
//
// ⚠️ WHICH CHORD FIRES WHICH STEP IS THE SPECIFICATION, and it follows LGPT: the D-pad's HORIZONTAL
// axis is the small step and the VERTICAL axis the large one. A+RIGHT/A+LEFT is ±1, A+UP/A+DOWN is
// ±`largeStep`. Everything a screen overrides below keeps that split — the sample editor's fine and
// coarse nudges, the theme editor's ±0x01 and ±0x10, the INSTRUMENT TYPE cycle (a ±1, so horizontal).
// The one gesture that is not a step at all, the FX picker, stays on the vertical axis.

/**
 * A+DPAD on the INSTRUMENT screen's TYPE cell. Switching a slot's type FREES whatever source it
 * holds — a sampler has no use for an .sf2 and vice versa — so a loaded slot is asked about through
 * the confirm dialog first, and only an EMPTY slot switches outright.
 *
 * ⚠️ Silently dropping a loaded sample because the user nudged A+RIGHT one cell too far is the failure
 * this shape exists to prevent. The dialog is the guard; do not add a path around it.
 */
void InputDispatcher::request_instrument_type_toggle(int delta) {
    const Instrument& ins =
        host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];

    if (ins.sampleFilePath.has_value() || ins.soundfontPath.has_value()) {
        s_.confirm.open(ConfirmDialogState::Kind::CHANGE_TYPE, delta);
        return;
    }
    toggle_instrument_type(delta);   // an empty slot has nothing to lose — switch it outright
}

/**
 * Step the TYPE cell by `delta`, wrapping through the types this build offers.
 *
 * A+RIGHT and A+LEFT have to disagree about direction or a cell with three stops can only ever be walked
 * forwards. The cycle runs on a COUNT rather than a chain of ternaries, so a fourth type joins it by
 * existing.
 *
 * A build with MIDI authoring hidden skips EXTERNAL while retaining the saved enum values. An
 * instrument already set to EXTERNAL in a project or preset still draws as EXTERNAL.
 */
void InputDispatcher::toggle_instrument_type(int delta) {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

    const int cur   = static_cast<int>(ins.instrumentType);
    const int step  = delta < 0 ? -1 : +1;
    int candidate = cur;
    do {
        candidate = (candidate + step + songcore::INSTRUMENT_TYPE_COUNT) % songcore::INSTRUMENT_TYPE_COUNT;
    } while (!s_.caps.midi && candidate == static_cast<int>(songcore::InstrumentType::EXTERNAL));
    const auto next = static_cast<songcore::InstrumentType>(candidate);

    // The name the slot would have adopted from the source it is ABOUT to lose — read before the
    // change, exactly as a source load reads it. See the adopt rule at the end of browser activation:
    // this is the same question asked at the other end of the same slot's life.
    const std::string previousAutoName = instrument_auto_name(host_.project(), s_.currentInstrument);

    host_.set_instrument_type(s_.currentInstrument, next);

    // ⚠️⚠️ **A TYPE CHANGE DROPS THE SOURCE, SO A NAME TAKEN FROM THAT SOURCE HAS TO GO WITH IT — AND
    // THE SECOND HALF IS THE ONE THAT BIT.** An orphaned adopted name does not merely mislead: it no
    // longer matches what the slot's new type would auto-name, so the adopt rule on the NEXT load
    // reads it as a name the user TYPED and keeps it. The slot then wears the first file's name
    // through every later load, and the only way out was to blank the name by hand.
    //
    // ⚠️ A name the user really did type still survives, here as there — `previousAutoName` is what
    // tells the two apart, and it is empty for a slot that never had a source. (`ins` is still the
    // same slot: the type change rewrites it in place and never resizes the pool.)
    if (!previousAutoName.empty() && ins.name == previousAutoName)
        ins.name = songcore::default_instrument_name(ins.id);

    // The row map just changed under the cursor — the three layouts have 16, 15 and 11 rows — and the
    // cursor is sitting on row 0, which exists in all three. Its COLUMN may not: row 0 caps at 3 on a
    // sampler, 2 on a SoundFont and 1 on EXTERNAL, and the cursor is on column 1 (the TYPE cell) to
    // have reached this code at all. Nothing to clamp, then; but the SF preset readback must be
    // re-taken, and the feed does that from the path+type on the next frame.
    s_.statusMessage = std::string("TYPE: ") + songcore::instrument_type_name(next);
    s_.statusSuccess = true;
}

/** True when the cursor is on INSTRUMENT's TYPE cell, the one A+DPAD does not merely increment. */
bool InputDispatcher::on_instrument_type_cell() const {
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 0 &&
           s_.instrumentCursorColumn == 1;
}

// A+DPAD has no meaning in either modal — the keyboard's D-pad moves its key cursor (a bare press,
// handled above) and the browser's moves its file cursor. Swallowed, not passed through: an A held
// down over a browser must not reach the editor screen underneath it.

// ⚠️ On the SAMPLE EDITOR, A+DPAD on rows 3..8 does not step a cell — it DRAGS the selection's active
// edge (START on col 0, END on col 1). LEFT/RIGHT are the fine step and UP/DOWN the coarse one, both
// scaled by the ZOOM, so a nudge is always about a pixel's worth of the waveform you can actually see:
// zoomed out, UP moves a sixteenth of the sample; zoomed to 16×, it moves a sixteenth of the WINDOW.
//
// This is checked ahead of the FX helper's column test and the instrument TYPE cell, exactly where
// Kotlin checks it, because neither of those exists on this screen.
static int64_t sample_fine_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (256LL << se.zoomLevel));
}
static int64_t sample_coarse_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (16LL << se.zoomLevel));
}

// ⚠️ THE EQ ARM COMES FIRST in all five A-combo handlers, ahead of the FX helper, the sample editor's
// selection rows, the FX-type column and INSTRUMENT's type cell — which is Kotlin's order, and it is not
// merely defensive. Every one of those four questions is asked of `currentScreen`, and `currentScreen`
// is the screen UNDERNEATH the overlay. They all happen to answer "no" today (you cannot open the EQ
// from an FX column, and the editor's own cell is row 16, not the sample editor's 3..8) — which is
// exactly the kind of accident that stops being true the day someone adds an EQ cell somewhere new.
// `generic_input()` carries the real arm; these five make sure nothing gets in front of it.

// ⚠️ THE THEME ARM COMES FIRST IN ALL FOUR, and it is where the editor's whole edit lives — the module
// has no `handle_input` and no CursorContext (see generic_input). The four gestures are not symmetric:
//
//   A+LEFT / A+RIGHT → on the THEME row, step the BUILT-IN palette (prev / next).
//                      on a colour row, nudge the cursor's channel by ∓0x01.
//   A+UP   / A+DOWN  → on the THEME row, step the palette too: a list of presets has no coarse step,
//                      and a cell that can be changed at all should answer both axes.
//                      on a colour row, nudge the cursor's channel by ±0x10.

void InputDispatcher::on_a_up() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::RENDER |
                         Overlay::MAP_PICK)) return;
    if (render_dialog_open()) { render_dialog_edit(+render_dialog_coarse_step()); return; }
    if (theme_open()) {
        theme_dpad_edit(+1, +0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_up(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_up(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_code(),
                                          fx_layout_for(visible_effect_type_count()));
        return;
    }
    if (on_map_dest_cell()) {
        s_.mapPicker = map_picker_opened_at(static_cast<songcore::MapDestId>(
            s_.project->midiMappings[static_cast<size_t>(s_.midiMapCursorRow)].dest));
        return;
    }
    // The TYPE cell is a three-stop cycle with no coarse step, so both axes walk it — and both go
    // through the same request, which means both still meet the confirm dialog on a loaded slot.
    if (on_instrument_type_cell()) { request_instrument_type_toggle(+1); return; }
    selection_or_single(pt::ui::increment_fast);
}

void InputDispatcher::on_a_down() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::RENDER |
                         Overlay::MAP_PICK)) return;
    if (render_dialog_open()) { render_dialog_edit(-render_dialog_coarse_step()); return; }
    if (theme_open()) {
        theme_dpad_edit(-1, -0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_down(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_down(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_code(),
                                          fx_layout_for(visible_effect_type_count()));
        return;
    }
    if (on_map_dest_cell()) {
        s_.mapPicker = map_picker_opened_at(static_cast<songcore::MapDestId>(
            s_.project->midiMappings[static_cast<size_t>(s_.midiMapCursorRow)].dest));
        return;
    }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(-1); return; }
    selection_or_single(pt::ui::decrement_fast);
}

void InputDispatcher::on_a_left() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::RENDER |
                         Overlay::MAP_PICK)) return;
    if (render_dialog_open()) { render_dialog_edit(-1); return; }
    if (theme_open()) {
        theme_dpad_edit(-1, -0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement); return; }
    if (s_.fxHelper.isOpen) { fx_move_left(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_left(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(-1); return; }
    selection_or_single(pt::ui::decrement);
}

void InputDispatcher::on_a_right() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::RENDER |
                         Overlay::MAP_PICK)) return;
    if (render_dialog_open()) { render_dialog_edit(+1); return; }
    if (theme_open()) {
        theme_dpad_edit(+1, +0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment); return; }
    if (s_.fxHelper.isOpen) { fx_move_right(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_right(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(+1); return; }
    selection_or_single(pt::ui::increment);
}

void InputDispatcher::on_a_released() {
    // Ahead of the overlay test: it is not an overlay's gesture, and nothing can start a second
    // preview while A is down (START is refused under A), so this can only silence the one A began.
    if (heldNotePreview_) {
        heldNotePreview_ = false;
        host_.stop_preview(/*cut=*/true);
    }

    // Both pickers commit on RELEASE, not on a press — which is what lets you hold A, read your way
    // through the list, and let go on the one you want.
    if (top_overlay() == Overlay::MAP_PICK) { apply_map_picker_choice(); return; }
    if (top_overlay() != Overlay::FX_HELPER) return;
    apply_fx_type_change(s_.fxHelper.selected_effect_code());
    s_.fxHelper = FxHelperState{};
}

void InputDispatcher::on_a_deferred() {
    // The mapper is holding this press. Nothing acts here — the one thing recorded is the number that
    // will have MOVED by the time A comes back up. Every other deferred cell opens something that reads
    // no clock, so they write the "nothing was sounding" value and never look at it again.
    sliceTapPlayhead_ = on_slice_tap_cell() ? s_.sampleEditor.playbackPosition : -1.0f;
}

// ─── A+B: delete / reset ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_a_b() {
    if (overlay_swallows(Overlay::EQ)) return;

    // A+B in the EQ editor RESETS the band param under the cursor to its default — FREQ to 0x80
    // (≈450 Hz, the middle of the log sweep), GAIN to 120 (0 dB) and Q to 0x80. TYPE has no default and
    // no delete, so A+B there is inert, exactly as Kotlin's inline context leaves it.
    if (eq_open()) { generic_input(pt::ui::on_a_b); return; }

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        Project&              p = host_.edit_project();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.delete_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                          b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.delete_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.delete_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                        b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.delete_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // ⚠️ The SAMPLE EDITOR's SELECTION row (8): A+B RESETS the edge under the cursor to the sample's own
    // bound — START to 0, END to the last frame. It is the fast way back out of a selection you have
    // nudged into a corner, and the only meaning "delete" can have on a cell that cannot be empty.
    // (Rows 3..7 are the waveform, and Kotlin's arm is row 8 alone.)
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 8) {
        SampleEditorState& se = s_.sampleEditor;
        if (se.cursorCol == 0)      se.selectionStart = 0;
        else if (se.cursorCol == 1) se.selectionEnd   = se.totalFrames;
        return;
    }

    // ⚠️ The SLICE DETAIL row (11): A+B puts the boundary under the cursor back where its own method
    // would have put it — the detected position under TRANSIENT and the arithmetic cut under DIVIDE.
    // Under MANUAL there is no such position and it DELETES instead, the slices after it renumbering.
    // ONE boundary, matching row 8 above; changing the method or its parameter is what resets them all
    // (engine_feed.h).
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 11) { reset_slice_marker(); return; }

    // The pool's NAME column: A+B CLEARS the slot (M8's EDIT+OPTION). It frees the sample's PCM and,
    // if this was the SoundFont's last user, that .sf2's engine slot too — which is the whole reason it
    // is a host verb and not a field assignment. The instrument TYPE survives, so a SoundFont slot
    // stays a (now empty) SoundFont slot rather than silently becoming a sampler under the cursor.
    if (s_.currentScreen == ScreenType::INST_POOL && s_.poolCursorColumn == 0) {
        host_.clear_instrument(s_.currentInstrument);
        mark_modified();
        return;
    }

    generic_input(pt::ui::on_a_b);
}

// ─── A,A: insert the next UNUSED item ────────────────────────────────────────────────────────────

// ⚠️ A STATED SUPERSET OF KOTLIN, and the one place S8 deliberately adds a guard Kotlin does not have.
//
// Kotlin does NOT check the EQ editor in `handleAA`, `handleLA`, `handleLB`, `handleLBA` or `handleLR`.
// It gets away with it by accident: all five are gated on the screen, to SONG / CHAIN / PHRASE / TABLE /
// FILE_BROWSER — and the EQ editor can only be raised from INSTRUMENT, INST.POOL, MIXER, EFFECTS and the
// SAMPLE EDITOR. The two sets do not intersect, so every one of them is already inert under the overlay.
//
// That is a proof about today's screens, not about the gesture, and it is worth exactly nothing the day
// an EQ cell appears on a screen that has a clipboard. The guard costs a token; the accident costs a
// silent paste into a phrase you cannot see. It changes no observable behaviour on either platform
// (ptdispatch asserts the whole button set is inert under the overlay, which is the claim that actually
// matters and which holds with or without these lines).

void InputDispatcher::on_a_a() {
    if (overlay_swallows(Overlay::NONE)) return;

    // ⚠️ THE SAMPLE EDITOR'S ROW 11 NEEDS NO ARM HERE, though the fast pass it would be for is real —
    // it taps a boundary per hit, and a 16th note at 120 BPM is 125 ms, well inside the 300 ms window.
    // Its A is DEFERRED (`defer_a_to_release`) and the mapper clears `lastAPress` on every defer, so no
    // tap can reach this handler to be dropped by the insert-position gate below.

    // ⚠️ RESAMPLE is checked BEFORE the double-tap-position gate below, and it must be — Kotlin's
    // handleAA opens with the identical arm ahead of its InsertPosition logic. Under a SONG selection
    // the FIRST A press COPIED the selection rather than inserting an item, so `hasInsertPos_` was
    // never armed; the position gate would `return` and this arm would never run. Opening the RESAMPLE
    // keyboard here is the double-tap's whole meaning on a SONG selection.
    if (s_.currentScreen == ScreenType::SONG && s_.selection.active) {
        open_qwerty(QwertyContext::RESAMPLE, resample_base_name(fs_), "SAMPLE NAME:", "",
                    /*max_length=*/20, /*clear_on_first_b=*/true);
        return;
    }

    // ⚠️ TAP TEMPO COUNTS EVERY PRESS, AND ITS FAST HALF ARRIVES HERE RATHER THAN AT `on_button_a`.
    // The mapper routes a second A inside 300 ms to the double-tap handler, and 300 ms IS 200 BPM —
    // squarely inside the row's 20..999 range. Without this arm every other tap above 200 BPM would
    // be swallowed by the insert-position gate below and the tempo would settle at half what was
    // tapped. Ahead of that gate for the same reason RESAMPLE is: PROJECT never arms an insert.
    if (s_.currentScreen == ScreenType::PROJECT &&
        s_.projectCursorRow == static_cast<int>(ProjectRow::TEMPO) &&
        s_.projectCursorColumn == 2) {
        tap_tempo();
        return;
    }

    // A double-tap is only a double-tap if the cursor has not moved between the presses. Anything
    // else is two separate A presses, and each of those already did something (they inserted the
    // LAST-EDITED item — see on_button_a).
    //
    // ⚠️ The PHRASE audition is owed on BOTH exits. A second A inside 300 ms lands here and never
    // reaches `on_button_a`, so a quick re-press on a note would otherwise be held in silence.
    if (!hasInsertPos_ || insertScreen_ != s_.currentScreen || insertRow_ != s_.cursorRow ||
        insertCol_ != s_.cursorColumn) {
        preview_held_note();
        return;
    }
    hasInsertPos_ = false;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];

        const int next = first_from_wrapping(s_.lastEditedChain + 1, 256, [&](int i) {
            return chain_is_blank(p.chains[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow) track.chainRefs.push_back(-1);
        track.chainRefs[static_cast<size_t>(s_.cursorRow)] = next;
        s_.lastEditedChain                                 = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];

        const int next = first_from_wrapping(s_.lastEditedPhrase + 1, 256, [&](int i) {
            return phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = next;
        chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
        s_.lastEditedPhrase                                      = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        // D1: advance the NOTE cell's instrument to the next FREE slot, keeping the note the first A
        // laid down. `instrument_is_free` (not a bare sampleFilePath==null) is the resample-safe
        // predicate — it skips configured SoundFonts, which also have a null sampleFilePath.
        if (s_.cursorColumn != 1) return;   // the NOTE column only
        Phrase&               ph   = p.phrases[static_cast<size_t>(s_.currentPhrase)];
        songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];

        const int count = static_cast<int>(p.instruments.size());
        const int next  = first_from_wrapping(s_.lastEditedInstrument + 1, count, [&](int i) {
            return songcore::instrument_is_free(p.instruments[static_cast<size_t>(i)]);
        });
        if (next >= 0) {
            step.instrument         = next;
            s_.lastEditedInstrument = next;
            mark_modified();
        }
        preview_held_note();
    }
}

// ─── B + D-pad: which item am I looking at? ──────────────────────────────────────────────────────

void InputDispatcher::cycle_current_item(int delta) {
    // ⚠️ The THEME editor SWALLOWS B+LEFT/RIGHT rather than doing anything with it (Kotlin's
    // `cycleCurrentItem` opens with the same line). It is not that there is nothing sensible to cycle —
    // B+LEFT/RIGHT could plausibly walk the built-in palettes — it is that A+LEFT/A+RIGHT already does, and
    // a second gesture for one job is a second thing to keep in step. Without this arm the press would
    // fall through to `currentScreen`, which is SETTINGS, whose `default:` arm does nothing — so the bug
    // would be invisible today and would arrive the day an EQ cell or a pool lands on SETTINGS.
    if (theme_open()) return;

    // ⚠️ In the EQ editor B+LEFT/RIGHT changes the SLOT — and it CLAMPS at 0 and 127 where every other
    // B+LEFT/RIGHT in the app wraps. A phrase pool is a ring you scroll through; the EQ bank is an index
    // you are pointing a mixer channel at, and wrapping from slot 127 back to 0 would silently re-point
    // it at a completely different curve.
    if (eq_open()) {
        const int newSlot = std::min(127, std::max(0, s_.eq.slotIndex + delta));
        s_.eq.slotIndex   = newSlot;
        apply_caller_eq_slot_change(newSlot);
        return;
    }

    // ⭐ ON SONG, B+LEFT/RIGHT TOGGLES THE TRANSPORT MODE — LGPT's own gesture, ported literally
    // because it was free here: the NAV=SONG arm below is gated on CHAIN and PHRASE, and the pool
    // switch under it has no SONG case, so this press has always reached `default: break;` and done
    // nothing. LEFT and RIGHT both toggle rather than one each: there are two modes, so a direction
    // that could only ever confirm the mode you are already in would be a button that does nothing.
    //
    // ⚠️ GATED ON SONG ALONE. The handler is shared: an ungated arm would swallow the pool cycle on
    // six screens and the song-relative walk on two.
    if (s_.currentScreen == ScreenType::SONG) {
        host_.set_live_mode(!host_.live_mode());
        return;
    }

    // ⭐ UNDER NAV = SONG, THIS WALKS THE SONG ROW instead of the pool — the whole gesture changes
    // meaning, so it takes the press before the pool arms below ever see it. CHAIN steps to the nearest
    // FILLED cell either side; PHRASE steps to the nearest one whose chain ALSO holds a phrase at the
    // chain row you are on, which is one predicate covering both reasons a track is skipped
    // (songcore/traversal.h). Both CLAMP: nothing to that side is a press that does nothing.
    //
    // ⚠️ `chainRow` is deliberately NOT reset by the move. It may then point at an empty row of the
    // chain you land on — which the CHAIN→PHRASE entry gate already refuses, so a second guard here
    // would only be a second place to get it wrong.
    if (s_.settings.navSongRelative &&
        (s_.currentScreen == ScreenType::CHAIN || s_.currentScreen == ScreenType::PHRASE)) {
        const int requireRow = (s_.currentScreen == ScreenType::PHRASE) ? pointer_chain_row(s_) : -1;
        const int songRow    = pointer_song_row(s_);
        const int track      = songcore::next_song_cell_h(*s_.project, songRow, pointer_track(s_),
                                                          delta, requireRow);
        set_pointer_song_cell(s_, songRow, track);
        refresh_song_relative_refs(s_);
        return;
    }

    // Kotlin's `(value + delta).mod(max + 1)` — a FLOORING modulo, so −1 wraps to the top rather than
    // staying at −1 the way C's % would.
    auto wrap = [delta](int value, int max) {
        const int n = max + 1;
        return ((value + delta) % n + n) % n;
    };

    switch (s_.currentScreen) {
        case ScreenType::CHAIN:
            s_.currentChain    = wrap(s_.currentChain, 255);
            s_.lastEditedChain = s_.currentChain;
            break;
        case ScreenType::PHRASE:
            s_.currentPhrase    = wrap(s_.currentPhrase, 255);
            s_.lastEditedPhrase = s_.currentPhrase;
            break;
        case ScreenType::TABLE:
            s_.currentTable    = wrap(s_.currentTable, 127);
            s_.lastEditedTable = s_.currentTable;
            break;
        case ScreenType::GROOVE:
            s_.currentGroove = wrap(s_.currentGroove, 127);
            break;
        case ScreenType::SCALE:
            s_.currentScale = wrap(s_.currentScale, songcore::POOL_SCALES - 1);
            break;
        // INSTRUMENT and MODS cycle the same thing — the instrument — because MODS *is* a view of one.
        // (INST_POOL is absent on purpose: there, the D-PAD already selects the instrument, so B+LEFT
        // would be a second, redundant way to do it. Kotlin has the same gap for the same reason.)
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
            s_.currentInstrument    = wrap(s_.currentInstrument, 127);
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        default:
            break;
    }
}

// ⚠️ THE THEME AND EQ EDITORS ARE ARMED HERE, and their arms live inside `cycle_current_item` above
// rather than in the two lines below — one of them swallows B+LEFT/RIGHT and the other re-points the
// EQ slot with it.
void InputDispatcher::on_b_left() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(-1);
}

void InputDispatcher::on_b_right() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(+1);
}

// ⚠️ AN ANDROID BUG, FOUND BY PORTING — and it is the modal rule's own warning coming true.
//
// `handleBUp`/`handleBDown` are the ONLY two handlers in the Kotlin dispatcher that never got an
// `eqEditorState.isOpen` guard. Every other one has it. It survived because the guard is only MISSING
// where it is also needed: of the two screens these two handlers act on, SONG cannot raise the EQ
// editor at all — but INST.POOL can, from its column 4.
//
// So on Android: open the EQ from the pool's EQ column, hold B (which does NOT close the editor — the
// deferred-B latch is holding it), press UP. The B+DPAD arm fires, cancels the latch, and pages
// `currentInstrument` sixteen slots. The editor stays open on the instrument you opened it FROM (the
// caller is captured, so the bands are still right), and the pool cursor is now somewhere else
// entirely. Close it and you are looking at a different instrument than the one you were editing.
//
// Not corrupting, and that is exactly why nobody ever reported it: it reads as a mis-press. Zone B, so
// fixed on Android too (`AppInputDispatcher.handleBUp`/`handleBDown`), per §4's rule.

/**
 * B+UP/DOWN under NAV = SONG — the two directions the pool ruleset leaves FREE on both screens.
 *
 * On CHAIN it walks the track COLUMN: the nearest song row above or below whose cell in this track is
 * filled, GAPS SKIPPED. On PHRASE it walks the CHAIN's own filled rows and NEVER LEAVES THE CHAIN —
 * crossing chains vertically from there is deliberately not a gesture.
 *
 * ⚠️ Returns true even when the walk CLAMPS. The press was owned and its answer was "nothing to that
 * side"; falling through to the pool arms would page the song out from under the pointer instead.
 */
bool InputDispatcher::song_relative_b_vertical(int delta) {
    if (!s_.settings.navSongRelative) return false;

    if (s_.currentScreen == ScreenType::CHAIN) {
        const int track = pointer_track(s_);
        const int row   = songcore::next_song_cell_v(*s_.project, pointer_song_row(s_), track, delta);
        set_pointer_song_cell(s_, row, track);
        refresh_song_relative_refs(s_);
        return true;
    }
    if (s_.currentScreen == ScreenType::PHRASE) {
        const int row = songcore::next_chain_row(*s_.project, s_.currentChain, pointer_chain_row(s_),
                                                 delta, /*wrap=*/false);
        set_pointer_chain_row(s_, row);
        refresh_song_relative_refs(s_);
        return true;
    }
    return false;
}

void InputDispatcher::on_b_up() {
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(-1)) return;

    // B+UP/DOWN sets the GROOVE screen's quantize pointer from ANY cell on it — the gesture is free
    // here (it is a page jump on SONG and the pool alone) and it means the editing aid can be armed
    // without leaving the step you are editing.
    if (s_.currentScreen == ScreenType::GROOVE) {
        s_.grooveQuantize = (s_.grooveQuantize + 1) % GROOVE_QUANTIZE_COUNT;
        return;
    }

    // The pool pages by 16 like the song does — but it CLAMPS at the ends where a single D-pad step
    // wraps 00↔7F. Paging past the end of a 128-slot list should stop at the end, not lap it.
    if (s_.currentScreen == ScreenType::INST_POOL) {
        s_.currentInstrument    = std::max(0, s_.currentInstrument - 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::max(0, s_.cursorRow - 16);   // TrackerController.moveSongBigUp
    scroll_song_to_row(s_, s_.cursorRow);
}

void InputDispatcher::on_b_down() {
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(+1)) return;

    if (s_.currentScreen == ScreenType::GROOVE) {
        s_.grooveQuantize =
            (s_.grooveQuantize + GROOVE_QUANTIZE_COUNT - 1) % GROOVE_QUANTIZE_COUNT;
        return;
    }

    if (s_.currentScreen == ScreenType::INST_POOL) {
        const int last = static_cast<int>(s_.project->instruments.size()) - 1;
        s_.currentInstrument    = std::min(last, s_.currentInstrument + 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::min(255, s_.cursorRow + 16);  // moveSongBigDown
    scroll_song_to_row(s_, s_.cursorRow);
}

// ─── R + D-pad: move between screens — except where it does not ──────────────────────────────────
//
// ⚠️ On the modals R+DPAD is NOT navigation, and this is the one place the modal rule pays for itself
// several times over. In the KEYBOARD, R+UP/DOWN switches layout (letters ↔ numbers) and R+LEFT/RIGHT
// moves the TEXT cursor — four bindings that have nowhere else to live on an eight-button device. In
// the BROWSER, R+UP/DOWN cycles the SORT MODE and R+LEFT goes UP A DIRECTORY, which is what its own
// bottom bar advertises ("R+<=UP R+^v=SORT"). In the SAMPLE EDITOR R+UP/DOWN is the waveform ZOOM and
// R+LEFT/RIGHT is swallowed. In the EQ EDITOR all four are simply SWALLOWED: the overlay has no cell in
// the 5×5 grid, so there is nowhere for R+DPAD to go FROM — and letting it navigate would leave the
// editor drawn over a screen it was never opened from, still writing into the caller that raised it.
//
// None of them may fall through to `navigate_*`: a browser is a popup, not a cell in the screen grid,
// and R+RIGHT out of one would land the user on a screen with the browser's cursor state still live.

void InputDispatcher::on_r_up() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER | Overlay::RENDER)) return;
    // R+UP/DOWN steps the RENDER dialog's range to the previous or next part of the song — the one
    // gesture on that panel that moves two values at once, because a part is a start and an end.
    if (render_dialog_open()) { render_dialog_step_section(-1); return; }
    if (qwerty_open()) { s_.qwerty.layout = 0; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(+1); return; }
    // Sample-editor ZOOM IN (v0.9.4 C3): R+UP/R+DOWN drive `zoomLevel` (0=1×…4=16×) without hopping to
    // the ZOOM row. The per-frame feed re-bins the waveform off `view_start`/`view_end`, so mutating the
    // level is the whole job. (SAMPLE_EDITOR is a popup, and `navigate_up`/`navigate_down` sit still on
    // it via their default arm — unlike the horizontal pair, which do NOT: see on_r_right.)
    if (on_sample_editor()) {
        if (s_.sampleEditor.showConfirmClose) return;   // the ARE YOU SURE? dialog owns the buttons
        s_.sampleEditor.zoomLevel = std::min(s_.sampleEditor.zoomLevel + 1, 4);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_up(ns));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER | Overlay::RENDER)) return;
    if (render_dialog_open()) { render_dialog_step_section(+1); return; }
    if (qwerty_open()) { s_.qwerty.layout = 1; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(-1); return; }
    if (on_sample_editor()) {   // ZOOM OUT — see on_r_up
        if (s_.sampleEditor.showConfirmClose) return;
        s_.sampleEditor.zoomLevel = std::max(s_.sampleEditor.zoomLevel - 1, 0);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_down(ns));
    s_.selection.exit();
}

void InputDispatcher::browser_cycle_sort(int delta) {
    FileBrowserState& b = s_.fileBrowser;

    // Step through the six modes BY INDEX, which is why the enum's declaration order is behaviour
    // rather than documentation (ui/filesystem.h).
    const int next = (static_cast<int>(b.sortMode) + delta + FILE_SORT_MODE_COUNT) % FILE_SORT_MODE_COUNT;
    b.sortMode = static_cast<FileSortMode>(next);

    // ⚠️ REBUILD, not `sort_items` on what is already there — see rebuild_items. Sorting the on-screen
    // list in place would make the tie-break depend on the sort mode you happened to arrive from.
    rebuild_items(b, fs_);

    // The cursor stays where it is (Android's does — `handleRUp` copies only `sortMode`), so the row
    // under it now holds a different file. That is the point: you are re-ordering the list you are
    // looking at, not jumping somewhere.
    b.statusMessage = file_sort_label(b.sortMode);
    b.statusSuccess = true;
}

// ─── The R+LEFT/R+RIGHT deep-link (AppInputDispatcher.syncLastEditedOnScreenSwitch) ──────────────
//
// What makes SONG-over-chain-04 → R+RIGHT land ON chain 04 rather than on chain 00. TWO halves,
// transcribed from :2760–2787:
//
//   • CAPTURE — the ref under the DEPARTING screen's cursor becomes the lastEdited memory. PHRASE
//     asks the module's own cursor_context whether the cell is empty (the CELL, column and all: an
//     empty FX cell of a noted step captures nothing); CHAIN and SONG guard on `ref >= 0`.
//   • APPLY — the ARRIVING screen deep-links its current* to the matching lastEdited*.
//
// ⚠️ HORIZONTAL MOVES ONLY, and only when the screen actually changes. Kotlin's handleRUp/handleRDown
// do plain cursor save/restore + selection exit (:2695/:2716) — sync them too and the port diverges
// the other way. ptdispatch §31 pins both directions of that trap.
void InputDispatcher::sync_last_edited_on_screen_switch(ScreenType from, ScreenType to) {
    const Project& p = *s_.project;

    switch (from) {
        case ScreenType::PHRASE:
            // `currentScreen` is still the departing PHRASE here, so cursor_context() is the same
            // question Kotlin puts to phraseEditorModule.getCursorContext. No `>= 0` guard on the
            // instrument, exactly as Kotlin has none (:2772) — the CLAMP below is what makes -1 safe.
            if (!cursor_context().capabilities.isEmpty) {
                s_.lastEditedInstrument = p.phrases[static_cast<size_t>(s_.currentPhrase)]
                                              .steps[static_cast<size_t>(s_.cursorRow)]
                                              .instrument;
            }
            break;

        case ScreenType::CHAIN: {
            const int ref = p.chains[static_cast<size_t>(s_.currentChain)]
                                .phraseRefs[static_cast<size_t>(s_.cursorRow)];
            if (ref >= 0) s_.lastEditedPhrase = ref;
            break;
        }

        case ScreenType::SONG: {
            // On SONG the cursor column IS the track, 1-based — and the size guard is load-bearing,
            // not defensive: a track's chainRefs vector may be SHORTER than the 256-row screen
            // (the model's default is empty, as Kotlin's mutableListOf() is).
            const auto& refs = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)].chainRefs;
            if (s_.cursorRow < static_cast<int>(refs.size()) &&
                refs[static_cast<size_t>(s_.cursorRow)] >= 0) {
                s_.lastEditedChain = refs[static_cast<size_t>(s_.cursorRow)];
            }
            break;
        }

        default:
            break;
    }

    switch (to) {
        case ScreenType::PHRASE: s_.currentPhrase = s_.lastEditedPhrase; break;
        case ScreenType::CHAIN:  s_.currentChain  = s_.lastEditedChain;  break;
        case ScreenType::INSTRUMENT: {
            // Kotlin assigns through the currentInstrument SETTER (TrackerController.kt:167–172),
            // which coerces into the pool and mirrors the CLAMPED value back into the memory — a
            // captured -1 must land on 00, not on "slot -1". Plain fields here, so both are said.
            const int last          = static_cast<int>(p.instruments.size()) - 1;
            s_.currentInstrument    = std::min(last, std::max(0, s_.lastEditedInstrument));
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        }
        default:
            break;
    }
}

void InputDispatcher::on_r_left() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        move_text_cursor_left(s_.qwerty);
        return;
    }
    if (on_sample_editor()) return;   // see on_r_right
    if (on_browser())  { navigate_to_parent(s_.fileBrowser, fs_); return; }
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_left(ns);
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        move_text_cursor_right(s_.qwerty);
        return;
    }
    // ⚠️ SWALLOWED on the sample editor, for the reason the EQ overlay is: it has no cell in the 5×5
    // grid, so `navigate_left`/`navigate_right` fall through their `!is_main_row` arm to
    // `main_screen_for_column(screen_column(SAMPLE_EDITOR))` = `main_screen_for_column(-1)` = PHRASE.
    // That is a way OUT of the editor that bypasses ARE YOU SURE? and discards an unsaved edit
    // silently — B is the only door, and it asks.
    if (on_sample_editor()) return;
    if (on_browser())  return;   // no "down a directory" — that is what A on a folder is for
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_right(ns);
    // ⚠️ THE ENTRY GATE, and it sits ABOVE the sync rather than beside `go_to_screen`: under NAV = SONG
    // a refused press must leave NOTHING behind, and `sync_last_edited_on_screen_switch` writes the
    // lastEdited memory before the screen moves. See ui/song_pointer.h — R+RIGHT is the only gated
    // direction, because it is the only one that goes DEEPER into the arrangement.
    if (!song_relative_entry_allowed(s_, r.screen)) return;
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

// ─── L: selection and the clipboard ──────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b() {
    if (overlay_swallows(Overlay::BROWSER)) return;

    // ⚠️ The browser's selection is a DIFFERENT machine from the grid editors'. Theirs is the multi-tap
    // CELL→ROW→SCREEN widener (ui/selection.h); the browser's is a plain anchor..cursor RANGE over a
    // list, and its second tap inside the window means SELECT ALL rather than "widen the scope". They
    // share a button and a 500 ms window and nothing else, which is why they are two pieces of code.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (!b.selectionMode) {
            b.selectionMode   = true;
            b.selectionAnchor = b.cursor;
            b.lastSelectTapMs = now_ms_;
        } else if (now_ms_ - b.lastSelectTapMs <= 500) {
            // Tap again inside the window: select everything, skipping the ".." row.
            const int first = b.first_selectable();
            const int last  = std::max(static_cast<int>(b.items.size()) - 1, first);
            b.selectionAnchor = first;
            b.cursor          = last;
            b.scroll          = std::max(0, last - BROWSER_VISIBLE_ROWS + 1);
            b.lastSelectTapMs = 0;   // …so a third tap re-anchors rather than re-selecting all
        } else {
            b.selectionAnchor = b.cursor;   // the window lapsed — start a fresh range here
            b.lastSelectTapMs = now_ms_;
        }
        return;
    }

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
        case ScreenType::CHAIN:
        case ScreenType::SONG:
        case ScreenType::TABLE:
            s_.selection.handle_select_b(now_ms_, cursor_row(), cursor_column(),
                                         max_selection_column(), max_selection_row());
            break;
        default:
            break;  // GROOVE has one column and no clipboard type — nothing to select
    }
}

void InputDispatcher::on_l_a() {
    // ⚠️⚠️ THE THEME EDITOR HAS TO BE NAMED HERE. The guard answers for the layers this handler
    // serves, and leaving the editor out of the set meant the gesture was thrown away one line above
    // the arm that implements it — the lock did nothing on a device, with nothing on screen to say
    // why. Any arm below that tests for a layer must appear in this set.
    if (overlay_swallows(Overlay::THEME | Overlay::BROWSER)) return;

    // ⚠️ MUST RETURN, like every other theme-editor arm: `currentScreen` is still SETTINGS underneath,
    // and falling through would run the grid selection's cut/paste on a screen the user cannot see.
    if (theme_open()) {
        const int color = theme_color_index(s_.themeEditor.cursorRow);
        if (color >= 0) s_.themeEditor.locks.toggle(color);
        return;
    }

    // On the browser L+A is the FILE clipboard's cut/paste — the same "inside a selection it cuts,
    // outside one it pastes" shape as the grid editors below, over files instead of cells.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (b.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (files.empty()) return;
            const size_t n = files.size();

            b.fileClipboard      = std::move(files);
            b.fileClipboardIsCut = true;
            b.selectionMode      = false;
            b.selectionAnchor    = -1;
            b.statusMessage = "CUT " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
            b.statusSuccess = true;
        } else if (!b.fileClipboard.empty()) {
            browser_paste();
        }
        return;
    }

    // Inside a selection L+A CUTS; outside one it PASTES. One button, two verbs, and which one you
    // get is a function of the selection — Kotlin's `handleSelectA()`, inlined because the C++
    // selection has no InputAction to return.
    Project& p = host_.edit_project();

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.cut_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                       b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.cut_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.cut_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                     b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.cut_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // Paste. The target id is the item being edited; SONG has none (its clip carries its own tracks).
    int targetId = 0;
    switch (s_.currentScreen) {
        case ScreenType::PHRASE: targetId = s_.currentPhrase; break;
        case ScreenType::CHAIN:  targetId = s_.currentChain;  break;
        case ScreenType::TABLE:  targetId = s_.currentTable;  break;
        default:                 targetId = 0;                break;
    }
    const PasteResult r =
        clip_.paste(p, s_.currentScreen, targetId, cursor_row(), cursor_column());
    if (r.kind == PasteResult::Kind::SUCCESS && r.itemsPasted > 0) mark_modified();
}

// ─── R+A / R+B: MUTE and SOLO ────────────────────────────────────────────────────────────────────

void InputDispatcher::mute_solo_targets(int (&out)[8], int& count) const {
    count = 0;
    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            // A selection makes the chord act on every channel it covers — the reason the gesture is
            // worth having on a tracker at all: L+B+B selects the row, R+B drops the whole mix out.
            if (s_.selection.active) {
                const SelectionBounds b = s_.selection.bounds();
                for (int col = b.topLeftColumn; col <= b.bottomRightColumn; ++col)
                    if (col >= 1 && col <= 8) out[count++] = col - 1;
                return;
            }
            if (s_.cursorColumn >= 1 && s_.cursorColumn <= 8) out[count++] = s_.cursorColumn - 1;
            return;
        }
        case ScreenType::MIXER:
            // ⚠️ THE ROW IS PART OF THE ADDRESS. Row 0's columns 0..7 are the eight track faders, but
            // row 1 puts the REV and DEL send returns under those same two columns — read the column
            // alone and a chord aimed at a return lands on the track fader above it.
            //
            // ⚠️ Column 8 is the MASTER strip and has no mute of its own — the chord is a no-op there
            // rather than muting track 8, which does not exist. The selection is not consulted: it
            // belongs to the grid editors, and a stale one from SONG must not reach across.
            if (s_.mixerMasterRow == 0 && s_.mixerCursorColumn >= 0 && s_.mixerCursorColumn <= 7)
                out[count++] = s_.mixerCursorColumn;
            else if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 0)
                out[count++] = songcore::MIX_CH_REVERB;
            else if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 1)
                out[count++] = songcore::MIX_CH_DELAY;
            return;
        default:
            return;   // every other screen: the chord is the consumed no-op it has always been
    }
}

void InputDispatcher::toggle_mute_solo(bool solo) {
    // Ordering, not bookkeeping: a selection made earlier in THIS batch of events has to be seen as
    // older than the toggle about to happen. See `run_selection_recency()`.
    run_selection_recency();

    int targets[8];
    int count = 0;
    mute_solo_targets(targets, count);
    if (count == 0) return;

    Project& p = host_.edit_project();

    // ⚠️ ALL TEN PAIRS, and only on the FIRST toggle of a chord. A revert has to undo everything the
    // chord did — a selection touches several channels, and a solo changes what every other channel is
    // heard doing — and re-snapshotting per press would leave it able to undo only the last one.
    if (!mixSnapshot_.live) {
        for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
            const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
            if (!f.mute) continue;
            mixSnapshot_.mute[ch] = *f.mute;
            mixSnapshot_.solo[ch] = *f.solo;
        }
        mixSnapshot_.live = true;
    }

    for (int i = 0; i < count; ++i) {
        const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, targets[i]);
        if (!f.mute) continue;   // a channel the project does not have
        bool& flag = solo ? *f.solo : *f.mute;
        flag = !flag;
    }

    s_.lastClearable = AppState::Clearable::MUTE;

    // ⚠️ NO mark_dirty_and_arm_autosave(). Muting a channel is a PERFORMANCE action, not an edit to
    // the document: it is saved when the project is saved for some other reason, but on its own it
    // must not arm the 3 s autosave, must not make the song read as dirty, and must not put a
    // "RECOVER WORK?" in front of the next launch. `push_globals()` is what makes it audible, and it
    // sweeps all eight tracks because a solo changes the answer for the seven it was not aimed at.
    host_.push_globals();
}

void InputDispatcher::restore_full_playback() {
    Project& p = host_.edit_project();
    for (songcore::Track& t : p.tracks) { t.mute = false; t.solo = false; }
    p.reverbMute = p.reverbSolo = p.delayMute = p.delaySolo = false;
    s_.lastClearable = AppState::Clearable::NONE;
    host_.push_globals();
}

void InputDispatcher::on_r_b() {
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/false);
}

void InputDispatcher::on_r_a() {
    // ⚠️ BEFORE the mute/solo guard, which would return on any screen but SONG and MIXER and take the
    // gesture with it. The editor is an overlay standing on SETTINGS, so it never reaches that test.
    if (theme_open()) {
        if (theme_color_index(s_.themeEditor.cursorRow) >= 0) theme_roll_palette(/*rowOnly=*/true);
        return;
    }
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/true);
}

// The one question both the chord and the deferred B ask: is R+A/R+B a MUTE/SOLO here, right now?
// Derived rather than restated at the three sites, so a screen that grows the chord tomorrow starts
// deferring its B on the same day.
bool InputDispatcher::mute_solo_chord_live() const {
    if (overlay_swallows(Overlay::NONE)) return false;   // a modal owns the buttons while it is up
    return s_.currentScreen == ScreenType::SONG || s_.currentScreen == ScreenType::MIXER;
}

void InputDispatcher::on_r_combo_commit() {
    // R came up first, so what the chord did stands. Dropping the snapshot is the whole of it — the
    // next chord takes a fresh one. (A real handler rather than an absent one: the mapper's release
    // ordering is the specification, and a closer that never ran must be distinguishable from one
    // that ran and had nothing to do.)
    mixSnapshot_.live = false;
}

void InputDispatcher::on_r_combo_revert() {
    if (!mixSnapshot_.live) return;   // the chord armed on a screen that has no channels
    Project& p = host_.edit_project();
    for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
        const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
        if (!f.mute) continue;
        *f.mute = mixSnapshot_.mute[ch];
        *f.solo = mixSnapshot_.solo[ch];
    }
    mixSnapshot_.live = false;
    host_.push_globals();
}

unsigned InputDispatcher::selection_signature() const {
    unsigned h = clip_.has_data() ? 1u : 0u;
    h = h * 31u + static_cast<unsigned>(clip_.type());
    h = h * 31u + static_cast<unsigned>(clip_.width());
    h = h * 31u + static_cast<unsigned>(clip_.height());
    h = h * 31u + (s_.selection.active ? 1u : 0u);
    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        h = h * 31u + static_cast<unsigned>(b.topLeftRow);
        h = h * 31u + static_cast<unsigned>(b.topLeftColumn);
        h = h * 31u + static_cast<unsigned>(b.bottomRightRow);
        h = h * 31u + static_cast<unsigned>(b.bottomRightColumn);
    }
    return h;
}

void InputDispatcher::run_selection_recency() {
    const unsigned sig = selection_signature();
    if (sig == selectionSig_) return;
    selectionSig_    = sig;
    s_.lastClearable = AppState::Clearable::SELECTION;
}

void InputDispatcher::on_l_r() {
    if (overlay_swallows(Overlay::BROWSER)) return;
    if (on_browser()) {
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        return;
    }

    // ── ONE PRESS UNDOES ONE THING, MOST RECENT FIRST ────────────────────────────────────────────
    //
    // Two rungs: the mix (any mixer channel muted or soloed) and the selection with its buffer.
    // `s_.lastClearable` says which the user touched last, and that one is tried first — clearing
    // both at once would throw away a selection someone built press by press just because they also
    // dropped a channel out of the mix.
    //
    // ⚠️ A rung with nothing to clear FALLS THROUGH to the other. Without that, L+R reads as a dead
    // button whenever the most recent thing is already empty — and the recency flag survives the
    // clear that emptied it, so that is not a rare state.
    //
    // ⚠️ The SAMPLE_EDITOR exclusion covers this rung too, for the reason it covers the clipboard's:
    // L+R is reserved there for the editor's own selection, and one screen with two exclusion lists
    // is a special case someone has to remember.
    //
    // ⚠️ Asked over all MIX_CHANNELS through `mix_channel_flags` — the resolver the chord toggles with —
    // so the REV and DEL strips count. A walk over `p.tracks` alone leaves a return muted on its own
    // with no L+R to bring it back.
    const bool mix_touched = [&] {
        if (s_.currentScreen == ScreenType::SAMPLE_EDITOR) return false;
        Project& p = host_.edit_project();   // the resolver hands out pointers; nothing is written here
        for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
            const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
            if (f.mute && (*f.mute || *f.solo)) return true;
        }
        return false;
    }();

    if (s_.lastClearable == AppState::Clearable::MUTE && mix_touched) {
        restore_full_playback();
        return;
    }

    // Two-state rule (v0.9.4 C4), and the gate is `selection.active`, nothing else:
    //   • SELECTING → leave selection mode, but the copy buffer MUST SURVIVE — you might re-enter a
    //     selection by accident and must not lose what you copied. `selection.exit()` does not touch
    //     `clip_`, so the buffer is untouched.
    //   • NOT selecting → CLEAR the buffer. It is the only way to dismiss the top-strip clipboard
    //     readout (`Clipboard::info()`) short of a restart.
    if (s_.selection.active) {
        s_.selection.exit();   // buffer untouched
        return;
    }

    // ⚠️ A DENY-list, not an allow-list, and that is the point: the readout is drawn on the top strip
    // of EVERY screen, so scoping the clear to the four screens that can fill the buffer left it
    // undismissable from the other ten. The two exclusions above this line stand (the overlays own
    // every button; the browser's L+R cancels its own selection, which its hint bar advertises).
    //
    // ⚠️ SAMPLE_EDITOR is excluded: L+R is reserved there for the editor's own selection, and a
    // generic clear would shadow it. GROOVE and the other one-column screens are NOT excluded — a
    // clear there is a no-op that costs nothing, which beats a special case someone has to remember.
    const bool had_buffer = !clip_.info().empty();
    if (s_.currentScreen != ScreenType::SAMPLE_EDITOR) clip_.clear();

    // Nothing on the selection rung to clear, but the mix has something: take it rather than leave
    // the press doing nothing at all.
    if (!had_buffer && mix_touched) restore_full_playback();
}

// ─── L+B+A: clone ────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (overlay_swallows(Overlay::NONE)) return;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) { s_.selection.exit(); return; }
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
        const int currentChainId =
            (s_.cursorRow < static_cast<int>(track.chainRefs.size()))
                ? track.chainRefs[static_cast<size_t>(s_.cursorRow)]
                : -1;

        if (currentChainId != -1) {
            const Chain&        src         = p.chains[static_cast<size_t>(currentChainId)];
            const std::set<int> usedChains  = used_chain_ids(p);
            const std::set<int> usedPhrases = used_phrase_ids(p);

            // The destination must be a FREE chain: blank AND unreferenced.
            const int dstChainId = first_from_wrapping(currentChainId + 1, 256, [&](int i) {
                return usedChains.count(i) == 0 && chain_is_blank(p.chains[static_cast<size_t>(i)]);
            });

            // A DEEP clone: every phrase the chain references gets its own free slot, so the copy is
            // fully independent. `reserved` stops two source phrases claiming the same destination;
            // duplicate refs inside the chain map to the SAME clone, which is what keeps a chain that
            // plays phrase 5 twice still playing one phrase twice.
            std::vector<int>   srcPhraseIds;
            for (const int ref : src.phraseRefs)
                if (ref != -1 &&
                    std::find(srcPhraseIds.begin(), srcPhraseIds.end(), ref) == srcPhraseIds.end())
                    srcPhraseIds.push_back(ref);

            std::set<int>      reserved;
            std::map<int, int> phraseMap;
            bool               enoughPhrases = true;
            for (const int pid : srcPhraseIds) {
                const int slot = first_from_wrapping(0, 256, [&](int i) {
                    return reserved.count(i) == 0 && usedPhrases.count(i) == 0 &&
                           phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
                });
                if (slot < 0) { enoughPhrases = false; break; }
                reserved.insert(slot);
                phraseMap[pid] = slot;
            }

            // Capacity is checked in FULL before anything is written. Abort, never half-clone: a
            // partial clone leaves a chain pointing at phrases that were never copied.
            if (dstChainId < 0) {
                s_.statusMessage = "NO FREE CHAINS";
                s_.statusSuccess = false;
            } else if (!enoughPhrases) {
                s_.statusMessage = "NO FREE PHRASES";
                s_.statusSuccess = false;
            } else {
                for (const auto& kv : phraseMap)
                    p.phrases[static_cast<size_t>(kv.second)].steps =
                        p.phrases[static_cast<size_t>(kv.first)].steps;

                Chain& dst = p.chains[static_cast<size_t>(dstChainId)];
                for (size_t i = 0; i < src.phraseRefs.size(); ++i) {
                    const int ref     = src.phraseRefs[i];
                    dst.phraseRefs[i] = (ref == -1) ? -1 : phraseMap[ref];
                }
                dst.transposeValues = src.transposeValues;

                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = dstChainId;
                s_.lastEditedChain = dstChainId;
                s_.statusMessage   = "CHAIN CLONED";
                s_.statusSuccess   = true;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain&    chain           = p.chains[static_cast<size_t>(s_.currentChain)];
        const int currentPhraseId = chain.phraseRefs[static_cast<size_t>(s_.cursorRow)];
        if (currentPhraseId != -1) {
            const std::set<int> usedPhrases = used_phrase_ids(p);
            const int next = first_from_wrapping(currentPhraseId + 1, 256, [&](int i) {
                return usedPhrases.count(i) == 0 && phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
            });
            if (next >= 0) {
                p.phrases[static_cast<size_t>(next)].steps =
                    p.phrases[static_cast<size_t>(currentPhraseId)].steps;
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)] = next;
                s_.lastEditedPhrase                                 = next;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        const int srcPhraseId = s_.currentPhrase;
        const std::set<int> usedPhrases = used_phrase_ids(p);
        const int next = first_from_wrapping(srcPhraseId + 1, 256, [&](int i) {
            return i != srcPhraseId && usedPhrases.count(i) == 0 &&
                   phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next >= 0) {
            p.phrases[static_cast<size_t>(next)].steps =
                p.phrases[static_cast<size_t>(srcPhraseId)].steps;
            s_.currentPhrase = next;   // …and follow the clone, so you are editing the copy
            mark_modified();
        }
    }

    s_.selection.exit();
}

}  // namespace pt::ui
