// Link against host libpt-ui.a, libpockettracker.a and their codec libraries.
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>
#include "audio-engine.h"
#include "songcore/engine_setup.h"
#include "songcore/project_io.h"
#include "songcore/synth_chord.h"
#include "ui/modules/instrument_editor.h"
#include "ui/instrument_row_layout.h"

static std::vector<float> render(songcore::Instrument ins, float pitch = 220) {
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(48000);
    ins.id = ins.sampleId = 0;
    assert(songcore::load_synth_wave(*engine, ins));
    songcore::Routing routing;
    songcore::push_instrument_params(*engine, ins, routing, 128, 48000);
    engine->scheduleNote(0, 0, 0, pitch, 48000.0f / 1024, 0.5f);
    engine->scheduleNoteOff(48000, 0);
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

static double amplitude(const std::vector<float>& audio, double hz, int channel = 0) {
    double real = 0, imaginary = 0;
    constexpr int first = 4800, last = 38400;
    for (int i = first; i < last; ++i) {
        const double phase = 6.283185307179586 * hz * i / 48000;
        real += audio[i * 2 + channel] * std::cos(phase);
        imaginary += audio[i * 2 + channel] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (last - first);
}

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
    disabled.synthChordInterval2 = -24; disabled.synthChordInterval3 = 24;
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
    for (int mode = 1; mode <= 6; ++mode) {
        auto patch = ins;
        patch.synthChordMode = mode;
        patch.synthChordInterval2 = -12; patch.synthChordInterval3 = 7;
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
    assert(synth_chord_intervals(ins) == (std::array<int,3>{0,0,0}));
    auto project = make_default_project();
    const auto loaded = parse_project(json::parse(serialize_project(project)));
    assert(loaded.instruments[0].synthChordMode == 0);

    pt::ui::InstrumentEditorModule editor;
    ins.synthChordMode = 3;
    assert(editor.handle_input(ins, 12, 3, pt::ui::InputAction::set_value(10)).modified);
    assert(ins.synthChordMode == 6 && ins.synthChordInterval2 == 3 && ins.synthChordInterval3 == 10);
    assert(editor.handle_input(ins, 12, 1, pt::ui::InputAction::set_value(-12)).modified);
    assert(ins.synthChordInterval2 == -12);
    pt::ui::InstrumentEditorState state{ins}; state.cursorRow = 11; state.cursorColumn = 1;
    assert(editor.handle_input(ins, 11, 1, pt::ui::on_a_b(editor.cursor_context(state))).modified);
    assert(ins.synthChordMode == 0);
    assert(pt::ui::instrument_fx_row(ins.instrumentType) == 14);
    assert(pt::ui::instrument_eq_row(ins.instrumentType) == 15);
}
