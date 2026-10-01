#pragma once
#include <algorithm>
#include <array>
#include "model.h"
#include "../synth-oscillator.h"
namespace songcore {
inline constexpr const char* SYNTH_CHORD_NAMES[] = {"off", "uni", "bnk"};
inline std::array<int, 3> synth_chord_intervals(const Instrument& ins) {
    if (ins.synthChordMode == 1) return {0,0,0};
    const auto& bank = ins.synthChordBanks[std::clamp(ins.synthChordBank, 0, 15)];
    return {0, bank[0], bank[1]};
}
inline const char* synth_chord_name(const std::array<int, 2>& bank) {
    constexpr const char* names[] = {"maj", "min", "su2", "su4", "pwr", "dim", "aug", "mi7"};
    for (int i=0; i<8; ++i) if (bank == DEFAULT_CHORD_BANKS[i]) return names[i];
    return bank == std::array<int,2>{0,0} ? "uni" : "cus";
}
}
