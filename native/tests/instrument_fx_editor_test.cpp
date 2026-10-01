// Run: c++ -std=c++17 -Inative native/tests/instrument_fx_editor_test.cpp \
//      native/ui/modules/instrument_fx_editor.cpp native/ui/modules/instrument_editor.cpp \
//      native/ui/canvas.cpp -o /tmp/pt-fx-test && /tmp/pt-fx-test
#include <cassert>

#include "ui/modules/instrument_fx_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/instrument_row_layout.h"

int main() {
    using namespace pt;
    ui::InstrumentFxEditorModule fx;
    ui::InstrumentEditorModule main;
    songcore::Instrument untouched;
    for (auto type : {songcore::InstrumentType::SAMPLER, songcore::InstrumentType::SOUNDFONT,
                      songcore::InstrumentType::SYNTH}) {
        songcore::Instrument ins;
        ins.instrumentType = type;
        const int originalVolume = ins.volume;
        assert(fx.handle_input(ins, 0, ui::InputAction::set_value(80)));
        assert(fx.handle_input(ins, 1, ui::InputAction::set_value(255)));
        assert(fx.handle_input(ins, 2, ui::InputAction::set_value(10)));
        assert(ins.drive == 80 && ins.crush == 15 && ins.downsample == 10);
        assert(fx.handle_input(ins, 3, ui::InputAction::set_value(1)));
        assert(ins.filterType == "lp");
        assert(!fx.handle_input(ins, 3, ui::InputAction::set_value(4)));
        assert(fx.handle_input(ins, 4, ui::InputAction::set_value(140)));
        assert(fx.handle_input(ins, 5, ui::InputAction::set_value(90)));
        assert(ins.filterCut == 140 && ins.filterRes == 90);
        for (int row = 6; row < 9; ++row)
            assert(fx.handle_input(ins, row, ui::InputAction::set_value(123)));
        assert(ins.reverbSend == 123 && ins.delaySend == 123 && ins.chorusSend == 123);
        assert(ins.volume == originalVolume && untouched.drive == 0);
        assert(fx.handle_input(ins, 0, ui::on_a_b(fx.cursor_context(ins, 0))));
        assert(ins.drive == 0);
        assert(!fx.handle_input(ins, -1, ui::InputAction::set_value(2)));
        assert(!fx.handle_input(ins, 9, ui::InputAction::set_value(2)));
        ui::InstrumentEditorState state{ins};
        state.cursorRow = ui::instrument_fx_row(type);
        assert(main.cursor_context(state).valueType == ui::CursorValueType::READ_ONLY);
        state.cursorRow = ui::instrument_eq_row(type);
        assert(main.handle_input(ins, state.cursorRow, 1, ui::InputAction::set_value(7)).modified);
        assert(ins.eqSlot == 7);
    }
    songcore::Instrument sample;
    assert(main.handle_input(sample, 9, 1, ui::InputAction::set_value(1)).modified);
    assert(sample.loopMode == "fwd");
    assert(main.handle_input(sample, 11, 3, ui::InputAction::set_value(1)).modified);
    assert(sample.reverse);
    ui::InstrumentEditorState state{sample};
    state.cursorRow = 9;
    state.allowOscLoop = false;
    assert(main.cursor_context(state).maxValue == 2);
    assert(fx.map_target(0).id == songcore::MapDestId::INS_DRIVE);
    sample.instrumentType = songcore::InstrumentType::EXTERNAL;
    assert(ui::instrument_fx_row(sample.instrumentType) == -1);
    assert(!fx.handle_input(sample, 0, ui::InputAction::set_value(30)));
    state.cursorRow = 2;
    assert(main.cursor_context(state).maxValue == 15);
    state.cursorRow = 3;
    assert(main.cursor_context(state).maxValue == 127);
}
