#include "ui/modules/envelope_editor.h"

#include <algorithm>
#include <cstdlib>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

songcore::ModSlot default_slot(int slot) {
    songcore::ModSlot env;
    env.type = songcore::ModType::ADSR;
    env.dest = slot == 0 ? songcore::ModDest::VOLUME : songcore::ModDest::FILTER_CUTOFF;
    env.amount = slot == 0 ? 255 : 192;
    env.decay = slot == 0 ? 0 : 6;
    env.sustain = slot == 0 ? 255 : 0;
    env.release = 6;
    return env;
}

bool slot_active(const songcore::Instrument& ins, int slot) {
    return slot >= 0 && slot < 2 && slot < static_cast<int>(ins.modSlots.size()) &&
           ins.modSlots[static_cast<size_t>(slot)].type == songcore::ModType::ADSR &&
           ins.modSlots[static_cast<size_t>(slot)].dest ==
               (slot == 0 ? songcore::ModDest::VOLUME : songcore::ModDest::FILTER_CUTOFF);
}

songcore::ModSlot shown_slot(const songcore::Instrument& ins, int slot) {
    return slot_active(ins, slot) ? ins.modSlots[static_cast<size_t>(slot)] : default_slot(slot);
}

// The graph's horizontal stages use the same tick values the engine converts to frames. A fixed
// sustain span represents a held key; its actual length depends on the note, so it has no time value.
void line(Canvas& c, int x0, int y0, int x1, int y1, Argb color) {
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    for (int i = 0; i <= steps; ++i)
        c.fill_rect(x0 + dx * i / std::max(steps, 1),
                    y0 + dy * i / std::max(steps, 1), 2, 2, color);
}

}  // namespace

void EnvelopeEditorModule::draw(Canvas& c, const songcore::Instrument& ins, int slot, int cursor,
                                const Theme& t) const {
    const auto env = shown_slot(ins, slot);
    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);
    c.draw_text("ENVELOPE EDITOR", 10, 8, t.textTitle, CHAR_SPACING, FONT_SCALE);
    c.draw_text("INST " + hex2(ins.id) + (slot == 0 ? "  AMP ENV" : "  FILTER ENV"), 10, 34,
                t.textParam,
                CHAR_SPACING, FONT_SCALE);
    if (!slot_active(ins, slot))
        c.draw_text("OFF - EDIT TO ENABLE", 10, 55, t.textEmpty, CHAR_SPACING, FONT_SCALE);

    constexpr int left = 10, top = 78, width = 620, height = 170;
    c.fill_rect(left, top, width, height, t.vizBackground);
    c.stroke_rect(left, top, width, height, t.vizCenterLine);
    const int bottom = top + height - 12;
    const int peak = top + 12;
    const int sustainY = bottom - (bottom - peak) * std::clamp(env.sustain, 0, 255) / 255;
    const int attack = std::clamp(env.attack, 0, 255);
    const int decay = std::clamp(env.decay, 0, 255);
    const int release = std::clamp(env.release, 0, 255);
    const int available = width - 160;
    const int total = std::max(1, attack + decay + release);
    const int x0 = left + 20;
    const int x1 = x0 + available * attack / total;
    const int x2 = x1 + available * decay / total;
    const int x3 = x2 + 100;
    const int x4 = x0 + available + 100;
    c.fill_rect(left + 1, sustainY, width - 2, 1, t.vizCenterLine);
    line(c, x0, bottom, x1, peak, t.textValue);
    line(c, x1, peak, x2, sustainY, t.textValue);
    line(c, x2, sustainY, x3, sustainY, t.textValue);
    line(c, x3, sustainY, x4, bottom, t.textValue);
    c.draw_text("A", x0, top + height + 7, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("D", x1, top + height + 7, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("S", x2, top + height + 7, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("R", x3, top + height + 7, t.textParam, CHAR_SPACING, FONT_SCALE);

    const int values[5] = {env.attack, env.decay, env.sustain, env.release, env.amount};
    const char* labels[5] = {"ATK", "DEC", "SUS", "REL", "AMT"};
    for (int i = 0; i < 5; ++i) {
        const int x = 35 + (i % 2) * 300;
        const int y = 317 + (i / 2) * 38;
        c.draw_text(labels[i], x, y, i == cursor ? cursor_mark_ink(t) : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, hex2(values[i]), x + 90, y, i == cursor, t.textValue, t);
    }
    c.draw_text(slot == 1 && ins.filterType == "off" ? "SET FILTER TO LP ON INSTRUMENT" :
                 "A+DPAD EDIT  B BACK", 35, 440, t.textEmpty, CHAR_SPACING, FONT_SCALE);
}

CursorContext EnvelopeEditorModule::cursor_context(const songcore::Instrument& ins, int slot,
                                                    int cursor) const {
    const auto env = shown_slot(ins, slot);
    const auto defaults = default_slot(slot);
    const int values[5] = {env.attack, env.decay, env.sustain, env.release, env.amount};
    const int defaultValues[5] = {defaults.attack, defaults.decay, defaults.sustain,
                                  defaults.release, defaults.amount};
    if (slot < 0 || slot >= 2 || cursor < 0 || cursor >= 5) return cc::none();
    return cc::hex_byte(values[cursor], 0, 255, -1, false, false, false, defaultValues[cursor]);
}

bool EnvelopeEditorModule::handle_input(songcore::Instrument& ins, int slot, int cursor,
                                        const InputAction& action) const {
    if (slot < 0 || slot >= 2 || cursor < 0 || cursor >= 5 ||
        action.type != ActionType::SET_VALUE) return false;
    if (ins.modSlots.size() < 2) ins.modSlots.resize(4);
    auto& env = ins.modSlots[static_cast<size_t>(slot)];
    const bool initialized = !slot_active(ins, slot);
    if (initialized) {
        env = default_slot(slot);
    }
    int* values[5] = {&env.attack, &env.decay, &env.sustain, &env.release, &env.amount};
    const int next = std::clamp(action.value, 0, 255);
    if (*values[cursor] == next) return initialized;
    *values[cursor] = next;
    return true;
}

}  // namespace pt::ui
