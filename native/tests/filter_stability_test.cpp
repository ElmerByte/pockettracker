// Run: c++ -std=c++17 -Inative native/tests/filter_stability_test.cpp \
//      native/effects/primitives/daisysp/svf.cpp -o /tmp/pt-filter-test && /tmp/pt-filter-test
#include <cassert>
#include <cmath>
#include <initializer_list>

#include "effects/modules/filter-module.h"

int main() {
    // A full-scale square used to make the SVF diverge at high CUT/RES.
    for (int sampleRate : {44100, 48000}) {
        for (int type = 1; type <= 3; ++type) {
            for (int drive : {128, 255}) {
                FilterModule filter;
                filter.reset();
                filter.setParams(type, 255, 255, drive, static_cast<float>(sampleRate));
                filter.snapshotCoeffs();
                for (int i = 0; i < sampleRate; ++i) {
                    const float phase = std::sin(6.2831853f * 220.0f * i / sampleRate);
                    filter.setInterpolatedCoeffs(1.0f);
                    const float out = filter.processMono(phase >= 0.0f ? 1.0f : -1.0f);
                    assert(std::isfinite(out));
                }
            }
        }
    }
}
