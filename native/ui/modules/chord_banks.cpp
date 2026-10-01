#include "ui/modules/chord_banks.h"
#include "ui/helpers.h"
#include "songcore/synth_chord.h"
#include <algorithm>
#include <cstdlib>
namespace pt::ui {
void ChordBanksModule::draw(Canvas& c, const songcore::Instrument& ins, int row, int col, const Theme& t) const {
    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);
    c.draw_text("CHORD BANKS", 10, 8, t.textTitle, CHAR_SPACING, FONT_SCALE);
    c.draw_text("INST " + hex2(ins.id) + "  ROOT + INT2 + INT3", 10, 34, t.textParam, CHAR_SPACING, FONT_SCALE);
    const char* headers[] = {"BANK", "INT2", "INT3", "SHAPE"};
    const int x[] = {10,150,300,450};
    for (int i=0; i<4; ++i) c.draw_text(headers[i], x[i], 64, t.textParam, CHAR_SPACING, FONT_SCALE);
    for (int bank=0; bank<16; ++bank) {
        const int y = 86 + bank * 21;
        c.draw_text(hex2(bank), 10, y, bank == ins.synthChordBank && ins.synthChordMode == 2 ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        for (int field=0; field<2; ++field) {
            int value = ins.synthChordBanks[bank][field];
            const std::string text = std::string(value < 0 ? "-" : "+") + (std::abs(value)<10 ? "0" : "") + std::to_string(std::abs(value));
            draw_cursor_cell(c, text, 150 + field * 150, y, row == bank && col == field, t.textValue, t);
        }
        c.draw_text(songcore::synth_chord_name(ins.synthChordBanks[bank]), 450, y, t.textParam, CHAR_SPACING, FONT_SCALE);
    }
    c.draw_text("A+DPAD EDIT  START SELECT/PLAY  B BACK", 10, 445, t.textEmpty, CHAR_SPACING, FONT_SCALE);
}
CursorContext ChordBanksModule::cursor_context(const songcore::Instrument& ins, int row, int col) const {
    if (row < 0 || row > 15 || col < 0 || col > 1) return cc::none();
    auto context = cc::transpose(ins.synthChordBanks[row][col], false, songcore::DEFAULT_CHORD_BANKS[row][col]);
    context.minValue=-24; context.maxValue=24;
    return context;
}
bool ChordBanksModule::handle_input(songcore::Instrument& ins, int row, int col, const InputAction& action) const {
    if (row < 0 || row > 15 || col < 0 || col > 1 || action.type != ActionType::SET_VALUE) return false;
    const int value = std::clamp(action.value, -24, 24);
    if (ins.synthChordBanks[row][col] == value) return false;
    ins.synthChordBanks[row][col] = value;
    return true;
}
}
