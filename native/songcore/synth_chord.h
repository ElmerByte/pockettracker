#pragma once
#include <algorithm>
#include <array>
#include "model.h"
#include "../synth-oscillator.h"

namespace songcore {
inline constexpr const char* SYNTH_CHORD_NAMES[] = {"off", "uni", "maj", "min", "sus", "pwr", "cus"};
inline std::array<int, 3> synth_chord_intervals(const Instrument& ins) {
    return synth::chordIntervals(ins.synthChordMode, ins.synthChordInterval2, ins.synthChordInterval3);
}
} // namespace songcore
