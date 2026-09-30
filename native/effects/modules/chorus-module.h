#pragma once

#include <algorithm>
#include <cmath>

// Stereo chorus send bus: two short, independently modulated delay taps.
struct ChorusModule {
    static constexpr int kBufferSize = 8192; // covers 30 ms through 192 kHz
    float left[kBufferSize] = {};
    float right[kBufferSize] = {};
    int write = 0;
    float phase = 0.0f;
    float sampleRate = 44100.0f;
    float phaseStep = 0.0f;
    float depthSamples = 0.0f;
    float mix = 0.0f;
    float targetMix = 0.0f;

    void reset(float sr) {
        std::fill_n(left, kBufferSize, 0.0f);
        std::fill_n(right, kBufferSize, 0.0f);
        write = 0;
        phase = 0.0f;
        sampleRate = sr;
        mix = targetMix = 0.0f;
    }

    void setParams(int rate, int depth, int wet) {
        const float hz = 0.1f + (std::clamp(rate, 0, 255) / 255.0f) * 4.9f;
        phaseStep = 6.2831853f * hz / sampleRate;
        depthSamples = sampleRate * 0.005f * (std::clamp(depth, 0, 255) / 255.0f);
        targetMix = 0.5f * (std::clamp(wet, 0, 255) / 255.0f);
    }

    float tap(const float* line, float delay) const {
        float pos = static_cast<float>(write) - delay;
        if (pos < 0.0f) pos += kBufferSize;
        const int index = static_cast<int>(pos);
        const int next = (index + 1) % kBufferSize;
        const float frac = pos - index;
        return line[index] + (line[next] - line[index]) * frac;
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, int frames) {
        const float base = std::min(sampleRate * 0.012f, static_cast<float>(kBufferSize - 2));
        const float mixStep = 1.0f / (sampleRate * 0.005f);
        for (int i = 0; i < frames; ++i) {
            left[write] = inL[i];
            right[write] = inR[i];
            // A short ramp avoids a click when MIX is edited during playback.
            mix += std::clamp(targetMix - mix, -mixStep, mixStep);
            if (mix > 0.0f) {
                const float modL = std::sin(phase);
                const float modR = std::cos(phase);
                const float wetL = tap(left, base + depthSamples * modL);
                const float wetR = tap(right, base + depthSamples * modR);
                outL[i] = wetL * mix;
                outR[i] = wetR * mix;
            } else {
                outL[i] = outR[i] = 0.0f;
            }
            write = (write + 1) % kBufferSize;
            phase += phaseStep;
            if (phase >= 6.2831853f) phase -= 6.2831853f;
        }
    }
};
