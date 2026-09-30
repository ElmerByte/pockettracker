// Run: c++ -std=c++17 -Inative native/tests/synth_sync_test.cpp -o /tmp/pt-sync-test && /tmp/pt-sync-test
#include <cassert>
#include <cmath>
#include <vector>

#include "songcore/engine_setup.h"
#include "songcore/project_io.h"

struct Capture {
    std::vector<float> wave;
    bool loadSample(int, const float* data, int size) {
        wave.assign(data, data + size);
        return true;
    }
};

int main() {
    songcore::Instrument ins;
    Capture out;
    assert(songcore::load_synth_wave(out, ins));
    const float original = out.wave[128];
    assert(std::abs(original - 0.7071f) < 0.001f);

    ins.synthSync = 255; // Eight slave cycles per note period.
    assert(songcore::load_synth_wave(out, ins));
    assert(std::abs(out.wave[128]) < 0.001f);
    assert(out.wave.front() == out.wave.back()); // Seam stays a whole master period.

    auto project = songcore::make_default_project();
    project.instruments[0].synthSync = 123;
    const auto saved = songcore::serialize_project(project);
    const auto loaded = songcore::parse_project(songcore::json::parse(saved));
    assert(loaded.instruments[0].synthSync == 123);
}
