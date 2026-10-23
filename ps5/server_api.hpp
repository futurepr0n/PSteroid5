#pragma once
// CSSteroids server endpoints (CSSTeroids-2 server.js and routes/, unchanged):
//   GET/POST /api/high-scores, GET /api/ships/public, GET /api/ships/passphrase/:p
#include "json_lite.hpp"
#include <array>
#include <string>
#include <vector>

namespace cssteroids::api {
constexpr const char *high_scores_path = "/api/high-scores";
constexpr const char *public_ships_path = "/api/ships/public";

struct ScoreRow {
    std::string name, shipType;
    int score = 0;
};

// The GET body is an array of HighScore rows; anything else is rejected.
inline bool parse_high_scores(const std::string &body, std::vector<ScoreRow> &out) {
    json::Value root;
    if (!json::parse(body, root) || !root.is_array()) return false;
    out.clear();
    for (const auto &row : root.items) {
        if (!row.is_object()) continue;
        ScoreRow s;
        s.name = row.str("name", "Anonymous").substr(0, 24);
        s.shipType = row.str("shipType");
        s.score = int(row.num("score"));
        out.push_back(std::move(s));
        if (out.size() == 10) break;
    }
    return true;
}

// Body for POST /api/high-scores, matching the browser's submission (main.js).
inline std::string high_score_body(const std::string &name, int score, const std::string &shipType,
                                   const std::string &shipColor, const std::string &shipPassphrase = {}) {
    json::Value v = json::Value::object();
    v.set("name", json::Value::string_value(name));
    v.set("score", json::Value::number_value(score));
    v.set("shipType", json::Value::string_value(shipType));
    v.set("shipColor", json::Value::string_value(shipColor));
    if (!shipPassphrase.empty()) v.set("shipPassphrase", json::Value::string_value(shipPassphrase));
    return json::dump(v);
}

// Passphrases are "adjective-noun-verb" from these lists (utils/helpers.js), so a
// controller can enter any generated one with three word wheels.
inline const std::vector<std::string> adjectives = {
    "red", "blue", "green", "brave", "swift", "mighty", "cosmic", "distant", "fierce",
    "ancient", "digital", "stellar", "solar", "lunar", "orbital", "quantum", "blazing",
    "radiant", "shining", "daring", "silent", "hidden", "secret", "phantom", "shadow",
    "glowing", "mystic", "noble", "royal", "elite", "frozen", "burning", "primal"};
inline const std::vector<std::string> nouns = {
    "star", "moon", "sun", "comet", "planet", "galaxy", "nebula", "voyager", "explorer",
    "hawk", "eagle", "phoenix", "dragon", "titan", "hunter", "arrow", "sword", "shield",
    "ranger", "sentinel", "viper", "falcon", "storm", "thunder", "lightning", "whisper",
    "shadow", "blade", "fang", "talon", "wing", "orbit", "pulse", "nova", "horizon"};
inline const std::vector<std::string> verbs = {
    "flying", "racing", "exploring", "jumping", "soaring", "diving", "orbiting", "gliding",
    "hunting", "seeking", "chasing", "wandering", "traveling", "roaming", "cruising",
    "drifting", "sailing", "blasting", "rushing", "warping", "speeding", "hovering",
    "launching", "protecting", "defending", "attacking", "evading", "escaping", "landing"};

inline std::string passphrase_path(const std::string &passphrase) {
    std::string out = "/api/ships/passphrase/";
    for (unsigned char c : passphrase) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += char(c);
        else { static const char *hex = "0123456789ABCDEF"; out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
}
