// Run: c++ -std=c++17 -Inative native/tests/envelope_editor_test.cpp \
//      native/ui/modules/envelope_editor.cpp native/ui/canvas.cpp -o /tmp/pt-env-test && /tmp/pt-env-test
#include <cassert>

#include "ui/modules/envelope_editor.h"

int main() {
    using namespace pt;
    ui::EnvelopeEditorModule editor;
    songcore::Instrument ins;
    const auto otherSlot = ins.modSlots[1];

    assert(editor.handle_input(ins, 0, 0, ui::InputAction::set_value(0x20)));
    assert(ins.modSlots[0].type == songcore::ModType::ADSR);
    assert(ins.modSlots[0].dest == songcore::ModDest::VOLUME);
    assert(ins.modSlots[0].attack == 0x20);
    assert(ins.modSlots[1].type == otherSlot.type && ins.modSlots[1].dest == otherSlot.dest);

    assert(editor.handle_input(ins, 0, 3, ui::InputAction::set_value(0x70)));
    assert(editor.handle_input(ins, 0, 3, ui::on_a_b(editor.cursor_context(ins, 0, 3))));
    assert(ins.modSlots[0].release == 6);

    assert(editor.handle_input(ins, 1, 1, ui::InputAction::set_value(4)));
    assert(ins.modSlots[1].type == songcore::ModType::ADSR);
    assert(ins.modSlots[1].dest == songcore::ModDest::FILTER_CUTOFF);
    assert(ins.modSlots[1].decay == 4 && ins.modSlots[1].sustain == 0);
    assert(ins.modSlots[0].attack == 0x20);
}
