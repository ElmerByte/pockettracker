// Link against the host libpockettracker.a and its codec libraries.
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>
#include "audio-engine.h"
#include "synth-oscillator.h"
#include "songcore/engine_setup.h"
#include "songcore/project_io.h"
#include "ui/modules/instrument_editor.h"
#include "ui/instrument_row_layout.h"

static std::vector<float> render(int mix, int wave2, int detune, bool synthEnabled = true, int wave1 = 0, float pitch = 220) {
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(48000);
    songcore::Instrument ins;
    ins.instrumentType = songcore::InstrumentType::SYNTH;
    ins.id = ins.sampleId = 0;
    ins.synthWave = wave1;
    ins.synthMix = mix;
    ins.synthWave2 = wave2;
    ins.synthDetune2 = detune;
    assert(songcore::load_synth_wave(*engine, ins));
    songcore::Routing routing;
    songcore::push_instrument_params(*engine, ins, routing, 128, 48000);
    if (!synthEnabled) {
        auto program = songcore::make_program(ins, 1, -1);
        program.type = songcore::PROGRAM_SAMPLER;
        engine->setProgram(0, program, nullptr, 0);
    }
    engine->scheduleNote(0, 0, 0, pitch, 48000.0f / 1024, 0.5f);
    std::vector<float> audio(48000 * 2);
    for (int frame = 0; frame < 48000; frame += 128) {
        const int n = std::min(128, 48000 - frame);
        engine->processLiveBlock(audio.data() + frame * 2, n, 2, 48000);
    }
    return audio;
}

static float frequency(const std::vector<float>& audio) {
    int crossings = 0;
    for (int i = 4800; i < 48000 - 1; ++i) {
        assert(std::isfinite(audio[i * 2]));
        if (audio[i * 2] <= 0 && audio[(i + 1) * 2] > 0) ++crossings;
    }
    return crossings / 0.9f;
}

int main() {
    // Verify the actual mixer keeps the original audio when OSC2 is off.
    const auto original = render(0, 0, 128, false);
    assert(original == render(0, 3, 255));
    const auto centered = render(255, 0, 128);
    const float centerHz = frequency(centered);
    assert(std::abs(centerHz - 220) < 3);
    assert(frequency(render(255, 0, 0)) < centerHz - 10);
    assert(frequency(render(255, 0, 255)) > centerHz + 10);
    assert(centered == render(255, 0, 128)); // New notes start from a repeatable phase.
    assert(render(128, 2, 140) != original);
    assert(render(255, 3, 128) != centered);

    // Fresh noise has no short wavetable repetition and no dependence on note pitch or detune.
    const auto noise = render(255, 4, 128);
    assert(noise == render(255, 4, 0));
    assert(noise == render(255, 4, 255));
    assert(render(0, 0, 128, true, 4, 220) == render(0, 0, 128, true, 4, 880));
    assert(render(24, 4, 128) != original); // Quiet OSC2 noise for pads.
    for (const float value : noise) assert(std::isfinite(value) && std::abs(value) <= 1.0f);
    synth::Noise generator{123};
    double sum = 0, energy = 0, repeatedCycle = 0;
    std::vector<float> raw(65536);
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = generator.sample(0);
        sum += raw[i]; energy += raw[i] * raw[i];
        if (i >= synth::CYCLE) repeatedCycle += raw[i] * raw[i - synth::CYCLE];
    }
    assert(std::abs(sum / raw.size()) < 0.02);
    assert(energy / raw.size() > 0.3 && energy / raw.size() < 0.36);
    assert(std::abs(repeatedCycle / raw.size()) < 0.02);
    generator.trigger(0);
    const float held = generator.sample(3);
    for (int i = 1; i < 8; ++i) assert(generator.sample(3) == held);
    assert(generator.sample(3) != held);

    auto project = songcore::make_default_project();
    auto& ins = project.instruments[0];
    ins.instrumentType = songcore::InstrumentType::SYNTH;
    ins.synthWave = ins.synthWave2 = 4; ins.synthMix = 128; ins.synthDetune2 = 141;
    const auto saved = songcore::json::parse(songcore::serialize_project(project));
    const auto loaded = songcore::parse_project(saved);
    assert(loaded.instruments[0].synthWave == 4 && loaded.instruments[0].synthWave2 == 4);
    assert(loaded.instruments[0].synthMix == 128);
    assert(loaded.instruments[0].synthDetune2 == 141);
    const auto legacy = songcore::parse_project(songcore::json::parse(
        songcore::serialize_project(songcore::make_default_project())));
    assert(legacy.instruments[0].synthMix == 0);
    assert(legacy.instruments[0].synthDetune2 == 128);

    pt::ui::InstrumentEditorModule editor;
    assert(editor.handle_input(ins, 7, 1, pt::ui::InputAction::set_value(4)).modified);
    assert(editor.handle_input(ins, 8, 1, pt::ui::InputAction::set_value(4)).modified);
    assert(ins.synthWave == 4 && ins.synthWave2 == 4);
    assert(editor.handle_input(ins, 8, 1, pt::ui::InputAction::set_value(2)).modified);
    assert(editor.handle_input(ins, 9, 1, pt::ui::InputAction::set_value(200)).modified);
    assert(editor.handle_input(ins, 8, 3, pt::ui::InputAction::set_value(150)).modified);
    pt::ui::InstrumentEditorState state{ins};
    state.cursorRow = 8; state.cursorColumn = 3;
    assert(editor.handle_input(ins, 8, 3, pt::ui::on_a_b(editor.cursor_context(state))).modified);
    assert(ins.synthDetune2 == 128 && ins.synthMix == 200 && ins.synthWave2 == 2);
    assert(pt::ui::instrument_fx_row(ins.instrumentType) == 11);
    assert(pt::ui::instrument_eq_row(ins.instrumentType) == 12);
}
