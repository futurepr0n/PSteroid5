// Host tests for sound.hpp, and the samples tools/sound-evidence.js compares with the browser.
// usage: test-sound [out.json]
#include "sound.hpp"
#include <cstdio>
#include <fstream>

using namespace cssteroids::sound;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
    {   // every effect renders, at the expected length, normalised to 0.8
        const size_t lengths[sfx_count] = {8640, 5760, 12000, 4320, 76800, 76800, 33600, 21600, 12000, 4800, 4800};
        for (int i = 0; i < sfx_count; i++) {
            const auto s = render(Sfx(i));
            float peak = 0;
            for (float v : s) peak = std::max(peak, std::fabs(v));
            CHECK(s.size() == lengths[i]);
            CHECK(std::fabs(peak - 0.8f) < 1e-6f);
        }
        CHECK(valid_shot("zap") == "zap" && valid_shot("") == "pew" && valid_shot("boom") == "pew");
    }
    {   // mixer: silence, a shot, the thruster fading in and out, volume and mute
        std::vector<int16_t> thrust(4800, 16000), out(2 * 4800);
        Mixer m(thrust.data(), thrust.size());
        m.fill(out.data(), 256);
        bool silent = true; for (int i = 0; i < 512; i++) silent &= out[size_t(i)] == 0;
        CHECK(silent);
        m.shot("laser");
        m.fill(out.data(), 256);
        int peak = 0; for (int i = 0; i < 512; i++) peak = std::max(peak, std::abs(int(out[size_t(i)])));
        CHECK(peak > 1000 && out[100] == out[101]); // stereo channels match
        Mixer t(thrust.data(), thrust.size());
        t.set_thrust(true);
        for (int k = 0; k < 3; k++) t.fill(out.data(), 4800); // 300 ms: fully faded in (30 ms time constant)
        CHECK(std::abs(out[2 * 4799] - int(16000 * 0.45f * 0.8f)) < 60);
        t.set_thrust(false);
        for (int k = 0; k < 6; k++) t.fill(out.data(), 4800); // 40 ms time constant
        CHECK(std::abs(int(out[2 * 4799])) < 30);  // faded out
        t.set_volume(0);
        t.set_thrust(true);
        t.fill(out.data(), 4800);
        CHECK(out[2 * 4799] == 0);                 // muted: no thruster either
        Mixer b(thrust.data(), thrust.size());
        b.boundary(1.0); b.boundary(1.1);          // the second beat is too soon
        b.boundary(1.3);
        int voices = 0; b.fill(out.data(), 1); voices = out[0] != 0;
        CHECK(voices);
    }
    if (argc > 1) { // samples for the browser comparison
        std::ofstream f(argv[1]);
        f << "{";
        for (int i = 0; i < sfx_count; i++) {
            f << (i ? "," : "") << "\"" << sfx_names[i] << "\":[";
            const auto s = render(Sfx(i));
            char num[32];
            for (size_t k = 0; k < s.size(); k++) { std::snprintf(num, sizeof num, "%s%.9g", k ? "," : "", double(s[k])); f << num; }
            f << "]";
        }
        f << "}\n";
    }
    if (failures) { std::printf("%d sound test failure(s)\n", failures); return 1; }
    std::puts("sound tests passed");
    return 0;
}
