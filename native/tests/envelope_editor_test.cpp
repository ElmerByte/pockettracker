// Link against the host libpt-ui.a, libpockettracker.a and codec libraries.
#include <cassert>

#include "ui/modules/envelope_editor.h"
#include "audio-engine.h"
#include "ui/modules/modulation.h"
#include "songcore/project_io.h"
#include "songcore/engine_setup.h"
#include <memory>
#include <cmath>
#include <vector>

int main() {
    using namespace pt;
    ui::EnvelopeEditorModule editor;
    songcore::Instrument ins;
    ins.instrumentType = songcore::InstrumentType::SYNTH;
    ins.id = ins.sampleId = 0;
    const auto otherSlot = ins.modSlots[1];

    assert(editor.handle_input(ins, 0, 0, ui::InputAction::set_value(0x20)));
    assert((*ins.synthAmpEnvelope).type == songcore::ModType::ADSR);
    assert((*ins.synthAmpEnvelope).dest == songcore::ModDest::VOLUME);
    assert((*ins.synthAmpEnvelope).attack == 0x20);
    assert(ins.modSlots[1].type == otherSlot.type && ins.modSlots[1].dest == otherSlot.dest);

    assert(editor.handle_input(ins, 0, 3, ui::InputAction::set_value(0x70)));
    assert(editor.handle_input(ins, 0, 3, ui::on_a_b(editor.cursor_context(ins, 0, 3))));
    assert((*ins.synthAmpEnvelope).release == 6);

    assert(editor.handle_input(ins, 1, 1, ui::InputAction::set_value(4)));
    assert(ins.synthFilterEnvelope->type == songcore::ModType::ADSR);
    assert(ins.synthFilterEnvelope->dest == songcore::ModDest::FILTER_CUTOFF);
    assert(ins.synthFilterEnvelope->decay == 4 && ins.synthFilterEnvelope->sustain == 0);
    assert((*ins.synthAmpEnvelope).attack == 0x20);
    using namespace songcore;
    ui::ModulationModule mods;
    for (int index = 0; index < 4; ++index) {
        mods.handle_input(ins, index, 0, ui::InputAction::set_value(3)); // LFO
        mods.handle_input(ins, index, 1, ui::InputAction::set_value(static_cast<int>(ModDest::SYNTH_PW1)));
        mods.handle_input(ins, index, 2, ui::InputAction::set_value(40));
        assert(ins.synthAmpEnvelope->attack == 0x20 && ins.synthAmpEnvelope->release == 6);
        assert(ins.synthFilterEnvelope->decay == 4);
    }
    const auto pushes = derive_mod_pushes(ins, 128, 48000);
    for (int index = 0; index < 4; ++index) assert(pushes.slots[index].type == 3);
    assert(pushes.slots[4].type == 2 && pushes.slots[4].dest == 1);
    assert(pushes.slots[5].type == 2 && pushes.slots[5].dest == 5);

    InstrumentPreset preset; preset.instrument = ins;
    const auto text = serialize_instrument_preset(preset);
    assert(serialize_instrument_preset(parse_instrument_preset(json::parse(text))) == text);
    auto project = make_default_project(); project.instruments[0] = ins;
    const auto loaded = parse_project(json::parse(serialize_project(project)));
    assert(loaded.instruments[0].synthAmpEnvelope->attack == 0x20);
    assert(loaded.instruments[0].modSlots[0].type == ModType::LFO);

    // Legacy envelopes move once, preserving all values and leaving other mods in their slots.
    json old = {{"instrumentType", "SYNTH"}, {"modSlots", json::array({
        {{"type", "ADSR"}, {"dest", "VOLUME"}, {"attack", 37}, {"sustain", 221}, {"release", 19}},
        {{"type", "ADSR"}, {"dest", "FILTER_CUTOFF"}, {"decay", 42}, {"amount", 172}},
        {{"type", "LFO"}, {"dest", "SYNTH_MIX"}, {"lfoFreq", 73}}, json::object()})}};
    auto migrated = detail::parse_instrument(old, 0);
    assert(migrated.synthAmpEnvelope->attack == 37 && migrated.synthAmpEnvelope->release == 19);
    assert(migrated.synthFilterEnvelope->decay == 42 && migrated.synthFilterEnvelope->amount == 172);
    assert(migrated.modSlots[0].type == ModType::NONE && migrated.modSlots[1].type == ModType::NONE);
    assert(migrated.modSlots[2].lfoFreq == 73);
    old["instrumentType"] = "SAMPLER";
    const auto sampler = detail::parse_instrument(old, 0);
    assert(!sampler.synthAmpEnvelope && sampler.modSlots[0].type == ModType::ADSR);
    // Newly saved synths may freely put another volume ADSR in MOD1 without migrating it later.
    ins.modSlots[0].type = ModType::ADSR; ins.modSlots[0].dest = ModDest::VOLUME;
    preset.instrument = ins;
    auto newer = parse_instrument_preset(json::parse(serialize_instrument_preset(preset))).instrument;
    assert(newer.modSlots[0].type == ModType::ADSR && newer.synthAmpEnvelope->attack == 0x20);

    Routing routing;
    auto fresh = make_default_project();
    set_instrument_type<AudioEngine>(nullptr, fresh, 0, InstrumentType::SYNTH, routing);
    assert(fresh.instruments[0].synthAmpEnvelope->sustain == 255);
    for (const auto& mod : fresh.instruments[0].modSlots) assert(mod.type == ModType::NONE);

    // All four user slots active alongside AMP/FILT: finite audio, audible release, then silence.
    auto engine = std::make_unique<AudioEngine>(); engine->setDeviceSampleRate(48000);
    ins.synthWave = 3; ins.synthAmpEnvelope->attack = 4;
    ins.synthAmpEnvelope->release = 8;
    ins.filterType = "lp"; ins.filterCut = 100;
    for (auto& mod : ins.modSlots) { mod.type = ModType::LFO; mod.dest = ModDest::SYNTH_PW1; }
    assert(load_synth_wave(*engine, ins));
    push_instrument_params(*engine, ins, routing, 128, 48000);
    engine->scheduleNote(0, 0, 0, 220, 48000.0f / 1024, 0.5f);
    engine->scheduleNoteOff(48000, 0);
    std::vector<float> audio(96000 * 2);
    for (int frame = 0; frame < 96000; frame += 128)
        engine->processLiveBlock(audio.data() + frame * 2, 128, 2, 48000);
    float peak = 0, releasePeak = 0;
    for (size_t i = 0; i < audio.size(); ++i) {
        assert(std::isfinite(audio[i]) && std::abs(audio[i]) < 1.1f);
        peak = std::max(peak, std::abs(audio[i]));
        if (i >= 96000 && i < 98000) releasePeak = std::max(releasePeak, std::abs(audio[i]));
        if (i >= 180000) assert(std::abs(audio[i]) < 0.001f);
    }
    assert(peak > 0.01f && releasePeak > 0.001f);

}
