// What a cell opens: the FX helper, the mapping destination picker, INSTRUMENT's buttons, the
// cells whose A or B waits for the release, and the EQ editor.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"

#include <algorithm>

namespace pt::ui {

// ─── The FX-type column, and the helper it opens ──────────────────────────────────────────────────

bool InputDispatcher::on_fx_type_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            return s_.cursorColumn == 4 || s_.cursorColumn == 6 || s_.cursorColumn == 8;
        case ScreenType::TABLE:
            return s_.tableCursorColumn == 3 || s_.tableCursorColumn == 5 ||
                   s_.tableCursorColumn == 7;
        default:
            return false;
    }
}

// ─── The mapping destination picker ──────────────────────────────────────────────────────────────

bool InputDispatcher::on_map_dest_cell() const {
    if (s_.currentScreen != ScreenType::MIDI_MAP) return false;
    const Project& p = *s_.project;
    if (s_.midiMapCursorRow < 0 ||
        s_.midiMapCursorRow >= static_cast<int>(p.midiMappings.size()))
        return false;   // the ADD row — a plain A is its whole behaviour
    return s_.midiMapCursorColumn == static_cast<int>(MapCol::GROUP) ||
           s_.midiMapCursorColumn == static_cast<int>(MapCol::PARAM);
}

void InputDispatcher::apply_map_picker_choice() {
    const songcore::MapDest* d = s_.mapPicker.selected();
    s_.mapPicker = MapPickerState{};
    if (d == nullptr || !on_map_dest_cell()) return;

    Project& p = host_.edit_project();
    // ⚠️ `take_dest` and not a field write: a new destination brings its own RANGE and clears its
    // SCOPE, which is the whole reason that function exists rather than three copies of it.
    if (songcore::take_dest(p.midiMappings[static_cast<size_t>(s_.midiMapCursorRow)], *d))
        mark_dirty_and_arm_autosave();

    // ⚠️ No cursor clamp, and that is a claim rather than an omission: the picker opens only on GROUP
    // and PARAM, and those two columns exist on every destination. It is the SCOPE column to their
    // right that comes and goes, and the cursor cannot be sitting in it here.
}

int InputDispatcher::current_fx_type_code() const {
    const Project& p = *s_.project;
    int            code = 0;

    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: code = step.fx1Type; break;
            case 6: code = step.fx2Type; break;
            case 8: code = step.fx3Type; break;
            default: break;
        }
    } else if (s_.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: code = row.fx1Type; break;
            case 5: code = row.fx2Type; break;
            case 7: code = row.fx3Type; break;
            default: break;
        }
    }
    return code;
}

// The one place the FX list's length is decided — the picker's group lists and the FX column's own step
// both read it, and a build where those two disagreed would have a cell the picker cannot name.
//
// ⚠️ TWO TRIMS OFF ONE TAIL, SO THEY NEST RATHER THAN COMBINE: `LPO` is the entry directly below the
// MIDI six, so it can only be dropped once they are (songcore/effects.h). Written as a ladder for
// that reason — a build showing MIDI shows LPO whatever `loopWindow` says.
int InputDispatcher::visible_effect_type_count() const {
    if (s_.caps.midi)       return songcore::EFFECT_TYPE_COUNT;
    if (s_.caps.loopWindow) return songcore::EFFECT_TYPE_COUNT_NO_MIDI;
    return songcore::EFFECT_TYPE_COUNT_STABLE;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: step.fx1Type = effect_code; break;
            case 6: step.fx2Type = effect_code; break;
            case 8: step.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified();
    } else if (s_.currentScreen == ScreenType::TABLE) {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: row.fx1Type = effect_code; break;
            case 5: row.fx2Type = effect_code; break;
            case 7: row.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified(/*table_touched=*/true);
    }
}

// ─── INSTRUMENT's buttons — the three cells S4 drew and could not press ──────────────────────────

bool InputDispatcher::instrument_open_at_cursor() {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::INST_POOL) {
        // A on the pool's NAME column of an EMPTY slot loads a source straight into it (M8's "tap EDIT
        // on an empty slot"). A slot that already HAS a source is managed on the INSTRUMENT screen —
        // loading over it from the pool, where you cannot see what is in it, would be too easy to do by
        // accident.
        if (s_.poolCursorColumn != 0) return false;
        const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
        const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
        // ⚠️ An EXTERNAL slot has NO source of any kind, so there is nothing to browse for. Without
        // this it would fall to the sampler arm and open the SAMPLES browser — and a file loaded there
        // would flip the slot's type back out from under the user. The pool draws no EDIT for it.
        if (!instrument_has_source_row(ins.instrumentType)) return false;
        if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;

        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }

    if (s_.currentScreen != ScreenType::INSTRUMENT) return false;

    const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    const int         row  = s_.instrumentCursorRow;
    const int         col  = s_.instrumentCursorColumn;

    if (row == instrument_fx_row(ins.instrumentType) && col == 1) {
        s_.instrumentFxCursor = 0;
        s_.currentScreen = ScreenType::INSTRUMENT_FX;
        return true;
    }

    // Row 0 — TYPE (col 1), LOAD or synth AMP (col 2), and sampler EDIT or synth FILT (col 3).
    // The instrument PRESET save/load lives on row 5.
    //
    // ⚠️ Neither button exists on EXTERNAL — it draws neither and the cursor caps at column 1 — so
    // this is the same belt-and-braces consume the SF's EDIT arm below is: unreachable by the D-pad,
    // and refusing here rather than opening a browser for a source that does not exist.
    if (row == 0 && (col == 2 || col == 3) &&
        ins.instrumentType == songcore::InstrumentType::SYNTH) {
        s_.envelopeCursor = 0;
        s_.envelopeSlot = col - 2;
        s_.currentScreen = ScreenType::ENVELOPE_EDITOR;
        return true;
    }
    if (row == 0 && col >= 2 && !instrument_has_source_row(ins.instrumentType)) return true;

    if (row == 0 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }
    // ⚠️ Samplers only, and the refusal is silent: a SoundFont has no single waveform to cut — it is a
    // bank of them, each mapped to a key range — so there is nothing for the editor to draw. The EDIT
    // button is not drawn on SF (the cursor caps at LOAD), so this arm is reached on samplers only; the
    // isSF guard stays as a belt-and-braces consume.
    if (row == 0 && col == 3) {
        if (isSF) return true;   // handled: the press is CONSUMED, it just opens nothing
        open_sample_editor();
        return true;
    }

    // Row 5 — the INSTRUMENT PRESET row. A .pti carries the whole instrument: its params, its mod slots,
    // its table, and the path to its source. SAVE (col 2) names and writes it; LOAD (col 3) browses.
    if (row == 5 && col == 2) {
        const std::string dir  = fs_.instruments_directory();
        const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                    /*clear_on_first_b=*/true);
        return true;
    }
    if (row == 5 && col == 3) {
        open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, browser_dir(BrowserDir::INSTRUMENTS),
                          {"pti"});
        return true;
    }

    // ⚠️ Row 1 (NAME) and the EQ row col 1 are NOT here — they are the two DEFERRED cells,
    // and they live in `open_sub_screen_at_cursor` with the other four. That split is Kotlin's, and it
    // is the difference between a cell whose A fires on the PRESS (these — read-only buttons with no
    // A+DPAD to protect) and one whose A must wait for the RELEASE.
    return false;
}

bool InputDispatcher::defer_a_to_release() const {
    // ⚠️ NO CELL OPENS A SUB-SCREEN WHILE A MODAL IS ALREADY UP, and this guard is Kotlin's
    // (`currentCellOpensSubScreen` opens with it). It stopped being merely tidy the moment the EQ editor
    // landed: the cursor UNDERNEATH the open editor is, by construction, always parked on the very EQ
    // cell that raised it. Without this line every plain A inside the editor would be deferred to
    // release, and the A,A window cleared, for a press that has nothing to open.
    if (any_modal_open()) return false;

    // ⚠️ The one deferred cell that OPENS NOTHING, and it is deferred for the other half of the reason
    // the rest are: on row 11 under MANUAL a plain A cuts a boundary at the playhead, and the same A is
    // held down for the slice-number step, the boundary drag and the delete. Acting on the press left a
    // stray boundary in front of every one of them.
    if (on_slice_tap_cell()) return true;

    return const_cast<InputDispatcher*>(this)->open_sub_screen_at_cursor(/*peek=*/true);
}

bool InputDispatcher::defer_b_to_release() const {
    // The EQ editor. See the header: B is both the CLOSE and the modifier of the slot cycle, so it
    // cannot act until it is known which one the user meant — and only the release says that.
    if (eq_open()) return true;

    // ⚠️ AND WHEREVER R+B IS A MUTE, for the same reason one layer out. TWO BUTTONS ARE NEVER PRESSED
    // ON THE SAME FRAME: a shoulder and a face button aimed at each other still arrive as two events,
    // in whichever order the pad reported them. Acting on B's own press means a B that lands a frame
    // ahead of its R COPIES the selection and closes it — and the R+B that was aimed at that selection
    // then mutes one channel under the cursor instead of the eight that were highlighted.
    //
    // Holding B until it comes up makes the two gestures the same either way round: R can still claim
    // the press (button_mapper's R_SHIFT arm), and a B that is let go unclaimed is the plain copy it
    // always was. It costs nothing visible — B's copy on the release of B is the same instant to a
    // hand — and it is also what stops B+UP/B+DOWN paging a selection away on the way past.
    return mute_solo_chord_live();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// THE EQ EDITOR (Phase 3 S8)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

bool InputDispatcher::open_sub_screen_at_cursor(bool peek) {
    const Project& p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PROJECT:
            // The NAME row, any column but the label. Its characters are cursor COLUMNS, each an
            // in-place CHARACTER cell — so on ONE cell A opens the keyboard, A+RIGHT walks that character
            // through the alphabet, and A+B blanks it. The sharpest case the defer latch exists for.
            if (s_.projectCursorRow == static_cast<int>(ProjectRow::NAME) &&
                s_.projectCursorColumn >= 1) {
                if (!peek) open_qwerty(QwertyContext::PROJECT_NAME, host_.project().name,
                                       "PROJECT NAME:", "", PROJECT_NAME_MAX_CHARS);
                return true;
            }
            break;

        case ScreenType::INSTRUMENT: {
            const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

            if (s_.instrumentCursorRow == 1) {
                if (!peek) {
                    // A default-named slot opens the box EMPTY rather than with "INST07" in it: you are
                    // naming the instrument, and deleting a placeholder first is six presses of nothing.
                    const std::string cur =
                        songcore::instrument_has_default_name(ins) ? "" : ins.name;
                    open_qwerty(QwertyContext::INSTRUMENT_NAME, cur, "INSTRUMENT NAME:", "");
                }
                return true;
            }
            // EXTERNAL has no EQ row, and `instrument_eq_row` answers −1 there — a row number no cursor
            // can hold — so this test goes false without a type check of its own to forget here.
            if (s_.instrumentCursorRow == instrument_eq_row(ins.instrumentType) &&
                s_.instrumentCursorColumn == 1) {
                // ⚠️ `max(0, …)`: an UNASSIGNED EQ is −1 in the project, and −1 is the engine's bypass
                // value — but it is not a SLOT, and the editor has to open ON one. Kotlin's
                // `coerceAtLeast(0)` says the same thing: opening an unassigned EQ starts you on slot 0.
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;
        }

        case ScreenType::INST_POOL:
            if (s_.poolCursorColumn == 4) {
                const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;

        case ScreenType::MIXER:
            if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 8) {
                if (!peek) open_eq_editor(std::max(0, p.masterEqSlot), EqCallerContext::master());
                return true;
            }
            break;

        case ScreenType::EFFECTS:
            if (s_.effectsCursorRow == EffectModule::ROW_REV_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.reverbInputEq), EqCallerContext::reverb_in());
                return true;
            }
            if (s_.effectsCursorRow == EffectModule::ROW_DLY_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.delayInputEq), EqCallerContext::delay_in());
                return true;
            }
            break;

        case ScreenType::SAMPLE_EDITOR:
            // Only the EQ SLOT cell (row 16, col 1, with the EQ effect selected). Column 2 is APPLY and
            // keeps its own A; A+DPAD on column 1 still dials the slot.
            if (s_.sampleEditor.cursorRow == 16 && s_.sampleEditor.cursorCol == 1 &&
                s_.sampleEditor.fxType == 3) {
                if (!peek) open_eq_editor(std::min(127, std::max(0, s_.sampleEditor.fxValue)),
                                          EqCallerContext::sample_editor_fx());
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

void InputDispatcher::open_eq_editor(int slot, EqCallerContext caller) {
    s_.eq           = EqEditorState{};
    s_.eq.isOpen    = true;
    s_.eq.slotIndex = std::min(127, std::max(0, slot));
    s_.eq.cursorRow = 0;   // BAND 1, TYPE — the top-left cell, every time
    s_.eq.caller    = caller;
}

void InputDispatcher::eq_move_cursor(int d_band, int d_param) {
    const int band  = std::min(2, std::max(0, s_.eq.cursor_band() + d_band));
    const int param = std::min(3, std::max(0, s_.eq.cursor_param() + d_param));
    s_.eq.cursorRow = band * 4 + param;
}

void InputDispatcher::apply_caller_eq_slot_change(int new_slot) {
    Project& p = host_.edit_project();

    // ⚠️ FIVE DIFFERENT PROJECT FIELDS, ONE GESTURE — and this is the whole reason `EqCallerContext`
    // exists. Cycling the slot from inside the editor has to write back to the cell that RAISED it, and
    // the editor's own state has no idea which that was unless it was told at open time.
    switch (s_.eq.caller.kind) {
        case EqCallerContext::Kind::MASTER:
            p.masterEqSlot = new_slot;
            host_.set_master_eq_slot(new_slot);
            break;
        case EqCallerContext::Kind::REVERB_IN:
            p.reverbInputEq = new_slot;
            host_.set_reverb_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::DELAY_IN:
            p.delayInputEq = new_slot;
            host_.set_delay_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::INSTRUMENT: {
            const int id = s_.eq.caller.instrId;
            if (id >= 0 && id < static_cast<int>(p.instruments.size())) {
                p.instruments[static_cast<size_t>(id)].eqSlot = new_slot;
                host_.set_instrument_eq_slot(id, new_slot);
            }
            break;
        }
        case EqCallerContext::Kind::SAMPLE_EDITOR_FX:
            // No engine call, and none is missing: the sample editor's EQ is applied DESTRUCTIVELY to
            // the buffer when its APPLY button is pressed, reading the bank at that moment. Nothing is
            // filtering live, so there is nothing to re-point.
            s_.sampleEditor.fxValue = new_slot;
            break;
    }

    // Dirty AND armed — not a bare projectVersion++. This path bypasses mark_modified on purpose
    // (its wholesale push_globals is oversized for a 100 ms-repeat band dial; the right-sized push
    // is push_eq_band_to_engine's two calls), but the bypass must not also skip the crash autosave:
    // a session whose only edits are EQ bands deserves the same protection as any other edit.
    mark_dirty_and_arm_autosave();
}

void InputDispatcher::push_eq_band_to_engine() {
    const Project& p       = *s_.project;
    const int      slot    = s_.eq.slotIndex;
    const int      bandIdx = s_.eq.cursor_band();

    if (slot < 0 || slot >= static_cast<int>(p.eqPresets.size())) return;
    const songcore::EqPreset& preset = p.eqPresets[static_cast<size_t>(slot)];
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return;
    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    // The BAND, into the engine's 128-slot bank.
    host_.set_eq_band(slot, bandIdx, band.type, band.freq, band.gain, band.q);

    // ⚠️ AND THEN THE CALLER IS RE-HANDED THE SLOT, and the second call is not redundant: `set_eq_band`
    // writes the BANK, and nothing that is filtering right now reads the bank. Re-assigning the slot is
    // what makes the consumer recompile its coefficients. Drop it and every band you dial does nothing
    // you can hear (SongcoreHost::set_eq_band has the engine's side of it).
    //
    // ⚠️ AN ANDROID BUG, FOUND HERE, AND IT IS WHY THIS GOES THROUGH `apply_caller_eq_slot_change`
    // RATHER THAN AN INLINE `when` OVER THE ENGINE SETTERS — which is what Kotlin had.
    //
    // Opening the editor on an UNASSIGNED EQ shows slot 0 (−1 is the bypass value, not a slot, so it is
    // clamped up for display). But nothing WRITES 0 into the project. So Kotlin's band edit told the
    // ENGINE "use slot 0" while `masterEqSlot` stayed at −1: you could HEAR the EQ, the mixer cell still
    // read "--", and the next save-and-reload threw it away, because the load path faithfully re-pushes
    // the −1 the project still held. The project and the engine disagreed about which slot was live —
    // which is the SAME failure S5 found from the other end (deleting a slot wrote −1 to the project and
    // never told the engine, so the EQ went on filtering).
    //
    // Editing a band therefore ADOPTS the slot: one call writes the field AND makes the engine call, so
    // the two cannot come apart. For an already-assigned slot it is the same engine call plus a no-op
    // write. Zone B, so fixed on Android too (`AppInputDispatcher.handleGenericInput`), per §4's rule.
    apply_caller_eq_slot_change(slot);
}


}  // namespace pt::ui
