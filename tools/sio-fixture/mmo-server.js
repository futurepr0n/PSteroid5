// Dev-only local MMO/co-op server for PSteroids5 tests: the REAL CSSteroids-2 world engine
// (required read-only from ../../../CSSTeroids-2/server/world-engine.js) behind handlers copied
// from CSSTeroids-2 server.js at e3b17db: simple-session REST (84-161), join-simple-session
// (472-538), start-multiplayer-game (406-442) and the MMO events (545-710). No Express/MySQL.
// Prints the port. Usage: node mmo-server.js [port] [host]
const path = require('path');
const { createServer } = require('http');
const { Server } = require('socket.io');
const ref = path.resolve(__dirname, '..', '..', '..', 'CSSTeroids-2');
const {
  COOP_CONFIG, createWorld: createEngineWorld, startWorldLoop, stopWorldLoop, destroyMMOAsteroid, removeMMOEnemy,
  sendFullScores, sanitizeViewport, joinPlayer, markPlayerReady, activePlayer, asteroidSnapshot, handleShoot,
  handlePlayerUpdate, killPlayer, contactPlausible, countEvent, eventStatsLine
} = require(path.join(ref, 'server', 'world-engine'));
const { TICK_MS } = require(path.join(ref, 'public', 'js', 'shared', 'world-sim'));
const { sanitizeMMOShipData } = require(path.join(ref, 'utils', 'helpers'));

const simpleSessions = new Map();
function json(res, status, body) { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(body)); }
const http = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'POST' && url.pathname === '/api/simple-sessions/create') {
    let raw = ''; req.on('data', c => { raw += c; });
    req.on('end', () => {
      let body = {}; try { body = JSON.parse(raw || '{}'); } catch { /* as express: empty */ }
      const sessionId = body.sessionId || 'session_' + Math.random().toString(36).substring(2, 10);
      const session = { id: sessionId, hostPlayerId: body.hostPlayerId || 'anonymous_' + Math.random().toString(36).substring(2, 8),
        maxPlayers: body.maxPlayers || 3, currentPlayers: 0, gameState: 'waiting', worldWidth: body.worldWidth || 2000,
        worldHeight: body.worldHeight || 1500, createdAt: new Date().toISOString() };
      simpleSessions.set(sessionId, session);
      json(res, 200, { success: true, session });
    });
    return;
  }
  if (req.method === 'GET' && url.pathname === '/api/simple-sessions')
    return json(res, 200, { success: true, sessions: Array.from(simpleSessions.values()).filter(x => x.gameState === 'waiting' && x.currentPlayers < x.maxPlayers) });
  const one = url.pathname.match(/^\/api\/simple-sessions\/([^/]+)$/);
  if (req.method === 'GET' && one) {
    const session = simpleSessions.get(decodeURIComponent(one[1]));
    return session ? json(res, 200, { success: true, session }) : json(res, 404, { success: false, error: 'Session not found' });
  }
  if (!url.pathname.startsWith('/socket.io/')) json(res, 404, { message: 'not found' });
});
const io = new Server(http, { cors: { origin: '*', methods: ['GET', 'POST'] } });
const MMO_WORLD_ID = 'mmo_world';
const worlds = new Map();
function createWorld(id, mode, options = {}) {
  const world = createEngineWorld(id, mode, options, (target, ev, data) => io.to(target.socketId || target).emit(ev, data));
  startWorldLoop(world);
  worlds.set(id, world);
  return world;
}
function destroyWorld(world) { stopWorldLoop(world); worlds.delete(world.id); }
function getOrCreateCoopWorld(sessionId) {
  let world = worlds.get(sessionId);
  if (!world) {
    const session = simpleSessions.get(sessionId);
    if (!session) return null;
    world = createWorld(sessionId, 'coop', { maxPlayers: session.maxPlayers, width: session.worldWidth, height: session.worldHeight });
  }
  return world;
}
function removePlayerFromWorld(socket, reason) {
  const world = worlds.get(socket.worldId);
  if (!world) return;
  const player = world.players.get(socket.id);
  world.players.delete(socket.id);
  io.to(world.room).emit('mmo-player-left', { playerId: socket.id, playerName: player?.name });
  socket.leave(world.room);
  socket.worldId = null;
  console.error(`left ${player?.name} (${reason})`);
  if (world.mode === 'coop' && world.players.size === 0) destroyWorld(world);
}
const mmoWorld = createWorld(MMO_WORLD_ID, 'mmo');
const fresh = new Map(); // socket id -> { ticks: Set of ticks with an applied update, first }

io.on('connection', socket => {
  socket.on('disconnect', () => {
    removePlayerFromWorld(socket, 'disconnected');
    if (socket.currentSession) {
      socket.to(socket.currentSession).emit('player-disconnected', { playerId: socket.id, timestamp: Date.now() });
      const session = simpleSessions.get(socket.currentSession);
      const left = (io.sockets.adapter.rooms.get(socket.currentSession)?.size || 1) - 1;
      if (session) { session.currentPlayers = Math.max(0, left); if (left <= 0) simpleSessions.delete(socket.currentSession); }
    }
  });
  socket.on('join-simple-session', sessionId => {
    if (socket.currentSession === sessionId) {
      socket.emit('session-joined', { sessionId, playerId: socket.id, playerCount: io.sockets.adapter.rooms.get(sessionId)?.size || 0 });
      return;
    }
    if (socket.currentSession) socket.leave(socket.currentSession);
    socket.join(sessionId);
    socket.currentSession = sessionId;
    const roomSize = io.sockets.adapter.rooms.get(sessionId)?.size || 0;
    if (!simpleSessions.has(sessionId)) simpleSessions.set(sessionId, { id: sessionId, hostPlayerId: socket.id, maxPlayers: 2, currentPlayers: roomSize,
      gameState: 'waiting', worldWidth: 2000, worldHeight: 1500, createdAt: new Date().toISOString() });
    else simpleSessions.get(sessionId).currentPlayers = roomSize;
    socket.to(sessionId).emit('player-joined', { playerId: socket.id, timestamp: Date.now() });
    socket.emit('session-joined', { sessionId, playerId: socket.id, playerCount: roomSize });
  });
  socket.on('leave-session', () => {
    if (!socket.currentSession) return;
    socket.to(socket.currentSession).emit('player-left', { playerId: socket.id, timestamp: Date.now() });
    socket.leave(socket.currentSession);
    socket.currentSession = null;
  });
  socket.on('start-multiplayer-game', () => {
    const sessionId = socket.currentSession;
    if (!sessionId) return;
    const session = simpleSessions.get(sessionId);
    if (session) session.gameState = 'playing';
    const existing = worlds.get(sessionId);
    if (existing && existing.mode === 'coop') destroyWorld(existing);
    getOrCreateCoopWorld(sessionId);
    io.to(sessionId).emit('game-started', { sessionId, worldWidth: 2000, worldHeight: 1500, startedBy: socket.id, timestamp: Date.now() });
  });
  socket.on('mmo-join', data => {
    if (!data || typeof data !== 'object') return;
    const worldId = data.sessionId && data.sessionId !== MMO_WORLD_ID ? data.sessionId : MMO_WORLD_ID;
    const world = worldId === MMO_WORLD_ID ? mmoWorld : getOrCreateCoopWorld(worldId);
    if (!world) { socket.emit('mmo-join-error', { error: 'World not available' }); return; }
    if (socket.worldId === world.id) world.players.delete(socket.id);
    else if (socket.worldId) removePlayerFromWorld(socket, 'switched worlds');
    if (world.players.size >= world.maxPlayers) { socket.emit('mmo-join-error', { error: `World full (max ${world.maxPlayers} players)` }); return; }
    const player = joinPlayer(world, socket.id, data, (ev, payload) => socket.emit(ev, payload));
    if (!player) return;
    socket.join(world.room);
    socket.worldId = world.id;
    console.error(`joined ${player.name}`);
    socket.emit('mmo-join-success', {
      player, sessionId: world.id, mode: world.mode, round: world.round, maxRounds: COOP_CONFIG.maxRounds, lives: world.lives,
      worldBounds: { width: world.worldWidth, height: world.worldHeight }, asteroids: asteroidSnapshot(world),
      enemies: Array.from(world.enemies.values()),
      players: Array.from(world.players.values()).map(p => ({ id: p.id, name: p.name, x: p.x, y: p.y, angle: p.angle, score: p.score, dead: p.dead, shipData: p.shipData })),
      tick: world.tick, serverTime: world.tick * TICK_MS
    });
    socket.to(world.room).emit('mmo-player-joined', { playerId: socket.id, playerName: player.name, x: player.x, y: player.y, angle: player.angle, shipData: player.shipData });
    sendFullScores(world, socket.id);
  });
  socket.on('mmo-time-sync', data => {
    const world = worlds.get(socket.worldId);
    if (!world || !Number.isFinite(data?.sentAt)) return;
    socket.emit('mmo-time-sync', { sentAt: data.sentAt, serverTime: world.tick * TICK_MS, tick: world.tick });
  });
  socket.on('mmo-viewport', data => { const p = worlds.get(socket.worldId)?.players.get(socket.id); if (p) p.viewport = sanitizeViewport(data); });
  socket.on('mmo-ready', () => { const w = worlds.get(socket.worldId); if (w) markPlayerReady(w, socket.id); });
  socket.on('mmo-leave', () => removePlayerFromWorld(socket, 'left'));
  // JITTER_MS=n delays each update by a random 0..n ms (one-way network jitter). Fresh-tick
  // accounting: how many server ticks got at least one new position from this player.
  socket.on('mmo-player-update', data => {
    const apply = () => {
      const w = worlds.get(socket.worldId); if (!w) return;
      handlePlayerUpdate(w, socket.id, data);
      const f = fresh.get(socket.id) || { ticks: new Set(), first: w.tick };
      f.ticks.add(w.tick); fresh.set(socket.id, f);
    };
    const jitter = Number(process.env.JITTER_MS || 0);
    if (jitter > 0) setTimeout(apply, Math.random() * jitter); else apply();
  });
  socket.on('mmo-shoot', data => { const w = worlds.get(socket.worldId); if (w) handleShoot(w, socket.id, data); });
  socket.on('mmo-player-died', data => {
    const world = worlds.get(socket.worldId); if (!world) return;
    const player = activePlayer(world, socket.id); if (!player) return;
    if (!contactPlausible(world, player, data)) { countEvent(world, 'deathRejected'); socket.emit('mmo-respawn', { x: player.x, y: player.y, angle: player.angle }); return; }
    killPlayer(world, player);
  });
  socket.on('mmo-ship-collision', data => {
    const world = worlds.get(socket.worldId); if (!world) return;
    const player = activePlayer(world, socket.id);
    if (!player || data?.type === 'enemyBullet' || !contactPlausible(world, player, data)) { countEvent(world, 'collisionRejected'); return; }
    if (data.type === 'asteroid') destroyMMOAsteroid(world, world.asteroids.get(data.objectId), socket.id);
    else removeMMOEnemy(world, world.enemies.get(data.objectId), socket.id);
    killPlayer(world, player);
  });
  socket.on('mmo-bullet-hit', () => { const w = worlds.get(socket.worldId); if (w && activePlayer(w, socket.id)) countEvent(w, 'hitPredicted'); });
  socket.on('mmo-ship-data', data => {
    const world = worlds.get(socket.worldId); if (!world) return;
    const player = world.players.get(socket.id); if (!player) return;
    player.shipData = sanitizeMMOShipData(data?.shipData);
    socket.to(world.room).emit('mmo-ship-data', { playerId: socket.id, shipData: player.shipData });
  });
  // Test hooks (not in the real server): world facts and rejection counters for assertions.
  socket.on('fixture-reset-fresh', () => fresh.delete(socket.id));
  socket.on('fixture-stats', () => {
    const p = mmoWorld.players.get(socket.id);
    socket.emit('fixture-stats', { tick: mmoWorld.tick, stats: typeof eventStatsLine === 'function' ? eventStatsLine(mmoWorld) : '',
      player: p ? { x: p.x, y: p.y, score: p.score, warping: p.warping, dead: p.dead, shipData: p.shipData } : null,
      fresh: (() => { const f = fresh.get(socket.id); if (!f) return null; const span = mmoWorld.tick - f.first;
        return { ticksWithUpdate: f.ticks.size, ticksElapsed: span }; })(),
      asteroids: asteroidSnapshot(mmoWorld).map(a => ({ id: a.id, x: a.x, y: a.y, spawnTick: a.spawnTick })) });
  });
});
http.listen(Number(process.argv[2] || 0), process.argv[3] || '127.0.0.1', () => console.log(http.address().port));
