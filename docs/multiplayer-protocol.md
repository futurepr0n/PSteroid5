# CSSteroids-2 multiplayer protocol (for the native client)

Extracted 2026-10-23 from CSSTeroids-2 at e3b17db (read-only). Paths are relative to
CSSTeroids-2. Server: socket.io 4.8.1 / Engine.IO v4.

## Connection
- `wss://asteroids.futurepr0n.com/socket.io/?EIO=4&transport=websocket` (nginx/openresty
  proxies 443 → 6161). No auth, no query params, CORS `*`, no origin check, no middleware.
- Engine.IO ping/pong defaults (25 s / 20 s): answer `2` with `3`.
- `socket.id` is the player id everywhere. The only version gate is `protocol: 4` in `mmo-join`;
  a mismatch gets `mmo-reload {protocol:4}`.

## Constants (public/js/shared/world-sim.js, server/world-engine.js)
TICK_MS 50 (20 Hz), FRAMES_PER_TICK 3, SHIP_RADIUS 15, VIEW_MARGIN 200, BULLET_SPEED 8,
BULLET_LIFE 1.8 s, ID_BLOCK 8, MAX_SYNC_TICKS 600, RESPAWN_TICKS 60, WARP_MAX_TICKS 100,
LAG_COMP_TICKS 4, ENEMY_SNAPSHOT_TICKS 2, ASTEROID_CHECK_TICKS 100, SCORES_TICKS 20.
Server time = `tick * 50` ms (engine counter). PROTOCOL_VERSION 4.

## Worlds
- **MMO**: a single persistent world `'mmo_world'`. Max players: env (default 5). Size: 2000x1500
  scaled by sqrt(max/5). Always read the size from `mmo-join-success.worldBounds`.
  `GET /api/mmo-world` returns `{success, world:{playerCount, maxPlayers, worldBounds, players, ...}}`.
- **Co-op**:
  1. `POST /api/simple-sessions/create {hostPlayerId, maxPlayers:2, worldWidth:2000, worldHeight:1500}`.
     `GET /api/simple-sessions` lists open sessions.
  2. Socket `join-simple-session` with the bare id string → `session-joined {sessionId, playerId, playerCount}`.
  3. `start-multiplayer-game` → `game-started {sessionId, ...}`. Everyone then sends `mmo-join` with `sessionId` = the lobby id.
  4. 10 rounds, 3 shared lives.

## Client → server
| Event | Payload | Notes |
|---|---|---|
| `mmo-join` | `{sessionId, playerData:{name≤20, shipData}, protocol:4, viewport:{w,h}}` | shipData is sanitized (helpers.js:72-122): ≤500 lines, thrusterPoints ≤3, weaponPoints ≤2, strings ≤32 |
| `mmo-time-sync` | `{sentAt}` | 5× at 100 ms spacing after join; reply `{sentAt, serverTime, tick}` |
| `mmo-viewport` | `{w,h}` | ≤1/s; clamped 320x240..2560x1440; drives the area of interest (±w/2+200, ±h/2+200) |
| `mmo-ready` | – | Ends warp. Send after the clock offset is known, ≥1 `mmo-state` has arrived and ≥1.5 s have passed |
| `mmo-player-update` | `[round(x), round(y), round2(angle), round2(vx), round2(vy), thrust?1:0]` | Every 50 ms. Skip if unchanged, but heartbeat at least 1 Hz. Server clamps to bounds, ≤22.5 px/tick and |v|≤5, with no correction sent back |
| `mmo-shoot` | `{x,y,angle,vx,vy,tick:floor(serverTick)}` | ≤5 per 1 s and ≥150 ms apart, otherwise silently dropped. Origin >100 px from the server pose → the server pose is used. Lag rewind 0..4 ticks |
| `mmo-player-died` | `{type:'enemyBullet'|'asteroid'|'enemy', objectId, x, y}` | Plausibility check: within objR+15+60. If implausible the server replies `mmo-respawn` (un-kill) |
| `mmo-ship-collision` | `{type:'asteroid'|'enemy', objectId, x, y}` | The server destroys the object and kills the player; no score |
| `mmo-bullet-hit` | `{type, objectId, x, y}` | Hint only; optional |
| `mmo-ship-data` | `{shipData}` | Relayed; also changes your server-side weapon points |
| `mmo-leave` | – | |

Angles are in game convention (0 = up, x += sin, y -= cos). Velocities are px per 60 Hz frame.

## Server → client
| Event | Payload |
|---|---|
| `mmo-join-success` | `{player:{id, idx, ...}, sessionId, mode, round, maxRounds, lives, worldBounds:{width,height}, asteroids:[full], enemies:[full], players:[{id,name,x,y,angle,score,dead,shipData}] (incl. self), tick, serverTime}` |
| `mmo-join-error` | `{error}` |
| `mmo-state` | `{t, p:[[id,x,y,a,dead,thrust]], e:[[id,x,y,a_math]] (even ticks), g?:[ids out of view]}`. Delta-coded per viewer; never contains yourself. Interpolate 100 ms behind (render at serverTick−2) using the SnapshotBuffer hold-last rule |
| `mmo-asteroid-spawn` | `{id,x,y,vx,vy,size,radius,rotation,rotationSpeed,seed,spawnTick}` |
| `mmo-asteroid-destroyed` | `[id, tick, killerIdx]`. Advance the parent to `tick`, then `splitAsteroid` |
| `mmo-asteroid-check` | `{tick, a:[[id,x,y]]}`. Correct records that are >1.5 px off |
| `mmo-enemy-spawn` | `[id,x,y,vx,vy]` |
| `mmo-enemy-destroyed` | `[id, killerIdx]` |
| `mmo-enemy-shoot` | `{enemyId,x,y,angle_math}`. Bullet at angle+π/2, with no inherited velocity |
| `mmo-bullet-fired` | `{bullet:{x,y,angle,vx,vy}, playerId}`. Visual replay using the shooter's weaponPoints |
| `mmo-player-joined` / `-left` | `{playerId, playerName, x, y, angle, shipData}` / `{playerId, playerName}` |
| `mmo-player-died` | `{playerId,x,y}` |
| `mmo-respawn` | `{x,y,angle}` (you). Zero velocity, 3 s client-side invulnerability |
| `mmo-player-respawned` | `{playerId,x,y}` |
| `mmo-ship-data` | `{playerId, shipData}` |
| `mmo-scores` | `{players:[[id,name,score]], left?:[ids], full?:true}`. `full` replaces, `left` deletes, entries upsert, sort descending |
| co-op | `mmo-round-start {round,maxRounds}`, `mmo-round-cleared`, `mmo-lives {lives,lostBy}`, `mmo-game-over` / `mmo-game-complete {round,teamScore}` |

## Deterministic asteroids (must be bit-exact)
- `stepAsteroid` once per tick: pos += v·3 and rot += rs·3, then clamp to the radius from each edge and reflect the velocity.
- `splitAsteroid(parent, tick)`:
  - children for i = 0, 1 with side = −1, +1;
  - id `id - id%8 + 2*(id%8) + 1 + i`;
  - seed `(imul(seed ^ (i+1), 2654435761) + i*40503) >>> 0`;
  - spread `0.5 + childSeed(seed,2)/2^32*0.7`; angle = heading + side·spread;
  - position parent + dir·childR·0.5; velocity dir·childSpeed;
  - rotation speed `parent.rs * -side * 1.5`; spawnTick = tick.
- Shape from mulberry32(seed): vertices = floor(r·3)+6, jag = r·0.2+0.1, offsets[i] = R·(1−jag+r·jag).

## Authority
| Thing | Who decides |
|---|---|
| Own pose | Client-predicted, server-clamped |
| Kills and scores | Server bullet sweep with lag rewind |
| Remote bullets | Visual only |
| Destruction | Server events; the client hides the target optimistically and restores it after 300 ms + RTT without confirmation |
| Own death | Client-detected, server-checked |
| Respawn, lives | Server |

## Bullets (createShotBullets)
- No weapon points: one bullet at nose+15, speed 8, life 1.8 s.
- Two weapon points: rotated origins, speed 6, life 1.26 s.
- v = dir·speed + shipV·0.5. Per frame: pos += v, speed *= 0.99, then v is recomputed from the angle.

## Enemies
- Server-simulated and snapshot-driven (`e` entries, math angle). Health 1, radius 15, 500 points.
- They fire with a 2-3 s cooldown within 450 px and 0.15 rad. The server keeps their bullets only for death plausibility checks.

## Leave and idle
`mmo-leave` or a disconnect removes the player. There is no idle timeout; Engine.IO ping/pong is the only liveness check.
