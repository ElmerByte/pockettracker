// Run: c++ -std=c++17 -Inative native/tests/chorus_test.cpp -o /tmp/pt-chorus-test && /tmp/pt-chorus-test
#include <cassert>
#include <cmath>

#include "effects/modules/chorus-module.h"

int main() {
    ChorusModule chorus;
    chorus.reset(48000.0f);
    chorus.setParams(0x40, 0x80, 0);
    float left = 1.0f, right = -1.0f, wetL = 1.0f, wetR = 1.0f;
    chorus.process(&left, &right, &wetL, &wetR, 1);
    assert(wetL == 0.0f && wetR == 0.0f);

    chorus.reset(48000.0f);
    chorus.setParams(0x40, 0x80, 0xFF);
    float peak = 0.0f;
    float difference = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const float input = std::sin(6.2831853f * 440.0f * i / 48000.0f);
        chorus.process(&input, &input, &wetL, &wetR, 1);
        assert(std::isfinite(wetL) && std::isfinite(wetR));
        peak = std::max(peak, std::abs(wetL));
        difference += std::abs(wetL) + std::abs(wetL - wetR);
    }
    assert(peak < 1.1f && difference > 100.0f);
}
