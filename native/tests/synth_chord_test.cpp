// Link against host libpt-ui.a, libpockettracker.a and their codec libraries.
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>
#include "audio-engine.h"
#include "songcore/engine_setup.h"
#include "songcore/project_io.h"
#include "songcore/synth_chord.h"
#include "songcore/scheduler.h"
#include "songcore/engine_consumer.h"
#include "ui/fx_helper.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/chord_banks.h"
#include "ui/instrument_row_layout.h"

static std::vector<float> render(songcore::Instrument ins, float pitch = 220, int chordFx = -1, bool table = false, bool restart = false, int offAt = -1) {
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(48000);
    ins.id = ins.sampleId = 0;
    assert(songcore::load_synth_wave(*engine, ins));
    songcore::Routing routing;
    songcore::push_instrument_params(*engine, ins, routing, 128, 48000);
    if (table) {
        uint8_t rows[16 * 8]{};
        for (int row = 0; row < 16; ++row) rows[row * 8 + 1] = 255;
        rows[2] = songcore::FX_CHD; rows[3] = static_cast<uint8_t>(chordFx);
        engine->loadTable(0, rows);
        // Hold this shape for the note; later blank rows do not erase it.
    }
    engine->scheduleNote(0, 0, 0, pitch, 48000.0f / 1024, 0.5f, 1.0f, 0.5f,
                         -1, -1, table ? 0 : -1);
    if (chordFx >= 0 && !table)
        engine->scheduleVoiceCc(1, 0, songcore::CC_SYNTH_CHORD, chordFx / 255.0f);
    if (restart) {
        engine->scheduleVoiceCc(24000, 0, songcore::CC_SYNTH_CHORD, 1 / 255.0f);
        engine->scheduleNote(48000, 0, 0, pitch, 48000.0f / 1024, 0.5f);
    }
    if (offAt >= 0) engine->scheduleVoiceCc(offAt, 0, songcore::CC_SYNTH_CHORD, 1.0f);
    engine->scheduleNoteOff(restart ? 72000 : 48000, 0);
    std::vector<float> audio(96000 * 2);
    for (int frame = 0; frame < 96000; frame += 128)
        engine->processLiveBlock(audio.data() + frame * 2, 128, 2, 48000);
    float peak = 0;
    for (const float sample : audio) {
        assert(std::isfinite(sample) && std::abs(sample) < 1.1f);
        peak = std::max(peak, std::abs(sample));
    }
    assert(peak > 0.001f);
    for (size_t i = 180000; i < audio.size(); ++i) assert(std::abs(audio[i]) < 0.001f);
    return audio;
}

static double amplitude(const std::vector<float>& audio, double hz, int channel = 0, int first = 4800, int last = 38400) {
    double real = 0, imaginary = 0;
    for (int i = first; i < last; ++i) {
        const double phase = 6.283185307179586 * hz * i / 48000;
        real += audio[i * 2 + channel] * std::cos(phase);
        imaginary += audio[i * 2 + channel] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (last - first);
}

static uint32_t float_bits(float f) { uint32_t value; std::memcpy(&value, &f, sizeof(value)); return value; }

int main() {
    using namespace songcore;
    Instrument ins;
    ins.instrumentType = InstrumentType::SYNTH;
    auto& amp = ins.modSlots[0];
    amp.type = ModType::ADSR; amp.dest = ModDest::VOLUME;
    amp.sustain = 255; amp.release = 8;
    const auto original = render(ins);
    // Off ignores every bank setting and keeps the previous synth path bit-for-bit.
    auto disabled = ins;
    disabled.synthChordDetune = 255; disabled.synthChordWidth = 255;
    disabled.synthChordBanks[0] = {-24,24};
    assert(render(disabled) == original);

    ins.synthChordMode = 2;
    ins.synthChordDetune = 0; ins.synthChordWidth = 0;
    const auto major = render(ins);
    for (int offset : {0, 4, 7})
        assert(amplitude(major, 220 * std::pow(2.0, offset / 12.0)) > 0.04);
    assert(amplitude(original, 220 * std::pow(2.0, 4 / 12.0)) < 0.01);
    for (size_t i = 0; i < major.size(); i += 2) assert(std::abs(major[i] - major[i+1]) < 0.000001f);
    const auto transposed = render(ins, 440);
    for (int offset : {0, 4, 7})
        assert(amplitude(transposed, 440 * std::pow(2.0, offset / 12.0)) > 0.04);

    ins.synthChordWidth = 255;
    const auto stereo = render(ins);
    assert(amplitude(stereo, 220, 0) > amplitude(stereo, 220, 1) * 5);
    const double fifth = 220 * std::pow(2.0, 7 / 12.0);
    assert(amplitude(stereo, fifth, 1) > amplitude(stereo, fifth, 0) * 5);

    ins.synthChordMode = 1;
    ins.synthChordDetune = 100;
    const auto unison = make_program(ins, 1, -1);
    assert(unison.synthChordRatio[0] < 1 && unison.synthChordRatio[1] == 1 && unison.synthChordRatio[2] > 1);
    assert(std::abs(unison.synthChordRatio[0] * unison.synthChordRatio[2] - 1) < 0.000001f);
    render(ins);
    for (int mode = 0; mode < 16; ++mode) {
        auto patch = ins;
        patch.synthChordMode = 2; patch.synthChordBank = mode;
        patch.synthChordBanks[8] = {-12,7};
        patch.synthWave = 3; patch.synthWave2 = 4; patch.synthMix = 24;
        patch.synthPulseWidth1 = 40; patch.synthSync = 100;
        patch.filterType = "lp"; patch.filterCut = 120;
        patch.modSlots[2].type = ModType::LFO;
        patch.modSlots[2].dest = ModDest::SYNTH_PW1;
        patch.modSlots[2].amount = 80;
        render(patch); // All bank modes work with sync, PWM, noise, filter and release.
        InstrumentPreset preset; preset.instrument = patch;
        const auto text = serialize_instrument_preset(preset);
        assert(serialize_instrument_preset(parse_instrument_preset(json::parse(text))) == text);
    }
    // CHD reaches the actual audio path from either a phrase controller or a table row.
    auto fxIns = ins;
    fxIns.synthChordMode = 0; fxIns.synthChordDetune = 0; fxIns.synthChordWidth = 0;
    for (bool table : {false, true}) {
        const auto audio = render(fxIns, 220, 1, table);
        assert(amplitude(audio, 220 * std::pow(2.0, 3 / 12.0)) > 0.04);
        assert(amplitude(audio, 220 * std::pow(2.0, 4 / 12.0)) < 0.01);
    }
    const auto changing = render(fxIns, 220, 0, false, true);
    const double majThird = 220 * std::pow(2.0, 4 / 12.0);
    const double minThird = 220 * std::pow(2.0, 3 / 12.0);
    assert(amplitude(changing, majThird, 0, 4800, 22000) > 0.04);
    assert(amplitude(changing, minThird, 0, 26000, 45000) > 0.04);
    assert(amplitude(changing, minThird, 0, 52000, 68000) < 0.01);
    assert(amplitude(changing, majThird, 0, 52000, 68000) < 0.01);
    assert(amplitude(changing, 220, 0, 52000, 68000) > 0.1);
    fxIns.synthChordMode = 2;
    const auto offFx = render(fxIns, 220, 255);
    assert(amplitude(offFx, 220) > 0.1);
    assert(amplitude(offFx, 220 * std::pow(2.0, 4 / 12.0)) < 0.01);

    // A fully audible chord must keep the hidden base noise/phase clock running.
    // Turning it off resumes the exact noise sequence and OSC2 pitch of a plain note.
    for (int wave : {0, 3, 4}) {
        auto patch = fxIns;
        patch.synthChordMode = 0;
        patch.synthWave = patch.synthWave2 = wave;
        patch.synthMix = 128; patch.synthDetune2 = 145;
        const auto plain = render(patch);
        const auto toggled = render(patch, 220, 0, false, false, 24000);
        for (int frame = 26000; frame < 45000; ++frame)
            for (int channel = 0; channel < 2; ++channel)
                assert(std::abs(plain[frame*2+channel] - toggled[frame*2+channel]) < 0.000001f);
    }

    struct Recorder : IMidiConsumer {
        std::vector<Event> events;
        void consume(const Event& event) override { events.push_back(event); }
        void on_play(const std::string&, const std::string&, int64_t, int, int) override {}
        void on_stop() override {}
    } recorder;
    auto fxProject = make_default_project();
    fxProject.instruments[0] = fxIns;
    auto& steps = fxProject.phrases[0].steps;
    steps[0].note = Note{9, 3}; steps[0].instrument = 0;
    steps[0].fx1Type = songcore::FX_CHD; steps[0].fx1Value = 2;
    steps[1].fx1Type = songcore::FX_CHD; steps[1].fx1Value = 3;
    MidiRouter router; router.add_consumer(&recorder);
    Sequencer seq(router, fxProject, 48000); seq.playPhrase(0);
    seq.updatePlaybackBuffer();
    int64_t firstNote = -1;
    bool sameStep = false, heldStep = false;
    for (const auto& ev : recorder.events) {
        if (ev.type == EV_NOTE_ON && firstNote < 0) firstNote = ev.frame;
        if (ev.type == EV_CC && ev.cc.param == CC_SYNTH_CHORD) {
            if (ev.cc.valueBits == float_bits(2 / 255.0f)) sameStep |= ev.frame == firstNote + 1;
            if (ev.cc.valueBits == float_bits(3 / 255.0f)) heldStep |= ev.frame > firstNote + 1;
        }
    }
    assert(sameStep && heldStep);
    assert(effect_name(songcore::FX_CHD) == "CHD" && effect_value_max(songcore::FX_CHD) == 255);
    assert(pt::ui::effect_descriptions()[effect_type_index(songcore::FX_CHD)][0] == "CHD: Synth chord bank");
    assert(resolve_cc_param(fxIns, CC_SYNTH_CHORD) == -1); // Never sends a masked MIDI CC.
    assert(synth_chord_intervals(ins) == (std::array<int,3>{0,0,0}));
    auto project = make_default_project();
    const auto loaded = parse_project(json::parse(serialize_project(project)));
    assert(loaded.instruments[0].synthChordMode == 0);

    pt::ui::InstrumentEditorModule editor;
    pt::ui::ChordBanksModule banks;
    ins.synthChordMode = 2; ins.synthChordBank = 1;
    assert(banks.handle_input(ins, 1, 1, pt::ui::InputAction::set_value(10)));
    assert(ins.synthChordBanks[1] == (std::array<int,2>{3,10}));
    assert(banks.handle_input(ins, 8, 0, pt::ui::InputAction::set_value(-12)));
    assert(ins.synthChordBanks[8][0] == -12);
    assert(banks.handle_input(ins, 8, 0, pt::ui::InputAction::set_value(-100)));
    assert(ins.synthChordBanks[8][0] == -24);
    assert(banks.handle_input(ins, 8, 0, pt::ui::on_a_b(banks.cursor_context(ins,8,0))));
    assert(ins.synthChordBanks[8][0] == 0);
    assert(pt::ui::decrement(banks.cursor_context(ins,8,0)).value == -1);
    assert(pt::ui::decrement_fast(banks.cursor_context(ins,8,0)).value == -12);
    assert(!banks.handle_input(ins, 16, 0, pt::ui::InputAction::set_value(1)));
    assert(banks.cursor_context(ins, 1, 0).minValue == -24);
    assert(editor.handle_input(ins, 12, 1, pt::ui::InputAction::set_value(15)).modified);
    assert(ins.synthChordBank == 15);

    // Legacy fixed shapes, custom intervals and both FX pools migrate once.
    json legacy = {{"instruments", json::array({{
        {"instrumentType", "SYNTH"}, {"synthChordMode", 6},
        {"synthChordInterval2", -12}, {"synthChordInterval3", 10}}})},
        {"phrases", json::array({{{"steps", json::array({{{"fx1Type", songcore::FX_CHD}, {"fx1Value", 3}}})}}})},
        {"tables", json::array({{{"rows", json::array({{{"fx2Type", songcore::FX_CHD}, {"fx2Value", 0}}})}}})}};
    constexpr int oldBank[] = {0,0,0,1,3,4,8};
    constexpr int oldFx[] = {255,254,0,1,3,4,8};
    for (int mode=0; mode<=6; ++mode) {
        auto old = legacy;
        old["instruments"][0]["synthChordMode"] = mode;
        old["phrases"][0]["steps"][0]["fx1Value"] = mode;
        const auto restored = parse_project(old);
        assert(restored.instruments[0].synthChordMode == std::min(mode,2));
        assert(restored.instruments[0].synthChordBank == oldBank[mode]);
        assert(restored.phrases[0].steps[0].fx1Value == oldFx[mode]);
    }
    const auto migrated = parse_project(legacy);
    assert(migrated.instruments[0].synthChordMode == 2 && migrated.instruments[0].synthChordBank == 8);
    assert(migrated.instruments[0].synthChordBanks[8] == (std::array<int,2>{-12,10}));
    assert(migrated.phrases[0].steps[0].fx1Value == 1);
    assert(migrated.tables[0].rows[0].fx2Value == 255);
    auto saved = serialize_project(migrated);
    assert(serialize_project(parse_project(json::parse(saved))) == saved);
    auto legacyPreset = parse_instrument_preset(json{{"instrument", legacy["instruments"][0]},
        {"tableRows", json::array({{{"fx1Type", songcore::FX_CHD}, {"fx1Value", 6}}})}});
    assert(legacyPreset.tableRows->at(0).fx1Value == 8);
    render(fxIns, 220, 254); // explicit unison
    render(fxIns, 220, 16); // unsupported bank values are ignored
    pt::ui::InstrumentEditorState state{ins}; state.cursorRow = 11; state.cursorColumn = 1;
    assert(editor.handle_input(ins, 11, 1, pt::ui::on_a_b(editor.cursor_context(state))).modified);
    assert(ins.synthChordMode == 0);
    assert(pt::ui::instrument_fx_row(ins.instrumentType) == 14);
    assert(pt::ui::instrument_eq_row(ins.instrumentType) == 15);
}
