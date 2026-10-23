# CSSteroids — native PS5 edition

Title **PPSA99305**, content ID `UP9000-PPSA99305_00-CSSTEROIDSPS5000`, version
01.000.000. A native C++ folder title of the CSSteroids-2 browser game built with
the workspace's PayloadSDK/Vulkan toolchain (see `../../PS5-PORTING-HANDBOOK.md`).
Plan: `../docs/plans/2026-10-23-ps5-native-port.md`.

## Status (milestones 1–2)

- Classic single-player: rotate, thrust, fire, screen wrap, asteroids split
  large → medium → small, `(4 - size) * 100` points, 3 lives, 3 s respawn
  invulnerability, `2 + level` asteroids per level, debris, game over, local best.
- Title screen fetches the top 10 from the existing server's `GET /api/high-scores`
  on a worker thread and shows ONLINE / OFFLINE / error status.
- Not yet: enemies, custom ships, score submission, passphrase ships,
  multiplayer, ship customizer, audio (later milestones).
- Deliberate difference: the browser loses a life from both a timer and the
  explosion update; this port deducts exactly one life per hit.

## Controls

| Input | Action |
| --- | --- |
| Left stick / D-pad left-right | Turn |
| R2, D-pad up or stick up | Thrust |
| CROSS, R1 or SQUARE | Fire (CROSS also starts) |
| OPTIONS | Pause; hold 1.25 s to exit |
| CIRCLE | Back to title |
| TRIANGLE (title) | Refresh server high scores |

## Server connection

Put the server's numeric address in `/data/cssteroids-ps5/server.txt`
(preferred) or `server.txt` beside `eboot.bin`, e.g. `192.168.1.50:6161`
(see `server.txt.example`). The server must listen on the LAN, not only
localhost. With no file the game runs offline. Saves: `/data/cssteroids-ps5/highscore.txt`.

## Build

```powershell
& .\Build-PS5.ps1 *> .\build-ps5.log
```

Runs host tests (simulation, JSON/HTTP with a loopback fixture), the native
build, SELF integrity inspection and generic folder validation of both the WSL
and Windows copies. Output: `dist/PPSA99305/` (git-ignored). Staging:
`~/work/psteroids5-app` in WSL. Run `ps5/Build-Icon.ps1` to regenerate the icon.

Smoke test: put a positive frame count in `smoke-test.txt` beside eboot.bin; the
title auto-plays that many frames and writes `smoke-test.ppm`.
