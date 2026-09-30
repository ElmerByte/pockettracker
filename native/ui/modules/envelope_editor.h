#pragma once

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// A visual editor for the synth's volume and filter envelopes. They use the existing first two mod
// slots; the other two slots and their routing stay available on MODS.
class EnvelopeEditorModule {
public:
    void draw(Canvas& c, const songcore::Instrument& ins, int slot, int cursor,
              const Theme& theme) const;
    CursorContext cursor_context(const songcore::Instrument& ins, int slot, int cursor) const;
    bool handle_input(songcore::Instrument& ins, int slot, int cursor,
                      const InputAction& action) const;
};

}  // namespace pt::ui
