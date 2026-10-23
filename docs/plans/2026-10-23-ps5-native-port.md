# CSSteroids-2 native PS5 port — implementation plan

Date: 2026-10-23. Workspace rules: `L:\Development\PS5\AGENTS.md`,
`PS5-PORTING-HANDBOOK.md`, `PS5-BUILD-GUIDE.md`.

## Goal

A native C++ PS5 folder title of CSSteroids-2 that keeps every gameplay feature
and talks to the **existing, unchanged** Node/Express/socket.io server and MySQL
database: ship loading by passphrase, the public ship list, high scores, and
co-op/MMO multiplayer shared with browser players.

## Identity

| Field | Value |
| --- | --- |
| Title ID | `PPSA99305` (unused: 99303 = KB-RPCS3, 99304 = BattleStar) |
| Content ID | `UP9000-PPSA99305_00-CSSTEROIDSPS5000` |
| Title name | CSSteroids |
| Source | `PSteroids5/` (own git repo; `CSSTeroids-2` is read-only reference) |
| WSL staging | `~/work/psteroids5-app/` (separate from BattleStar) |
| Windows output | `PSteroids5/dist/PPSA99305/` (git-ignored) |
| Entry point | `PSteroids5/Build-PS5.ps1` |

## Architecture

- `ps5/asteroids_core.hpp`: portable, deterministic simulation translated from
  `public/js/entities/*.js`, `core/game.js` and `shared/world-sim.js`. Fixed
  60 Hz steps, since the JS physics is per-frame. No platform includes. Tested with host g++.
- `ps5/json_lite.hpp`: a small JSON value/parser/serializer for API payloads.
- `ps5/net_http.hpp`: blocking HTTP/1.1 client over BSD sockets (numeric IPv4,
  send/receive timeouts, `Content-Length` and chunked bodies), plus an `Online`
  worker thread so the render loop never blocks. Builds on host Linux and PS5.
- `ps5/cssteroids.cpp`: the renderer/input/lifecycle sample, on the BattleStar shell
  (HUI DrawList at 1920x1080 logical).
- Server address: first line of `/data/cssteroids-ps5/server.txt`, else
  `/app0/server.txt`, e.g. `192.168.1.50:6161`. No address → offline mode.

Server API (verified in `server.js` and the files in `routes/`): `GET/POST /api/high-scores`,
`GET /api/ships/public`, `GET /api/ships/passphrase/:p`, `POST /api/ships`,
`/api/multiplayer/*`; socket.io v4 on the same port, plain HTTP.

## Milestones

1. **Native launch + vertical slice (this session).** Separate stage/ID/icon,
   build + SELF integrity + package validation. Playable classic mode: rotate,
   thrust, fire, screen wrap, asteroids split 3→2→1, score `(4-size)*100`,
   3 lives with 3 s invulnerability, level N spawns `2+N` large asteroids away
   from the ship, debris, game over, local best score. Default/triangle/diamond ships.
2. **Network spike (this session).** HTTP client + JSON parser, host-tested
   against a loopback fixture server. The title screen shows the server's top-10
   `/api/high-scores` (or a clear offline/error message). Console verification
   needs the PS5 on and the server reachable; this is the go/no-go for online play.
3. Score submission (`POST /api/high-scores`), ship by passphrase (on-screen
   keyboard via `libSceImeDialog` or a character picker) and public ship browser;
   custom-line ships with thruster/weapon points and line-collision.
4. WebSocket client + Engine.IO v4/socket.io framing
   (`/socket.io/?EIO=4&transport=websocket`); host-tested against the real
   server; then the MMO/co-op events (`mmo-join`, `mmo-player-update`,
   `mmo-shoot`, snapshot interpolation as in `WorldSim.SnapshotBuffer`).
5. Remaining single-player features: enemies, enemy fire, menus, settings.
6. Controller-driven ship customizer (grid cursor, colours, thruster/weapon
   points), saving to the database.
7. Audio, polish, measured console performance.

## Verification per milestone

Host tests (`ps5/test_*.cpp`) → native build → `ps5-native-tool self --inspect`
must report `integrity: valid` → `validate_package.py --generic` on WSL and
Windows copies → `validation.json` hashes. The package stays "not console-tested"
until the user reports a hardware result. No FTP unless requested.
