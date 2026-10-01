#pragma once

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// Dedicated synth AMP/FILT envelopes; all four MODS slots remain freely assignable.
class EnvelopeEditorModule {
public:
    void draw(Canvas& c, const songcore::Instrument& ins, int slot, int cursor,
              const Theme& theme) const;
    CursorContext cursor_context(const songcore::Instrument& ins, int slot, int cursor) const;
    bool handle_input(songcore::Instrument& ins, int slot, int cursor,
                      const InputAction& action) const;
};

}  // namespace pt::ui
