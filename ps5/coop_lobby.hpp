#pragma once
// Two-player campaign rooms ("simple sessions" in CSSteroids-2 server.js): REST to create and
// list rooms, socket events to gather players and start. The game itself is a co-op world on
// the shared engine (mmo_world.hpp, sessionId = room id): 10 rounds, 3 shared lives.
#include "json_lite.hpp"
#include <string>
#include <vector>

namespace cssteroids::coop {
constexpr const char *rooms_path = "/api/simple-sessions";
constexpr const char *create_path = "/api/simple-sessions/create";
constexpr int room_size = 2; // what the browser creates

struct Room {
    std::string id, hostPlayerId, gameState;
    int currentPlayers = 0, maxPlayers = room_size;
};

inline bool read_room(const json::Value &v, Room &out) {
    if (!v.is_object() || v.str("id").empty()) return false;
    out.id = v.str("id").substr(0, 64);
    out.hostPlayerId = v.str("hostPlayerId");
    out.gameState = v.str("gameState", "waiting");
    out.currentPlayers = int(v.num("currentPlayers"));
    out.maxPlayers = int(v.num("maxPlayers", room_size));
    return true;
}

// POST body exactly as session-manager.js sends it; the host is our socket id.
inline std::string create_body(const std::string &hostSocketId) {
    json::Value v = json::Value::object();
    v.set("hostPlayerId", json::Value::string_value(hostSocketId));
    v.set("maxPlayers", json::Value::number_value(room_size));
    v.set("worldWidth", json::Value::number_value(2000));
    v.set("worldHeight", json::Value::number_value(1500));
    return json::dump(v);
}

// {success, session} from create / GET one.
inline bool parse_room_reply(const std::string &body, Room &out, std::string &error) {
    json::Value v;
    if (!json::parse(body, v) || !v.is_object()) { error = "unreadable reply"; return false; }
    if (!(v.get("success") && v.get("success")->b)) { error = v.str("error", "request refused"); return false; }
    const auto *session = v.get("session");
    if (!session || !read_room(*session, out)) { error = "reply without a room"; return false; }
    return true;
}

// {success, sessions:[...]} from the open-room list (waiting and not full).
inline bool parse_room_list(const std::string &body, std::vector<Room> &out) {
    json::Value v;
    if (!json::parse(body, v) || !v.is_object() || !v.get("sessions") || !v.get("sessions")->is_array()) return false;
    out.clear();
    for (const auto &item : v.get("sessions")->items) {
        Room r;
        if (read_room(item, r)) out.push_back(r);
        if (out.size() == 20) break;
    }
    return true;
}

// The short code players read out to each other: the part after "session_".
inline std::string room_code(const std::string &id) {
    const std::string prefix = "session_";
    return id.rfind(prefix, 0) == 0 ? id.substr(prefix.size()) : id;
}
inline std::string room_id_from_code(const std::string &code) {
    std::string lower;
    for (char c : code) if (c != ' ') lower += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return lower.rfind("session_", 0) == 0 ? lower : "session_" + lower;
}

// What the lobby screen needs from socket events.
struct LobbyState {
    std::string roomId, error;
    int playerCount = 0, starts = 0; // starts counts game-started (a replay in the same room)
    bool joined = false, started = false;
    // Returns true when the event belonged to the lobby.
    bool handle(const std::string &name, const json::Value &d) {
        if (name == "session-joined") { joined = true; roomId = d.str("sessionId", roomId); playerCount = int(d.num("playerCount", playerCount)); return true; }
        if (name == "player-joined") { playerCount++; return true; }
        if (name == "player-left" || name == "player-disconnected") { if (playerCount > 1) playerCount--; return true; }
        if (name == "game-started") { if (d.str("sessionId") == roomId) { started = true; starts++; } return true; }
        if (name == "session-error") { error = d.str("error", "room error"); return true; }
        return false;
    }
};
}
