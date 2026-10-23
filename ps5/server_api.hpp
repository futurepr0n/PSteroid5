#pragma once
// CSSteroids server endpoints (routes/highscores.js, mounted at /api/high-scores).
#include "json_lite.hpp"
#include <string>
#include <vector>

namespace cssteroids::api {
constexpr const char *high_scores_path = "/api/high-scores";

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

// Body for POST /api/high-scores, matching the browser's submission.
inline std::string high_score_body(const std::string &name, int score, const std::string &shipType, const std::string &shipColor) {
    json::Value v = json::Value::object();
    v.set("name", json::Value::string_value(name));
    v.set("score", json::Value::number_value(score));
    v.set("shipType", json::Value::string_value(shipType));
    v.set("shipColor", json::Value::string_value(shipColor));
    return json::dump(v);
}
}
