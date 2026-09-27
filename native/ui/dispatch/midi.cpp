// The MIDI screens: choosing a port out and in, thru, and the mapping list's A.

#include "ui/dispatch/dispatch_common.h"

#include "ui/navigation.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── MIDI (phase B4.3) ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_port() {
    // The OFFSET first and unconditionally — it is a number the consumer needs whether or not a port
    // ever opens, and forgetting it is the "a setting that round-trips is not a setting that is
    // applied" bug in its purest form: the value would sit correct in settings.json, be drawn correctly
    // on the screen, and change nothing anybody could hear.
    host_.set_midi_offset_ms(midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
    // SYNC (phase C) for exactly the same reason, and it is the more dangerous of the two to forget:
    // OFFSET being unapplied is a few milliseconds nobody measures, but SYNC being unapplied means a
    // user who turned it on last session, saw ON when they came back, and got no clock at all.
    host_.set_midi_sync_out(s_.settings.midiSyncOut);

    refresh_midi_devices();
    if (port_open()) return;                       // the env override already opened this same device
    if (s_.midiDeviceIndex != 0) apply_midi_device();
    s_.midiStatusText.clear();                     // boot news is the console's job, not the screen's
}

// Resolve a SAVED NAME against the list that exists right now. Not found → 0 → OFF, and that rule is
// the whole reason the setting is a name and not an index: an index would silently come back pointing
// at whatever port took its place. One definition for all four call sites (both directions of both
// the OUT and IN pairs) so the not-found rule cannot be spelled differently in one of them.
//
// Index 0 is "OFF" and is never a device, so the search starts at 1.
static int resolve_port_index(const std::vector<std::string>& names, const std::string& want) {
    for (size_t i = 1; i < names.size(); ++i)
        if (names[i] == want) return static_cast<int>(i);
    return 0;
}

void InputDispatcher::refresh_midi_devices() {
    s_.midiDeviceNames.assign(1, "OFF");   // index 0, always — the module never handles "no device"

    if (s_.midiOut) {
        const int n = s_.midiOut->device_count();
        for (int i = 0; i < n; ++i) s_.midiDeviceNames.push_back(s_.midiOut->device_name(i));
    }

    s_.midiDeviceIndex = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);
}

void InputDispatcher::apply_midi_device() {
    // Re-resolve first: `settings.midiOutDevice` is the choice, `midiDeviceIndex` is where that choice
    // sits in the list the screen is drawing, and the module just changed the former.
    const int wanted = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);
    s_.midiDeviceIndex = wanted;

    if (!s_.midiOut) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    // ⚠️ THE PANIC IS OURS TO SEND — see the header. `set_out` panics on a POINTER change and the
    // pointer is not changing; only the device behind it is. Skip this and every note sounding on the
    // port we are about to close is held by that hardware until someone power-cycles it.
    host_.midi_out().panic();
    s_.midiOut->close();

    if (wanted == 0) {
        s_.midiStatusText = "OUTPUT OFF";
    } else if (s_.midiOut->open(wanted - 1)) {   // −1: index 0 of the list is OFF, not a device
        s_.midiStatusText = "PORT OPENED";
    } else {
        // ⚠️ SAID OUT LOUD, and the setting is left alone. A port that refuses to open is usually one
        // another app already holds exclusively — a transient the user can fix and retry — so throwing
        // their choice away on the first failure would be the wrong repair. The row reads OFF because
        // `is_open()` is false, which is the truth.
        s_.midiStatusText = "PORT BUSY";
    }

    // ⭐ ONE call, below every arm, because the loopback verdict depends on which port is OPEN and each
    // of the three arms above leaves that different — including "OUTPUT OFF", which is the arm that
    // turns thru back ON. A rule repeated at each site is a rule one site will forget.
    update_midi_thru();   // the OUT row's half of the ONE verdict (E4) — see apply_midi_in_device
}

// ─── MIDI IN (phase E2) ──────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_in_port() {
    // ⚠️ AT BOOT, not only when the row is touched: the channel is remembered in settings.json, so a
    // session that never opens the MIDI screen must still have its knobs reaching their mappings.
    // The same reason the port itself is opened here rather than waiting for a pick.
    host_.set_midi_control_channel(s_.settings.midiControlChannel);
    refresh_midi_in_devices();
    if (s_.midiInDeviceIndex != 0) apply_midi_in_device();
    // ⚠️ UNCONDITIONALLY, and after the OUT port's own boot (app.cpp calls them in that order): with no
    // input device the verdict is still a verdict — thru ON, nothing to loop — and a boot that skipped
    // it would leave the flag at whatever the default happens to be rather than at something decided.
    update_midi_thru();
    // ⚠️ …and drop what `apply` just wrote, exactly as `boot_midi_port` does: boot news belongs on the
    // CONSOLE, and a status line left over from launch would greet the user on the MIDI screen minutes
    // later as though something had just happened.
    s_.midiStatusText.clear();
}

void InputDispatcher::refresh_midi_in_devices() {
    s_.midiInDeviceNames.assign(1, "OFF");   // index 0, always — "no device" is a choice, not a gap

    if (s_.midiIn) {
        const int n = s_.midiIn->device_count();
        for (int i = 0; i < n; ++i) s_.midiInDeviceNames.push_back(s_.midiIn->device_name(i));
    }

    s_.midiInDeviceIndex = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);
}

void InputDispatcher::apply_midi_in_device() {
    const int wanted = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);
    s_.midiInDeviceIndex = wanted;

    if (!s_.midiIn) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    // ⚠️ The teardown order, and every step of it answers a way this can go wrong — see the header.
    s_.midiIn->set_sink(nullptr);
    s_.midiIn->close();
    host_.reset_midi_in();

    if (wanted == 0) {
        s_.midiStatusText = "INPUT OFF";
    } else {
        // The sink BEFORE the open: an open port is already delivering, and bytes that arrive between
        // the two would be dropped by a backend that has nowhere to put them — a silent loss at exactly
        // the moment a user is watching to see whether their keyboard works.
        s_.midiIn->set_sink(&host_.midi_in_sink());
        if (s_.midiIn->open(wanted - 1)) {   // −1: index 0 of the list is OFF, not a device
            s_.midiStatusText = "INPUT OPENED";
        } else {
            // The choice is KEPT, exactly as on the OUTPUT side: a port that refuses is usually one
            // another app holds, which is a transient the user can fix. The sink is unwired again so a
            // backend that half-opened cannot deliver into a port the app believes is closed.
            s_.midiIn->set_sink(nullptr);
            s_.midiStatusText = "INPUT BUSY";
        }
    }

    update_midi_thru();   // …and the IN half of the same one verdict (E4) — see apply_midi_device
}

void InputDispatcher::update_midi_thru() {
    // ⚠️ **BOTH PORTS OPEN, OR THERE IS NOTHING TO LOOP.** `is_open()` and not the saved choice: a
    // device that was picked and then refused to open (PORT BUSY) is a port sending nothing, and
    // suppressing thru for it would silence a feature over a cable that does not exist.
    const bool inOpen  = s_.midiIn != nullptr && s_.midiIn->is_open() && s_.midiInDeviceIndex > 0;
    const bool outOpen = port_open() && s_.midiDeviceIndex > 0;

    // The two names the SCREEN is showing. On every backend this project has met, a loopback's two
    // directions carry the SAME display name (winmm's "loopMIDI Port" both ways) — a hardware keyboard
    // and a hardware synth never do, because they are two different devices.
    // ⚠️ Index 0 is "OFF" on both lists, which is why the `> 0` above is part of the predicate and not
    // an optimisation: without it, two closed ports would compare equal and turn thru off.
    const bool loopback =
        inOpen && outOpen &&
        static_cast<size_t>(s_.midiInDeviceIndex) < s_.midiInDeviceNames.size() &&
        static_cast<size_t>(s_.midiDeviceIndex)   < s_.midiDeviceNames.size() &&
        s_.midiInDeviceNames[static_cast<size_t>(s_.midiInDeviceIndex)] ==
            s_.midiDeviceNames[static_cast<size_t>(s_.midiDeviceIndex)];

    host_.set_midi_in_thru(!loopback);

    // ⚠️ SAID OUT LOUD, on the screen the user just used to cause it. A suppression nobody is told
    // about is indistinguishable from an EXTERNAL instrument that does not respond to a keyboard —
    // and it overwrites "INPUT OPENED" deliberately, because it is the more surprising news of the two.
    if (loopback) s_.midiStatusText = "THRU OFF: LOOP";
}

void InputDispatcher::midi_action() {
    switch (static_cast<MidiRow>(s_.midiCursorRow)) {
        case MidiRow::PANIC:
            // Every note-off we owe, on every channel we have used, right now. It goes through the
            // CONSUMER and not the port, because the consumer is what knows which notes are sounding —
            // and it is the same call `SongcoreHost::stop()` makes, so there is one panic in the app.
            host_.midi_out().panic();
            s_.midiStatusText = port_open() ? "PANIC SENT" : "NO PORT";
            break;

        case MidiRow::TEST: {
            // ⚠️ THE ONE THING ON THIS SCREEN THAT WRITES TO THE PORT DIRECTLY, bypassing the bus — and
            // that is the point of it rather than a shortcut taken. TEST answers "is there a cable, and
            // does this machine's MIDI stack work at all?", and an answer routed through the sequencer,
            // the router and the instrument's EXTERNAL flag would be answering a much larger question:
            // a silent TEST would no longer mean "no cable", it would mean "something, somewhere".
            //
            // A note-on and its note-off in the same breath. Sustaining it would make the screen the one
            // place in the app that can leave a note hanging with no transport to stop it.
            if (!port_open()) { s_.midiStatusText = "NO PORT"; break; }
            const uint8_t on[3]  = {0x90, 60, 100};   // C-4, channel 1, mf
            const uint8_t off[3] = {0x80, 60, 0};
            s_.midiOut->send(on, 3);
            s_.midiOut->send(off, 3);
            s_.midiStatusText = "TEST SENT";
            break;
        }

        // The door into the mapping list. It carries no port and no cable, so unlike PANIC and TEST
        // it needs nothing refreshed on the way in — only the cursor put back inside a list whose
        // length belongs to whichever song is loaded now.
        case MidiRow::MAPPING: {
            clamp_midi_map_cursor();
            s_.midiMapReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::MIDI_MAP;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        // OUTPUT / OFFSET / PROG CHG are A+DPAD cells — the app-wide rule that single A is for actions.
        default:
            break;
    }
}

void InputDispatcher::clamp_midi_map_cursor() {
    const songcore::Project& p    = host_.project();
    const int                rows = midi_map_row_count(p);   // always ≥ 1 — the ADD row
    s_.midiMapCursorRow    = std::clamp(s_.midiMapCursorRow, 0, rows - 1);
    s_.midiMapCursorColumn = midi_map_clamp_column(p, s_.midiMapCursorRow, s_.midiMapCursorColumn);
}

void InputDispatcher::midi_map_action() {
    // The ADD row is the only one a bare A means anything on; every cell above it is an A+DPAD cell,
    // which is the app-wide rule that a single A is for actions.
    songcore::Project& p = host_.edit_project();
    if (s_.midiMapCursorRow != static_cast<int>(p.midiMappings.size())) return;
    if (static_cast<int>(p.midiMappings.size()) >= songcore::MIDI_MAP_MAX) return;

    // ⚠️ **A NEW ROW POINTS AT SOMETHING REAL FROM THE FIRST FRAME.** A mapping with no destination
    // would be a row whose parameter cell has nothing to cycle and whose range has no units — so it
    // starts on the catalogue's first entry, across that destination's whole range, and the user
    // dials it from there. Track 1's fader is also the one destination every project has.
    songcore::MidiMapping m;
    m.controller = 0;
    m.dest       = static_cast<uint8_t>(songcore::MAP_DESTS[0].id);
    m.scopeIndex = 0;
    m.rangeMin   = songcore::MAP_DESTS[0].min;
    m.rangeMax   = songcore::MAP_DESTS[0].max;
    p.midiMappings.push_back(m);

    // The cursor stays on the row it pressed A on, which is now the new mapping rather than the ADD
    // row — the row the user is about to edit, with the ADD row still one step below it.
    mark_modified();
}

}  // namespace pt::ui
