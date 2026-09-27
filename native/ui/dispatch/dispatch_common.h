// The helpers more than one of the dispatcher's files use. Private to `input_dispatcher.cpp` and
// `ui/dispatch/`; nothing else includes it.
#pragma once

#include "ui/input_dispatcher.h"
#include "ui/std_filesystem.h"   // path_stem

#include <string>

namespace pt::ui {

using songcore::Chain;
using songcore::Instrument;
using songcore::Note;
using songcore::Phrase;
using songcore::Project;

/**
 * One load, opened and closed.
 *
 * ⚠️ **RAII BECAUSE THE LOAD PATHS RETURN EARLY, AND SEVERAL OF THEM DO.** The browser's switch has
 * four arms that `return` from inside it; a hand-written `end_load()` at the bottom would be missed
 * by each of them and would leave `Overlay::LOADING` up forever — an app that has stopped taking
 * input with nothing on screen that ever appeared to explain it.
 */
struct LoadScope {
    LoadScope(InputDispatcher& d, long long now_ms, std::string detail) : d_(d) {
        d_.begin_load(now_ms, std::move(detail));
    }
    ~LoadScope() { d_.end_load(); }

    LoadScope(const LoadScope&)            = delete;
    LoadScope& operator=(const LoadScope&) = delete;

  private:
    InputDispatcher& d_;
};

/** The 20 characters a slot's name is cut to when it is taken from a file. One writer, two readers. */
inline std::string adopted_name(const std::string& stem) { return stem.substr(0, 20); }

/**
 * The name slot `id` would carry if it had adopted its CURRENT source file's — "" when it has no
 * source. Compare a slot's actual name against this to tell an auto-adopted name from a typed one.
 *
 * The source is the SoundFont path for a SoundFont slot and the sample path for everything else; an
 * EXTERNAL slot has neither, and answers "".
 */
inline std::string instrument_auto_name(const Project& p, int id) {
    if (id < 0 || static_cast<size_t>(id) >= p.instruments.size()) return {};
    const Instrument& ins = p.instruments[static_cast<size_t>(id)];
    const std::string source = (ins.instrumentType == songcore::InstrumentType::SOUNDFONT)
                                   ? ins.soundfontPath.value_or("")
                                   : ins.sampleFilePath.value_or("");
    return source.empty() ? std::string{} : adopted_name(path_stem(source));
}

}  // namespace pt::ui
