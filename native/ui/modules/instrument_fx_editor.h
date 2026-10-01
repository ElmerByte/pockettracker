#pragma once

#include "songcore/midi_map.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

class InstrumentFxEditorModule {
  public:
    static constexpr int ROWS = 9;
    void draw(Canvas& c, const songcore::Instrument& ins, int row, const Theme& t) const;
    CursorContext cursor_context(const songcore::Instrument& ins, int row) const;
    bool handle_input(songcore::Instrument& ins, int row, const InputAction& action) const;
    songcore::MapTarget map_target(int row) const;
};

}  // namespace pt::ui
