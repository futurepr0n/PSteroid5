#pragma once
// Local best score, adapted from the BattleStar port (PSPGameCPP/ps5/score_store.hpp).
// Kept out of the simulation; a failed save is non-fatal.
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace cssteroids {
struct ScoreStore {
    static constexpr int limit = 999999999;
    static constexpr const char *prefix = "CSSTEROIDS1 ";
    std::array<std::string, 2> paths{};
    int best = 0;
    bool dirty = false, failed = false;

    static int read(const std::string &path) {
        auto *file = std::fopen(path.c_str(), "rb");
        if (!file) return 0;
        char data[64]{};
        const size_t n = std::fread(data, 1, sizeof(data) - 1, file);
        const bool oversized = std::fgetc(file) != EOF;
        std::fclose(file);
        const std::string contents(data, n), head(prefix);
        if (oversized || contents.compare(0, head.size(), head) != 0) return 0;
        int value = 0;
        size_t used = head.size();
        const size_t start = used;
        while (used < n && data[used] >= '0' && data[used] <= '9') {
            const int digit = data[used++] - '0';
            if (value > (limit - digit) / 10) return 0;
            value = value * 10 + digit;
        }
        if (used == start) return 0;
        for (size_t i = used; i < n; i++) if (data[i] != '\n' && data[i] != '\r' && data[i] != ' ') return 0;
        return value;
    }
    void init(const std::string &preferred, const std::string &fallback) {
        paths = {preferred, fallback};
        best = 0; dirty = failed = false;
        for (const auto &p : paths) best = std::max(best, read(p));
    }
    bool record(int score) {
        score = std::clamp(score, 0, limit);
        if (score > best) { best = score; dirty = true; }
        if (!dirty) return !failed;
        for (const auto &path : paths) {
            if (path.empty()) continue;
            const auto split = path.find_last_of('/');
            if (split != std::string::npos) mkdir(path.substr(0, split).c_str(), 0777);
            const auto temp = path + ".tmp";
            auto *file = std::fopen(temp.c_str(), "wb");
            if (!file) continue;
            bool ok = std::fprintf(file, "%s%d\n", prefix, best) > 0;
            if (std::fflush(file) != 0) ok = false;
            if (std::fclose(file) != 0) ok = false;
            if (ok && std::rename(temp.c_str(), path.c_str()) == 0) { dirty = failed = false; return true; }
            std::remove(temp.c_str());
        }
        failed = true;
        return false;
    }
};
}
