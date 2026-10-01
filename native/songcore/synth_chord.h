#pragma once
#include <algorithm>
#include <array>
#include "model.h"

namespace songcore {
inline constexpr const char* SYNTH_CHORD_NAMES[] = {"off", "uni", "maj", "min", "sus", "pwr", "cus"};
inline std::array<int, 3> synth_chord_intervals(const Instrument& ins) {
    switch (ins.synthChordMode) {
        case 1: return {0, 0, 0};
        case 2: return {0, 4, 7};
        case 3: return {0, 3, 7};
        case 4: return {0, 5, 7};
        case 5: return {0, 7, 12};
        default: return {0, std::clamp(ins.synthChordInterval2, -24, 24),
                           std::clamp(ins.synthChordInterval3, -24, 24)};
    }
}
} // namespace songcore
