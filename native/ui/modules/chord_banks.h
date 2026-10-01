#pragma once
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"
namespace pt::ui {
class ChordBanksModule {
public:
    void draw(Canvas&, const songcore::Instrument&, int row, int col, const Theme&) const;
    CursorContext cursor_context(const songcore::Instrument&, int row, int col) const;
    bool handle_input(songcore::Instrument&, int row, int col, const InputAction&) const;
};
}
