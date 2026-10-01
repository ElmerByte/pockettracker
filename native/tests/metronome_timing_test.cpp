// Link against the host libpockettracker.a and codec libraries.
#include "audio-engine.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <memory>

int main() {
    for (bool mutedByGain : {false, true}) {
        auto engine = std::make_unique<AudioEngine>();
        engine->setDeviceSampleRate(48000);
        engine->startMetronome(0, 1024);
        engine->setMetronome(mutedByGain, mutedByGain ? 0.0f : 0.5f);
        std::array<float, 256> audio{};
        const auto silentBlock = [&] {
            engine->processLiveBlock(audio.data(), 128, 2, 48000);
            for (float value : audio) assert(std::abs(value) < 0.000001f);
        };
        for (int block = 0; block < 5; ++block) silentBlock();
        engine->setMetronome(true, 0.5f); // Enable at frame 640, between beats.
        for (int block = 0; block < 3; ++block) silentBlock();
        engine->processLiveBlock(audio.data(), 128, 2, 48000); // Beat at frame 1024.
        assert(std::any_of(audio.begin(), audio.end(), [](float value) {
            return std::abs(value) > 0.01f;
        }));
        engine->stopMetronome();
        silentBlock();
    }
}
