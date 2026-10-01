#include "ui/modules/instrument_fx_editor.h"

#include <algorithm>

#include "ui/helpers.h"
#include "ui/modules/instrument_editor.h"  // the existing filter type names

namespace pt::ui {
namespace {

const char* const labels[] = {"DRIVE", "CRUSH", "DOWNSAMPLE", "FILTER", "CUTOFF",
                             "RESONANCE", "REVERB SEND", "DELAY SEND", "CHORUS SEND"};

int* field(songcore::Instrument& ins, int row) {
    switch (row) {
        case 0: return &ins.drive;
        case 1: return &ins.crush;
        case 2: return &ins.downsample;
        case 4: return &ins.filterCut;
        case 5: return &ins.filterRes;
        case 6: return &ins.reverbSend;
        case 7: return &ins.delaySend;
        case 8: return &ins.chorusSend;
        default: return nullptr;
    }
}

}  // namespace

CursorContext InstrumentFxEditorModule::cursor_context(const songcore::Instrument& ins,
                                                       int row) const {
    if (ins.instrumentType == songcore::InstrumentType::EXTERNAL || row < 0 || row >= ROWS)
        return cc::none();
    if (row == 3) return cc::toggle_ternary(ins.filterType, filter_types());
    const int values[] = {ins.drive, ins.crush, ins.downsample, 0, ins.filterCut,
                          ins.filterRes, ins.reverbSend, ins.delaySend, ins.chorusSend};
    return row == 1 || row == 2 ? cc::hex_nibble(values[row], 0)
                               : cc::hex_byte(values[row], 0, 255, -1, false, false, false, 0);
}

bool InstrumentFxEditorModule::handle_input(songcore::Instrument& ins, int row,
                                           const InputAction& action) const {
    if (ins.instrumentType == songcore::InstrumentType::EXTERNAL ||
        action.type != ActionType::SET_VALUE) return false;
    if (row == 3) {
        if (action.value < 0 || action.value >= static_cast<int>(filter_types().size())) return false;
        const auto& next = filter_types()[static_cast<size_t>(action.value)];
        if (ins.filterType == next) return false;
        ins.filterType = next;
        return true;
    }
    int* value = field(ins, row);
    if (!value) return false;
    const int next = std::clamp(action.value, 0, row == 1 || row == 2 ? 15 : 255);
    if (*value == next) return false;
    *value = next;
    return true;
}

songcore::MapTarget InstrumentFxEditorModule::map_target(int row) const {
    using songcore::MapDestId;
    const MapDestId destinations[] = {MapDestId::INS_DRIVE, MapDestId::INS_CRUSH, MapDestId::INS_DWN,
        MapDestId::NONE, MapDestId::INS_CUT, MapDestId::INS_RES, MapDestId::INS_REV,
        MapDestId::INS_DLY, MapDestId::NONE};
    return row >= 0 && row < ROWS ? songcore::MapTarget{destinations[row], 0} : songcore::MapTarget{};
}

void InstrumentFxEditorModule::draw(Canvas& c, const songcore::Instrument& ins, int row,
                                    const Theme& t) const {
    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);
    c.draw_text("INSTRUMENT FX", 10, 8, t.textTitle, CHAR_SPACING, FONT_SCALE);
    c.draw_text("INST " + hex2(ins.id) + "  " + songcore::instrument_type_name(ins.instrumentType),
                10, 34, t.textParam, CHAR_SPACING, FONT_SCALE);
    if (ins.instrumentType == songcore::InstrumentType::EXTERNAL) return;
    for (int i = 0; i < ROWS; ++i) {
        const auto context = cursor_context(ins, i);
        const std::string value = i == 3 ? ins.filterType :
            i == 1 || i == 2 ? hex1(context.currentValue) : hex2(context.currentValue);
        const int y = 85 + i * 35;
        c.draw_text(labels[i], 35, y, i == row ? cursor_mark_ink(t) : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, 350, y, i == row, t.textValue, t);
    }
    c.draw_text("A+DPAD EDIT  START LISTEN  B BACK", 35, 440, t.textEmpty, CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
