#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Shared, read-only tables are built before playback; no allocation or initialization in the callback.
namespace synth {
inline constexpr int CYCLE = 1024;
inline const auto waves = [] {
    std::array<std::array<float, CYCLE + 1>, 4> result{};
    for (int i = 0; i <= CYCLE; ++i) {
        const float phase = static_cast<float>(i % CYCLE) / CYCLE;
        result[0][i] = std::sin(6.28318530718f * phase);
        result[1][i] = 1.0f - 4.0f * std::abs(phase - 0.5f);
        result[2][i] = 2.0f * phase - 1.0f;
        result[3][i] = phase < 0.5f ? 1.0f : -1.0f;
    }
    return result;
}();

inline float sample(int wave, double phase, int downsample = 0) {
    const double position = phase * CYCLE;
    int index = std::min(CYCLE - 1, static_cast<int>(position));
    const auto& table = waves[std::clamp(wave, 0, 3)];
    if (downsample > 0) {
        const int factor = 1 << std::clamp(downsample, 0, 15);
        return table[(index / factor) * factor];
    }
    return table[index] + (table[index + 1] - table[index]) * static_cast<float>(position - index);
}

inline void advance(double& phase, double step) {
    phase += step;
    if (phase >= 1.0) phase -= std::floor(phase);
}
} // namespace synth
