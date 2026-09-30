#include "ui/modules/midi_settings.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X  = 10;    // the label column
constexpr int VALUE_X = 156;   // the value column

// ⚠️ VALUE_X IS 156 HERE AND 210 ON PROJECT, AND THE 54px IS BOUGHT FOR ONE ROW: THE DEVICE NAME.
//
// This is B4.2's finding again — *a layout constraint is a data constraint* — except that this time the
// data belongs to the operating system and cannot be reshaped to fit. A Windows MIDI port is called
// things like "Microsoft GS Wavetable Synth" (28 characters); the panel is 510px and a glyph is 17, so
// even starting at the left margin only 29 fit and starting at PROJECT's value column only 17 do. The
// longest label on this screen is "PROG CHG" (8), so the column moves left to where that label ends and
// the name gets every pixel there is.
//
// It is still not always enough, and the row TRUNCATES rather than overflowing into the panel border.
// Head-first, because a port's distinguishing word is at the front far more often than at the back
// ("loopMIDI Port" vs "Microsoft GS…"). Two ports differing only in a trailing number is the case that
// costs, and it is accepted rather than solved: 20 characters is what there is.
constexpr int VALUE_MAX_CHARS = (MidiModule::WIDTH - VALUE_X - NAME_X) / CHAR_W;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/**
 * The OFFSET row's value: a sign, two digits and its unit — "+00 MS", "-25 MS".
 *
 * ⭐ AUTO prints the derived number BESIDE the word rather than instead of it. "AUTO" alone would
 * make the one row whose job is alignment refuse to say what alignment it had chosen, on a screen
 * whose whole purpose is dialling that number against a cable.
 */
std::string offset_text(int ms, bool automatic) {
    const int a = ms < 0 ? -ms : ms;
    const std::string n = std::string(ms < 0 ? "-" : "+") + dec2(a) + " MS";
    return automatic ? "AUTO " + n : n;
}

/**
 * One device row's value: the name that is OPEN, or OFF/AUTO plus how many ports there were.
 *
 * ⭐ Shared by OUTPUT and INPUT, and it has to be: the whole point of the "OFF  02 PORTS" wording
 * (B4.3) is telling *"no device picked"* apart from *"this machine has none"*, and an INPUT row that
 * answered that question differently from OUTPUT would be the second, quieter half of the same
 * confusion. A machine can easily have two outputs and no inputs — this desk did, until loopMIDI.
 */
std::string device_text(const std::vector<std::string>& names, int index, const std::string& open_name) {
    const int count = static_cast<int>(names.size());
    if (count == 0) return "OFF  NO PORTS";

    const int         idx     = clamp(index, 0, count - 1);
    const int         devices = count > MIDI_FIRST_PORT ? count - MIDI_FIRST_PORT : 0;
    const std::string ports   = devices > 0 ? dec2(devices) + " PORTS" : "NO PORTS";
    if (idx == 0) return "OFF  " + ports;
    if (idx == 1) return Canvas::clip_text(open_name.empty() ? "AUTO  " + ports : "AUTO " + open_name,
                                           VALUE_MAX_CHARS);

    return Canvas::clip_text(names[static_cast<size_t>(idx)], VALUE_MAX_CHARS);
}

/**
 * The CTL CH cycle: `ALL` first, then the sixteen channels. Seventeen stops, no empty one.
 *
 * ⚠️ Two functions rather than one arithmetic expression at each site, because the row's STORED value
 * is not its cursor value: `ALL` is 16 on disk and 0 in the cycle, so that it is the first thing
 * A+LEFT reaches rather than sitting past channel 16.
 */
constexpr int CTL_CH_OPTIONS = 17;
int ctl_ch_index(int stored) { return stored == songcore::MIDI_CTL_CH_ALL ? 0 : stored + 1; }
int ctl_ch_stored(int index) { return index <= 0 ? songcore::MIDI_CTL_CH_ALL : index - 1; }

/**
 * The CTL CH row's value: which channels may carry a mapping knob, and — when the answer cannot see
 * the knobs that are actually arriving — where they are instead.
 *
 * ⚠️ The report is the row's only way of being self-answering. It asks for a channel number, and a
 * controller's knob channel is a thing most people have never had to know; the cable is the only
 * thing on the machine that can say it.
 */
std::string ctl_ch_text(const MidiState& s) {
    const int ch = s.settings.midiControlChannel;
    if (ch == songcore::MIDI_CTL_CH_ALL) return "ALL  MAPPED KNOBS";
    if (s.lastCcChannel >= 0 && s.lastCcChannel != ch)
        return dec2(ch + 1) + "  KNOBS ON " + dec2(s.lastCcChannel + 1);
    return dec2(ch + 1) + "  MAPPED KNOBS";
}

/** The KEYS row: one voice is MONO, more is POLY and how many tracks a chord may take. */
std::string keys_text(int voices) {
    return voices <= 1 ? "MONO" : "POLY " + std::to_string(voices);
}

/**
 * "BUF 10.7MS  CPU 8% MAX 27%". ⚠️ 29 glyphs is all the panel holds from the label column, and
 * "BUF 92.9MS  CPU 999% MAX 999%" is exactly 29 — hence the clamp at 999 and no space before MS.
 */
std::string audio_load_text(const AudioLoad& a) {
    if (a.blockFrames <= 0 || a.sampleRate <= 0) return "BUF --  CPU --";
    const int tenthsMs = (a.blockFrames * 10000 + a.sampleRate / 2) / a.sampleRate;
    const std::string buf = tenthsMs >= 1000 ? std::to_string(tenthsMs / 10)
                                             : std::to_string(tenthsMs / 10) + "." + std::to_string(tenthsMs % 10);
    const auto pct = [](int load) { return std::to_string(std::min((load + 5) / 10, 999)) + "%"; };
    return "BUF " + buf + "MS  CPU " + pct(a.meanLoad) + " MAX " + pct(a.worstLoad);
}

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MidiModule::draw(Canvas& c, int x, int y, const MidiState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + NAME_X;
    const int valueX = x + VALUE_X;

    c.draw_text("MIDI", labelX, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    const int firstRowY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const auto rowY = [&](MidiRow row) { return firstRowY + midi_row_offset_y(row, ROW_HEIGHT); };

    const auto on_row = [&](MidiRow row) { return s.cursorRow == static_cast<int>(row); };

    const auto row_of = [&](MidiRow row, const char* name, const std::string& value) {
        c.draw_text(name, labelX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, valueX, rowY(row) + TEXT_PADDING, on_row(row), t.textValue, t);
    };

    // ── OUTPUT and INPUT — the ports, or WHY there is not one ────────────────────────────────────
    //
    // ⚠️ THE PORT COUNT RIDES INSIDE THE "OFF" TEXT, and the first draft had it as a separate `nn/nn`
    // counter drawn beside the label. ⭐ **A `ptshot` of this screen is what killed that** — the counter
    // started at 146px and the value column at 156, so the two printed on top of each other and the row
    // read as garbage. Nothing else could have caught it: the module compiled, ptdispatch drove every
    // row of it green, and both numbers were individually correct.
    //
    // The fix is B4.2's again — **a layout constraint is a data constraint** — and it improves the
    // screen rather than merely fitting it. "How many ports does this machine see?" is the question you
    // ask precisely WHEN the row reads OFF and you are trying to work out why; beside a named device it
    // is noise competing for the pixels that device's name needs. So the two states say different
    // things, and neither has to share a row with the other.
    row_of(MidiRow::OUTPUT, "OUTPUT", device_text(s.deviceNames,   s.deviceIndex,   s.outOpenName));
    row_of(MidiRow::INPUT,  "INPUT",  device_text(s.inDeviceNames, s.inDeviceIndex, s.inOpenName));

    row_of(MidiRow::OFFSET,   "OFFSET",   offset_text(midi_offset_in_force(s.settings, s.autoOffsetMs),
                                                      s.settings.midiOffsetAuto));
    // ⚠️ The value says what the switch DOES, not merely that it is on — "ON  24 PPQN" is the whole of
    // this row's documentation, on a device with no manual and no tooltip. It is the same reasoning as
    // OUTPUT's port count above: a row has pixels to spare exactly when its value is the boring one.
    row_of(MidiRow::SYNC,     "SYNC",     s.settings.midiSyncOut ? "ON  24 PPQN" : "OFF");
    // ⚠️ The value spells out what the channel is FOR, for SYNC's reason one line up.
    // ⚠️ …and when the row CANNOT SEE the knobs that are arriving, it says where they are instead.
    // That is the one state a user cannot get out of on their own: the row asks for a channel number
    // and nothing else on the machine knows it. On ALL, and on the channel that matches, there is
    // nothing to report — the same pixel-budget argument as OUTPUT's port count two rows up.
    row_of(MidiRow::CTL_CH,   "CTL CH", ctl_ch_text(s));
    row_of(MidiRow::PROG_CHG, "PROG CHG", s.project.midiSendProgramChange ? "ON" : "OFF");
    row_of(MidiRow::KEYS,     "KEYS",     keys_text(s.settings.midiInVoices));
    row_of(MidiRow::VELOCITY, "VELOCITY", s.settings.midiVelocity ? "ON" : "OFF");

    // The three action rows. Drawn like PROJECT's SYSTEM and EXIT, because they are the same kind of
    // thing: a row whose whole content is what A does on it.
    //
    // ⭐ MAPPING carries the COUNT, which is the one thing about the list worth knowing from outside
    // it — and on a screen where every other row is a cable setting, "NONE YET" is what says the
    // feature exists at all.
    {
        const int n = static_cast<int>(s.project.midiMappings.size());
        row_of(MidiRow::MAPPING, "MAPPING", n > 0 ? "A: " + dec2(n) + " MAPPED" : "A: NONE YET");
    }
    row_of(MidiRow::PANIC, "PANIC", "A: ALL NOTES OFF");
    row_of(MidiRow::TEST,  "TEST",  "A: C-4 CH 1");

    // ── The status readout ───────────────────────────────────────────────────────────────────────
    //
    // ⚠️ IT EXISTS BECAUSE PANIC AND TEST BOTH SUCCEED SILENTLY, AND SO DOES NEITHER OF THEM RUNNING.
    // That is the guardrail's "a handler whose correct behaviour is silence cannot be told from one
    // that never ran", sitting on a screen instead of in a log: the whole point of TEST is to answer
    // "is there a cable" on a machine where the answer is currently a guess, so the press has to say
    // out loud that it happened — and say NO PORT when it could not.
    if (!s.statusText.empty()) {
        const int statusY = firstRowY + midi_row_offset_y(MidiRow::TEST, ROW_HEIGHT) + ROW_HEIGHT * 2;
        c.draw_text(s.statusText, labelX, statusY + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                    FONT_SCALE);
    }

    // The debug build's audio line: how long one device buffer is, and how much of that time the
    // callback spent working — on average and at worst — over the last second. At 100 % the device runs dry.
    if (s.caps.debug) {
        const int lineY = firstRowY + midi_row_offset_y(MidiRow::TEST, ROW_HEIGHT) + ROW_HEIGHT * 3;
        c.draw_text(audio_load_text(s.audioLoad), labelX, lineY + TEXT_PADDING, t.textParam,
                    CHAR_SPACING, FONT_SCALE);
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MidiModule::cursor_context(const MidiState& s) const {
    if (s.cursorColumn == 0) return cc::read_only();   // the label — unreachable, as on PROJECT

    switch (static_cast<MidiRow>(s.cursorRow)) {
        case MidiRow::OUTPUT:
            // SETTINGS' OVERLAY row's context exactly: a cycle over a list the platform supplied, with
            // index 0 meaning "none". `enum_cycle` and not `index_cycle` — cursor.h explains at length
            // why those two are not interchangeable even though they behave identically.
            return cc::enum_cycle(s.deviceIndex, static_cast<int>(s.deviceNames.size()));

        case MidiRow::INPUT:
            return cc::enum_cycle(s.inDeviceIndex, static_cast<int>(s.inDeviceNames.size()));

        case MidiRow::OFFSET: {
            // ⚠️ `empty_value` is forced OUT OF RANGE. `hex_byte`'s default is −1, and −1 is a perfectly
            // ordinary offset — one millisecond early. Left at the default, the context would report
            // `isEmpty` at that one value and A+DPAD would go dead on it: an offset you could dial past
            // but not away from, on the one screen whose purpose is dialling it.
            CursorContext c = cc::hex_byte(midi_offset_in_force(s.settings, s.autoOffsetMs), -99, 99,
                                           /*empty_value=*/-1000);
            c.largeStep = 10;   // A+UP/DOWN walks it in tens, like TEMPO
            // ⚠️ **AUTO IS NOT `isEmpty`, AND IT CANNOT BE.** The dial stays live on an automatic row —
            // it hands over the derived number as the starting point, which is what makes "nudge it
            // from where the app put it" the one gesture. Only A+B is conditional: it is the way BACK,
            // so it is offered exactly when there is something to go back from.
            c.capabilities.canDelete = !s.settings.midiOffsetAuto;
            return c;
        }

        case MidiRow::SYNC:
            return cc::toggle_binary(s.settings.midiSyncOut);

        // Shows 01..16 over a stored 0..15.
        // ⚠️⚠️ **A CYCLE OF SEVENTEEN, NOT A HEX BYTE WITH AN EMPTY STATE.** It was the latter, and the
        // cell advertised an INSERT that the write-back below did not accept — so the row sat on its
        // own empty value and A+D-PAD moved nothing, for ever. A cycle has no state to be stuck in.
        case MidiRow::CTL_CH:
            return cc::enum_cycle(ctl_ch_index(s.settings.midiControlChannel), CTL_CH_OPTIONS);

        // Eight stops: MONO, then POLY 2..8. The stored value is the voice count, 1..8.
        case MidiRow::KEYS:
            return cc::enum_cycle(clamp(s.settings.midiInVoices, 1, 8) - 1, 8);

        case MidiRow::PROG_CHG:
            return cc::toggle_binary(s.project.midiSendProgramChange);

        case MidiRow::VELOCITY:
            return cc::toggle_binary(s.settings.midiVelocity);

        // The action rows. Read-only to the generic edit path; plain A is the whole of their behaviour
        // and the dispatcher owns it — it is the only layer that can reach a cable, and the only one
        // that can change which screen is up.
        case MidiRow::MAPPING:
        case MidiRow::PANIC:
        case MidiRow::TEST:
            return cc::read_only();
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MidiInputResult MidiModule::handle_input(songcore::Project& project, SettingsValues& settings,
                                         int cursor_row, int cursor_column,
                                         const std::vector<std::string>& device_names,
                                         const std::vector<std::string>& in_device_names,
                                         const InputAction& action) const {
    MidiInputResult r;
    if (cursor_column == 0 || action.type == ActionType::NONE) return r;

    // Not an early return: OFFSET answers DELETE (A+B gives the row back to AUTO).
    const bool isSet = (action.type == ActionType::SET_VALUE);

    // The device rows: the module writes the NAME, not the index it was just handed — see the header.
    // The index is a fact about the list as it stood a moment ago; the name is the choice.
    const auto pick_device = [&](const std::vector<std::string>& names, std::string& field) {
        if (!isSet || names.empty()) return false;
        const int idx = clamp(action.value, 0, static_cast<int>(names.size()) - 1);
        const std::string& picked = names[static_cast<size_t>(idx)];
        if (picked == field) return false;
        field = picked;
        return true;
    };

    switch (static_cast<MidiRow>(cursor_row)) {
        case MidiRow::OUTPUT:
            r.deviceChanged = pick_device(device_names, settings.midiOutDevice);
            break;

        case MidiRow::INPUT:
            r.inDeviceChanged = pick_device(in_device_names, settings.midiInDevice);
            break;

        case MidiRow::OFFSET: {
            if (isSet) {
                const int ms = clamp(action.value, -99, 99);
                // ⚠️ **THE FLAG IS HALF THE STATE, AND COMPARING THE NUMBER ALONE MISSES IT.** Under
                // AUTO the dial starts from the DERIVED value while the stored one is whatever was
                // last typed — so a nudge that happens to land back on the stored number would leave
                // AUTO on and report "nothing changed", and the row would spring back under the
                // user's thumb.
                if (ms != settings.midiOffsetMs || settings.midiOffsetAuto) {
                    settings.midiOffsetMs   = ms;
                    settings.midiOffsetAuto = false;
                    r.offsetChanged         = true;
                }
            } else if (action.type == ActionType::DELETE && !settings.midiOffsetAuto) {
                // A+B, the same "clear this cell" gesture as everywhere else — here it clears the
                // user's number and gives the row back to the device.
                settings.midiOffsetAuto = true;
                r.offsetChanged         = true;
            }
            break;
        }

        case MidiRow::SYNC: {
            if (!isSet) break;
            const bool on = action.value != 0;
            if (on != settings.midiSyncOut) {
                settings.midiSyncOut = on;
                r.syncChanged        = true;
            }
            break;
        }

        case MidiRow::CTL_CH: {
            if (!isSet) break;
            settings.midiControlChannel =
                ctl_ch_stored(clamp(action.value, 0, CTL_CH_OPTIONS - 1));
            r.controlChannelChanged = true;
            break;
        }

        // Read by the frame loop every tick, so nothing is pushed from here.
        case MidiRow::KEYS:
            if (isSet) settings.midiInVoices = clamp(action.value, 0, 7) + 1;
            break;

        // Read by the frame loop every tick, like KEYS.
        case MidiRow::VELOCITY:
            if (isSet) settings.midiVelocity = (action.value != 0);
            break;

        case MidiRow::PROG_CHG:
            // ⚠️ …and THIS one dirties the SONG, where the cable rows do not. It is a `Project` field
            // that emits into the .ptp, so changing it is an edit in exactly the sense the autosave and
            // the "unsaved work" dialog mean. OUTPUT, INPUT and OFFSET are settings.json's and must not
            // be — picking a cable is not composing.
            if (!isSet) break;
            project.midiSendProgramChange = (action.value != 0);
            r.projectModified             = true;
            break;

        case MidiRow::MAPPING:
        case MidiRow::PANIC:
        case MidiRow::TEST:
            break;
    }

    return r;
}

}  // namespace pt::ui
