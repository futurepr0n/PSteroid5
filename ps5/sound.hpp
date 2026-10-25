#pragma once
// Game sound effects, matching CSSteroids-2's public/js/core/sound.js: the same recipes and the
// same xorshift32 noise render the same samples (tools/sound-evidence.js compares them). The
// thruster loop is a recording (Kenney "Sci-Fi Sounds", CC0) passed in as 48 kHz mono PCM.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cssteroids::sound {
constexpr int rate = 48000;

// Shot sounds a ship can carry (the server's Ships.shotSound); anything else plays the default.
inline const std::array<std::string, 4> shot_sounds = {"pew", "zap", "laser", "arcade"};
inline const std::string default_shot = "pew";
inline std::string valid_shot(const std::string &s) {
    return std::find(shot_sounds.begin(), shot_sounds.end(), s) != shot_sounds.end() ? s : default_shot;
}

enum Sfx { shot_pew, shot_zap, shot_laser, shot_arcade, ship_explode, ship_explode_crushed,
           asteroid_large, asteroid_medium, asteroid_small, beat_low, beat_high, sfx_count };
inline const char *const sfx_names[sfx_count] = {"shot_pew", "shot_zap", "shot_laser", "shot_arcade", "ship_explode",
    "ship_explode_crushed", "asteroid_large", "asteroid_medium", "asteroid_small", "beat_low", "beat_high"};

// xorshift32, uniform in [-1, 1)
struct Noise {
    uint32_t x;
    explicit Noise(uint32_t seed) : x(seed) {}
    double next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x / 2147483648.0 - 1; }
};

inline double square(double p) { return p < 0.5 ? 1 : -1; }
inline double triangle(double p) { return 4 * std::fabs(p - 0.5) - 1; }
inline double laser_wave(double p) { return std::sin(2 * M_PI * p) + 0.4 * square(p); }

// A tone gliding from f0 to f1 Hz, decaying with time constant `decay` seconds
template <class Wave>
std::vector<float> sweep(double dur, double f0, double f1, Wave wave, double decay, double curve) {
    std::vector<float> out(size_t(std::floor(dur * rate)));
    double phase = 0;
    for (size_t i = 0; i < out.size(); i++) {
        const double t = double(i) / rate;
        phase += (f0 + (f1 - f0) * std::pow(t / dur, curve)) / rate;
        out[i] = float(wave(std::fmod(phase, 1.0)) * std::exp(-t / decay));
    }
    return out;
}

// Noise through a one-pole low-pass whose cutoff falls from c0 + floor to floor Hz;
// crush > 1 holds each noise value for that many samples
inline std::vector<float> boom(double dur, double c0, double fall, double floor, double decay, uint32_t seed, int crush) {
    std::vector<float> out(size_t(std::floor(dur * rate)));
    Noise noise(seed);
    double y = 0, held = 0;
    for (size_t i = 0; i < out.size(); i++) {
        const double t = double(i) / rate;
        const double a = 1 - std::exp(-2 * M_PI * (c0 * std::exp(-t / fall) + floor) / rate);
        if (!crush || i % size_t(crush) == 0) held = noise.next();
        y += a * (held - y);
        out[i] = float(y * std::exp(-t / decay));
    }
    return out;
}

inline std::vector<float> normalize(std::vector<float> s) {
    double peak = 1e-9;
    for (float v : s) peak = std::max(peak, double(std::fabs(v)));
    for (auto &v : s) v = float(v / peak * 0.8);
    return s;
}

inline std::vector<float> render(Sfx which) {
    switch (which) {
        case shot_pew: return normalize(sweep(0.18, 1400, 300, square, 0.06, 0.6));
        case shot_zap: return normalize(sweep(0.12, 2200, 600, triangle, 0.04, 0.4));
        case shot_laser: return normalize(sweep(0.25, 1800, 200, laser_wave, 0.09, 0.5));
        case shot_arcade: return normalize(sweep(0.09, 900, 500, square, 0.035, 1));
        case ship_explode: return normalize(boom(1.6, 3000, 0.4, 80, 0.5, 7, 0));
        case ship_explode_crushed: return normalize(boom(1.6, 2500, 0.5, 100, 0.55, 8, 6));
        case asteroid_large: return normalize(boom(0.7, 900, 0.7 / 3, 60, 0.7 / 3.5, 11, 0));
        case asteroid_medium: return normalize(boom(0.45, 1600, 0.45 / 3, 60, 0.45 / 3.5, 11, 0));
        case asteroid_small: return normalize(boom(0.25, 2800, 0.25 / 3, 60, 0.25 / 3.5, 11, 0));
        case beat_low: return normalize(sweep(0.1, 55, 50, square, 0.05, 1));
        case beat_high: return normalize(sweep(0.1, 62, 57, square, 0.05, 1));
        default: return {};
    }
}

// Mixes one-shot effects and the thruster loop. The game thread calls play()/set_thrust(); the
// audio thread calls fill() for 48 kHz interleaved stereo.
class Mixer {
public:
    Mixer(const int16_t *thrust, size_t thrustFrames) : thrust_(thrust), thrustFrames_(thrustFrames) {
        for (int i = 0; i < sfx_count; i++) buffers_[size_t(i)] = render(Sfx(i));
    }

    void set_volume(float v) { volume_.store(std::clamp(v, 0.f, 1.f)); }
    float volume() const { return volume_.load(); }

    void play(Sfx which, float gain = 1) {
        if (volume_.load() <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Voice *slot = &voices_[0];
        for (auto &v : voices_) { if (!v.data) { slot = &v; break; } if (v.pos > slot->pos) slot = &v; } // reuse the oldest
        const auto &b = buffers_[size_t(which)];
        *slot = {b.data(), b.size(), 0, gain};
    }
    void shot(const std::string &name) {
        const auto it = std::find(shot_sounds.begin(), shot_sounds.end(), valid_shot(name));
        play(Sfx(shot_pew + int(it - shot_sounds.begin())), 0.5f);
    }
    // Ship explosions alternate at random between the two explosion sounds
    void explosion(float gain = 1) { play((random_() & 1) ? ship_explode : ship_explode_crushed, gain); }
    // Asteroid sizes: 3 large, 2 medium, 1 small
    void asteroid_break(int size, float gain = 1) {
        play(size >= 3 ? asteroid_large : size == 2 ? asteroid_medium : asteroid_small, gain * 0.7f);
    }
    // Reaching the world boundary: alternating low and high beats, at most every 0.25 s
    void boundary(double nowSeconds) {
        if (nowSeconds < nextBeat_) return;
        nextBeat_ = nowSeconds + 0.25;
        lowBeat_ = !lowBeat_;
        play(lowBeat_ ? beat_low : beat_high, 0.9f);
    }
    // The player's own thrusters: the loop fades in and out
    void set_thrust(bool on) { thrustOn_.store(on && thrustFrames_ > 0); }

    void fill(int16_t *frames, int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        const float vol = volume_.load();
        const float target = thrustOn_.load() && vol > 0 ? 0.45f : 0.f;
        // time constants as in the browser: 30 ms in, 40 ms out
        const float in = 1 - std::exp(-1.f / (0.03f * rate)), out = 1 - std::exp(-1.f / (0.04f * rate));
        for (int i = 0; i < count; i++) {
            float mix = 0;
            for (auto &v : voices_) {
                if (!v.data) continue;
                mix += v.data[v.pos++] * v.gain;
                if (v.pos >= v.size) v.data = nullptr;
            }
            thrustGain_ += (target - thrustGain_) * (target > thrustGain_ ? in : out);
            if (thrustGain_ > 1e-4f) {
                mix += thrust_[thrustPos_] / 32768.f * thrustGain_;
                if (++thrustPos_ >= thrustFrames_) thrustPos_ = 0;
            } else if (target == 0) thrustPos_ = 0; // each burst starts at the loop's start, like the browser
            mix *= vol;
            const auto s = int16_t(std::lrint(std::clamp(mix, -1.f, 1.f) * 32767));
            frames[2 * i] = frames[2 * i + 1] = s;
        }
    }
    const std::vector<float> &buffer(Sfx which) const { return buffers_[size_t(which)]; }

private:
    struct Voice { const float *data = nullptr; size_t size = 0, pos = 0; float gain = 1; };
    std::array<std::vector<float>, sfx_count> buffers_;
    std::array<Voice, 24> voices_{};
    const int16_t *thrust_;
    size_t thrustFrames_, thrustPos_ = 0;
    float thrustGain_ = 0;
    std::atomic<bool> thrustOn_{false};
    std::atomic<float> volume_{0.8f};
    std::mutex mutex_;
    double nextBeat_ = 0;
    bool lowBeat_ = false;
    uint32_t seed_ = 0x9e3779b9u;
    uint32_t random_() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return seed_ >> 7; }
};
}
