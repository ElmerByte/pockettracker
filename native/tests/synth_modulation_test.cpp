// Link against host libpt-ui.a, libpockettracker.a, and their codec libraries.
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>
#include "audio-engine.h"
#include "songcore/engine_setup.h"
#include "songcore/project_io.h"
#include "synth-oscillator.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/modulation.h"

static std::vector<float> render(songcore::Instrument ins) {
    auto engine = std::make_unique<AudioEngine>();
    engine->setDeviceSampleRate(48000);
    ins.id = ins.sampleId = 0;
    assert(songcore::load_synth_wave(*engine, ins));
    songcore::Routing routing;
    songcore::push_instrument_params(*engine, ins, routing, 128, 48000);
    engine->scheduleNote(0, 0, 0, 220, 48000.0f / 1024, 0.5f);
    engine->scheduleNoteOff(48000, 0);
    std::vector<float> audio(120000 * 2);
    for (int frame = 0; frame < 120000; frame += 128) {
        const int n = std::min(128, 120000 - frame);
        engine->processLiveBlock(audio.data() + frame * 2, n, 2, 48000);
    }
    float peak = 0;
    for (const float sample : audio) {
        assert(std::isfinite(sample) && std::abs(sample) < 1.1f);
        peak = std::max(peak, std::abs(sample));
    }
    assert(peak > 0.001f);
    for (size_t i = 220000; i < audio.size(); ++i) assert(std::abs(audio[i]) < 0.001f);
    return audio;
}

int main() {
    using namespace songcore;
    // Default pulse width must match the old square table exactly, including sample-rate reduction.
    for (int ds : {0, 2, 7}) for (int i = 0; i < 4096; ++i) {
        const double phase = i / 4096.0;
        assert(synth::pulse(phase, 128, ds) == synth::sample(3, phase, ds));
    }
    for (int width : {0, 64, 128, 192, 255}) {
        int positive = 0;
        for (int i = 0; i < 1024; ++i)
            if (synth::pulse(i / 1024.0, width) > 0) ++positive;
        const float expected = std::clamp(width / 256.0f, 0.05f, 0.95f);
        assert(std::abs(positive / 1024.0f - expected) < 0.002f);
    }

    Instrument ins;
    ins.instrumentType = InstrumentType::SYNTH;
    ins.synthWave = ins.synthWave2 = 3;
    ins.synthMix = 128;
    auto& amp = ins.modSlots[0];
    amp.type = ModType::ADSR; amp.dest = ModDest::VOLUME;
    amp.sustain = 255; amp.release = 16;
    const auto original = render(ins);
    auto narrow = ins;
    narrow.synthPulseWidth1 = narrow.synthPulseWidth2 = 64;
    assert(render(narrow) != original);
    narrow.synthWave = narrow.synthWave2 = 0;
    auto sine = narrow;
    sine.synthPulseWidth1 = sine.synthPulseWidth2 = 128;
    assert(render(narrow) == render(sine)); // Width does not colour non-pulse waves.

    for (auto dest : {ModDest::SYNTH_PW1, ModDest::SYNTH_PW2,
                      ModDest::SYNTH_MIX, ModDest::SYNTH_DETUNE2}) {
        auto moving = ins;
        // Different waves make oscillator blend modulation observable.
        if (dest == ModDest::SYNTH_MIX) moving.synthWave2 = 0;
        const auto baseline = render(moving);
        auto& lfo = moving.modSlots[2];
        lfo.type = ModType::LFO; lfo.dest = dest;
        lfo.oscShape = 1; lfo.lfoFreq = 16; lfo.lfoTrigMode = 1;
        lfo.amount = 0;
        assert(render(moving) == baseline);
        lfo.amount = 255;
        assert(render(moving) != baseline);
        // Envelopes must use the same destination routes, including return to zero.
        lfo.type = ModType::ADSR; lfo.attack = 8; lfo.decay = 16;
        lfo.sustain = 64; lfo.release = 8;
        assert(render(moving) != baseline);
        InstrumentPreset preset; preset.instrument = moving;
        const auto text = serialize_instrument_preset(preset);
        assert(serialize_instrument_preset(parse_instrument_preset(json::parse(text))) == text);
    }
    auto pad = ins;
    pad.synthWave = 1; pad.synthWave2 = 4; pad.synthMix = 24;
    pad.filterType = "lp"; pad.filterCut = 100;
    pad.modSlots[0].attack = 24; pad.modSlots[0].release = 32;
    pad.modSlots[2].type = ModType::LFO;
    pad.modSlots[2].dest = ModDest::SYNTH_MIX;
    pad.modSlots[2].amount = 24; pad.modSlots[2].lfoFreq = 8;
    render(pad); // An airy pad must remain finite and finish its release.

    pt::ui::InstrumentEditorModule editor;
    assert(editor.handle_input(ins, 10, 1, pt::ui::InputAction::set_value(40)).modified);
    assert(editor.handle_input(ins, 10, 3, pt::ui::InputAction::set_value(200)).modified);
    pt::ui::InstrumentEditorState state{ins}; state.cursorRow = 10;
    assert(editor.handle_input(ins, 10, 1, pt::ui::on_a_b(editor.cursor_context(state))).modified);
    assert(ins.synthPulseWidth1 == 128 && ins.synthPulseWidth2 == 200);
    pt::ui::ModulationModule mods;
    ins.modSlots[0].type = ModType::LFO;
    pt::ui::ModulationState modState{ins}; modState.cursorRow = 1;
    assert(mods.cursor_context(modState).maxValue == MOD_DEST_COUNT - 1);
    ins.instrumentType = InstrumentType::SAMPLER;
    assert(mods.cursor_context(modState).maxValue == static_cast<int>(ModDest::SYNTH_PW1) - 1);
    const auto legacy = make_default_project();
    const auto loaded = parse_project(json::parse(serialize_project(legacy)));
    assert(loaded.instruments[0].synthPulseWidth1 == 128 && loaded.instruments[0].synthPulseWidth2 == 128);
}
