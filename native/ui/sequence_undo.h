#pragma once

#include <deque>
#include <optional>
#include <tuple>
#include <utility>

#include "songcore/model.h"
#include "ui/screen.h"

namespace pt::ui {

// UI-owned history: copying these grids never runs on the audio thread. Include the pools so a
// deep clone is undone together with the reference that made it reachable. Mixer state stays live.
class SequenceUndo {
  public:
    struct Position {
        ScreenType screen = ScreenType::SONG;
        int phrase = 0, chain = 0, row = 0, column = 1;
    };

  private:
    struct State {
        Position position;
        std::vector<songcore::Phrase> phrases;
        std::vector<songcore::Chain> chains;
        std::vector<std::vector<int>> song;

        explicit State(const songcore::Project& p) : phrases(p.phrases), chains(p.chains) {
            for (const auto& track : p.tracks) song.push_back(track.chainRefs);
        }

        static auto fields(const songcore::PhraseStep& s) {
            return std::tie(s.note.pitch, s.note.octave, s.instrument, s.volume,
                            s.fx1Type, s.fx1Value, s.fx2Type, s.fx2Value, s.fx3Type, s.fx3Value);
        }

        bool operator==(const State& other) const {
            if (song != other.song || phrases.size() != other.phrases.size() ||
                chains.size() != other.chains.size()) return false;
            for (size_t i = 0; i < phrases.size(); ++i) {
                const auto& a = phrases[i];
                const auto& b = other.phrases[i];
                if (a.id != b.id || a.steps.size() != b.steps.size()) return false;
                for (size_t j = 0; j < a.steps.size(); ++j)
                    if (fields(a.steps[j]) != fields(b.steps[j])) return false;
            }
            for (size_t i = 0; i < chains.size(); ++i) {
                const auto& a = chains[i];
                const auto& b = other.chains[i];
                if (a.id != b.id || a.phraseRefs != b.phraseRefs ||
                    a.transposeValues != b.transposeValues) return false;
            }
            return true;
        }

        void restore(songcore::Project& p) const {
            p.phrases = phrases;
            p.chains = chains;
            for (size_t i = 0; i < song.size() && i < p.tracks.size(); ++i)
                p.tracks[i].chainRefs = song[i];
        }
    };

    // ponytail: full grid snapshots, capped at 64 edits; use cell deltas if larger histories are needed.
    static constexpr size_t kLimit = 64;
    std::optional<State> baseline_;
    std::deque<State> undo_, redo_;

    static void push(std::deque<State>& stack, State state) {
        if (stack.size() == kLimit) stack.pop_front();
        stack.push_back(std::move(state));
    }

    bool travel(songcore::Project& p, std::deque<State>& from, std::deque<State>& to,
                Position& position) {
        if (from.empty()) return false;
        State current(p);
        current.position = position;
        // A replacement/cleanup outside the editors must never resurrect stale references.
        if (!baseline_ || !(current == *baseline_)) { reset(p); return false; }
        push(to, std::move(current));
        baseline_ = std::move(from.back());
        from.pop_back();
        baseline_->restore(p);
        position = baseline_->position;
        return true;
    }

  public:
    // The mapper calls this before each press: record the source cursor even when a clone moves it.
    void before_edit(Position position) {
        if (baseline_) baseline_->position = position;
    }

    void reset(const songcore::Project& p) {
        undo_.clear();
        redo_.clear();
        baseline_.emplace(p);
    }

    // Called after mutations. Non-grid changes only invalidate history if they change sequence data.
    void record(const songcore::Project& p, bool gridEdit, Position position) {
        State current(p);
        current.position = position;
        if (!baseline_) { baseline_ = std::move(current); return; }
        if (current == *baseline_) return;
        if (gridEdit) push(undo_, std::move(*baseline_));
        else undo_.clear();
        redo_.clear();
        baseline_ = std::move(current);
    }

    bool undo(songcore::Project& p, Position& position) { return travel(p, undo_, redo_, position); }
    bool redo(songcore::Project& p, Position& position) { return travel(p, redo_, undo_, position); }
};

}  // namespace pt::ui
