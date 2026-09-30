// Run: c++ -std=c++17 -Inative native/tests/sequence_undo_test.cpp native/ui/clipboard.cpp -o /tmp/pt-sequence-undo-test && /tmp/pt-sequence-undo-test
#include <cassert>
#include <cstdio>

#include "ui/clipboard.h"
#include "ui/sequence_undo.h"

int main() {
    using namespace pt;
    auto p = songcore::make_default_project();
    ui::SequenceUndo history;
    ui::SequenceUndo::Position pos{ui::ScreenType::PHRASE, 0, 0, 0, 1};
    ui::Clipboard clipboard;
    history.reset(p);

    history.before_edit(pos);
    p.phrases[0].steps[0].note = songcore::Note::C4();
    p.phrases[0].steps[0].volume = 95;
    p.phrases[0].steps[0].fx3Type = 4;
    p.phrases[0].steps[0].fx3Value = 33;
    history.record(p, true, pos);
    history.record(p, true, pos);  // a no-op must not consume an undo step

    // Use the actual cut/paste code; undo restores every cell without changing the clipboard.
    history.before_edit(pos);
    clipboard.cut_phrase_steps(p, 0, 0, 1, 0, 9);
    history.record(p, true, pos);
    assert(songcore::step_empty(p.phrases[0].steps[0]));
    assert(history.undo(p, pos));
    assert(p.phrases[0].steps[0].note == songcore::Note::C4());
    assert(p.phrases[0].steps[0].volume == 95 && p.phrases[0].steps[0].fx3Value == 33);
    history.before_edit(pos);
    assert(clipboard.paste(p, ui::ScreenType::PHRASE, 1, 0, 1).itemsPasted > 0);
    history.record(p, true, pos);
    assert(!history.redo(p, pos));  // editing after undo clears redo
    assert(history.undo(p, pos));
    assert(songcore::step_empty(p.phrases[1].steps[0]));

    // A clone edits multiple pools and follows the new phrase. Restore its source position too.
    history.before_edit(pos);
    p.phrases[1].steps = p.phrases[0].steps;
    p.chains[1].phraseRefs[0] = 1;
    p.chains[1].transposeValues[0] = -12;
    p.tracks[0].chainRefs = {1};
    pos.phrase = 1;
    history.record(p, true, pos);
    p.tracks[0].mute = true;
    p.tracks[0].volume = 42;
    p.tempo = 140;
    history.record(p, false, pos);  // other settings preserve sequence history
    pos.screen = ui::ScreenType::SONG;
    assert(history.undo(p, pos));
    assert(pos.screen == ui::ScreenType::PHRASE && pos.phrase == 0);
    assert(songcore::step_empty(p.phrases[1].steps[0]));
    assert(p.chains[1].phraseRefs[0] == -1 && p.chains[1].transposeValues[0] == 0);
    assert(p.tracks[0].chainRefs.empty());
    assert(p.tracks[0].mute && p.tracks[0].volume == 42 && p.tempo == 140);
    assert(history.redo(p, pos));
    assert(p.chains[1].phraseRefs[0] == 1 && p.tracks[0].chainRefs[0] == 1);

    // SONG deletion is one operation, with all affected tracks restored together.
    history.before_edit(pos);
    clipboard.delete_song_cells(p, 0, 1, 0, 8);
    history.record(p, true, pos);
    assert(history.undo(p, pos));
    assert(p.tracks[0].chainRefs[0] == 1);

    // Project replacement and sequence cleanup cannot resurrect references from the old data.
    history.reset(p);
    assert(!history.undo(p, pos) && !history.redo(p, pos));
    p.chains[1].phraseRefs[0] = 2;
    history.record(p, true, pos);
    p.chains[1].phraseRefs[0] = -1;
    history.record(p, false, pos);
    assert(!history.undo(p, pos));

    // Bound memory and verify that repeated edits can all be undone/redone within that limit.
    history.reset(p);
    for (int i = 1; i <= 70; ++i) {
        history.before_edit(pos);
        p.phrases[0].steps[0].volume = i;
        history.record(p, true, pos);
    }
    for (int i = 0; i < 64; ++i) assert(history.undo(p, pos));
    assert(p.phrases[0].steps[0].volume == 6 && !history.undo(p, pos));
    for (int i = 0; i < 64; ++i) assert(history.redo(p, pos));
    assert(p.phrases[0].steps[0].volume == 70 && !history.redo(p, pos));
    std::puts("Sequence undo checks passed");
}
