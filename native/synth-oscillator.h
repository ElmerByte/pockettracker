#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "rng.h"

// Shared, read-only tables are built before playback; no allocation or initialization in the callback.
namespace synth {
inline constexpr int CYCLE = 1024;
inline constexpr int NOISE = 4;

inline std::array<int, 3> chordIntervals(int mode, int second, int third) {
    switch (mode) {
        case 1: return {0, 0, 0};
        case 2: return {0, 4, 7};
        case 3: return {0, 3, 7};
        case 4: return {0, 5, 7};
        case 5: return {0, 7, 12};
        default: return {0, std::clamp(second, -24, 24), std::clamp(third, -24, 24)};
    }
}
inline std::array<float, 3> chordRatios(int mode, int second, int third, int detune) {
    const auto intervals = chordIntervals(mode, second, third);
    const float spread = std::clamp(detune, 0, 255) / 255.0f * 0.25f;
    std::array<float, 3> ratios;
    for (int n = 0; n < 3; ++n)
        ratios[n] = std::pow(2.0f, (intervals[n] + (n - 1) * spread) / 12.0f);
    return ratios;
}

struct Noise {
    uint32_t state;
    int remaining = 0;
    float held = 0.0f;

    void trigger(int track) {
        state += 0x9e3779b9u * static_cast<uint32_t>(track + 1);
        if (state == 0) state = 1;
        remaining = 0;
    }

    float sample(int downsample) {
        if (remaining <= 0) {
            held = xorshift32Bipolar(state);
            remaining = 1 << std::clamp(downsample, 0, 15);
        }
        --remaining;
        return held;
    }
};
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

// Same interpolation as the square table at width 80. Limit duty to 5..95% so PWM
// never collapses into a constant signal at either end of a modulation sweep.
inline float pulse(double phase, float widthByte, int downsample = 0) {
    phase -= std::floor(phase);
    const double position = phase * CYCLE;
    const int index = std::min(CYCLE - 1, static_cast<int>(position));
    const float edge = std::clamp(widthByte / 256.0f, 0.05f, 0.95f) * CYCLE;
    const auto at = [edge](int i) { return (i % CYCLE) < edge ? 1.0f : -1.0f; };
    if (downsample > 0) {
        const int factor = 1 << std::clamp(downsample, 0, 15);
        return at((index / factor) * factor);
    }
    return at(index) + (at(index + 1) - at(index)) * static_cast<float>(position - index);
}

inline void advance(double& phase, double step) {
    phase += step;
    if (phase >= 1.0) phase -= std::floor(phase);
}
} // namespace synth
