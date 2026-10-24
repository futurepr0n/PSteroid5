# PSteroid5 — native PS5 port of CSSteroids

Title **PPSA77305**, content ID `UP9000-PPSA77305_00-PSTEROID5PS50000`, version
01.000.000. A native C++ folder title of the CSSteroids-2 browser game built with
the workspace's PayloadSDK/Vulkan toolchain (see `../../PS5-PORTING-HANDBOOK.md`).
Plan: `../docs/plans/2026-10-23-ps5-native-port.md`.

## Status (milestones 1–3)

- Classic single-player: rotate, thrust, fire, screen wrap, asteroids split
  large → medium → small, `(4 - size) * 100` points, 3 lives, 3 s respawn
  invulnerability, `2 + level` asteroids per level, debris, game over, local best.
- **Server (unchanged CSSteroids-2 backend), plain HTTP or HTTPS:** top scores on the
  title screen; **Hangar** browses `GET /api/ships/public` with a rotating preview;
  a passphrase is entered on three word wheels (the server's adjective-noun-verb
  lists) and loaded via `GET /api/ships/passphrase/:p`; at game over TRIANGLE posts
  the score (`POST /api/high-scores`, same fields as the browser), only on request.
- Custom ships: coloured design lines, per-line collision, weapon points
  (several = 0.75x speed, 0.7x life, as the browser), thruster points or the
  lowest-points fallback. The chosen ship is saved locally and reloads offline.
- HTTPS: vendored mbedTLS 3.6.2 (`third_party/mbedtls`, unmodified) with the
  ISRG Root X1/X2 certificates; untrusted certificates are refused (tested).
  DNS uses the console resolver (`libSceNet`), then `getaddrinfo`.
- **Ship editor** (title > SHIP EDITOR): controller-first, with optional touchpad (slide to
  move the cursor, press-and-hold to drag). LINE/MOVE/ERASE/PAINT/THRUSTER/WEAPON tools,
  mirror on by default (R3), 5-unit grid with a fine step (L3), presets plus a custom colour,
  undo/redo, a live in-game-size preview, a collision outline and a test flight. Saves to
  the server; ships made or loaded by passphrase update in place, gallery ships save as
  copies. Evidence: `../docs/ship-editor-report.md`.
- Multiplayer: MMO (drop-in shared world) and MULTIPLAYER (two-pilot co-op rooms).
- Not yet: enemies in single-player, audio.
- Deliberate difference: the browser loses a life from both a timer and the
  explosion update; this port deducts exactly one life per hit.

## Controls

| Screen | Input | Action |
| --- | --- | --- |
| Title | D-pad up/down, CROSS | Play / Hangar / Pilot name / Refresh scores |
| Game | Left stick / D-pad left-right | Turn |
| Game | R2, D-pad up or stick up | Thrust |
| Game | CROSS, R1 or SQUARE | Fire |
| Game | OPTIONS / CIRCLE | Pause / title |
| Game over | TRIANGLE / CROSS | Submit score / play again |
| Hangar | Left/right, CROSS | Browse gallery, fly the shown ship |
| Hangar | SQUARE | Switch gallery ↔ passphrase wheels (left/right word, up/down change, CROSS load) |
| Pilot name | D-pad, CROSS, SQUARE, OPTIONS | Pick, add, delete, save |
| Any | Hold OPTIONS 1.25 s | Exit (not on the name screen) |

## Server connection

The package includes `server.txt` set to `https://asteroids.futurepr0n.com`.
`/data/cssteroids-ps5/server.txt` overrides it without repackaging; see
`server.txt.example` for a LAN development server (`http://IP:6161`) and for
adding a numeric IP after the URL if console DNS fails or the router cannot
hairpin to its own public address. Local files: `/data/cssteroids-ps5/`
(`highscore.txt`, `ship.json`, `pilot.txt`).

The console's clock must be roughly correct: certificate dates are checked.

## Build

```powershell
& .\Build-PS5.ps1 *> .\build-ps5.log
```

Runs host tests (simulation, ship parsing, JSON/HTTP/TLS against loopback fixtures; `-LiveServer https://asteroids.futurepr0n.com` adds read-only GETs to that server), the native
build, SELF integrity inspection and generic folder validation of both the WSL
and Windows copies. Output: `dist/PPSA77305/` (git-ignored). Staging:
`~/work/psteroids5-app` in WSL. Run `ps5/Build-Icon.ps1` to regenerate the icon.

Smoke test: put a positive frame count in `smoke-test.txt` beside eboot.bin; the
title auto-plays that many frames and writes `smoke-test.ppm`.
