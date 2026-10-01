// Run: cc -Inative/effects/soundpipe -c native/effects/soundpipe/pareq.c -o /tmp/pt-pareq.o
//      c++ -std=c++17 -Inative native/tests/delay_tone_test.cpp native/effects/primitives/daisysp/svf.cpp /tmp/pt-pareq.o -o /tmp/pt-delay-test && /tmp/pt-delay-test
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>
#include "effects/modules/delay-module.h"

int main() {
    for (float rate : {44100.0f, 48000.0f, 96000.0f}) {
        std::vector<float> input(512), silence(512), clean(512), dark(512), right(512);
        input[0] = 1.0f;
        const auto render = [&](int tone, std::vector<float>& output) {
            auto delay = std::make_unique<DelayModule>();
            delay->reset(rate);
            delay->setParamsSync(2, 0, rate * 60.0f / 128.0f);
            delay->setCharacter(false, tone, 0);
            delay->process(input.data(), silence.data(), output.data(), right.data(), 512);
            for (int i = 0; i < 512; ++i) {
                assert(std::isfinite(output[i]));
                assert(right[i] == 0);
            }
        };
        render(0xFF, clean);
        render(0, dark);
        // FF bypasses the filter and preserves the impulse and delay time exactly.
        for (int i = 0; i < 512; ++i)
            assert(std::abs(clean[i] - (i == 128 ? 1.0f : 0.0f)) < 1e-6f);
        // A dark TONE now filters the FIRST repeat, even with no feedback.
        assert(dark[128] > 0 && dark[128] < clean[128] * 0.1f);
        assert(dark[129] > 0);
    }
}
