// Writes ps5/testdata/world_vectors.json from the real CSSteroids-2 shared simulation
// (read-only require of ../CSSTeroids-2/public/js/shared/world-sim.js and asteroid.js),
// so the C++ port can be checked bit-for-bit. Run: node tools/gen-world-vectors.js
const fs = require('fs');
const path = require('path');
const ref = path.resolve(__dirname, '..', '..', 'CSSTeroids-2', 'public', 'js');
const WorldSim = require(path.join(ref, 'shared', 'world-sim.js'));
const asteroidSrc = fs.readFileSync(path.join(ref, 'entities', 'asteroid.js'), 'utf8');
const Asteroid = new Function(asteroidSrc + '; return Asteroid;')();

function shape(seed, radius) {
  const rand = Asteroid.seededRandom(seed);
  const vertices = Math.floor(rand() * 3) + 6;
  const jag = rand() * 0.2 + 0.1;
  const offsets = [];
  for (let i = 0; i < vertices; i++) offsets.push(radius * (1 - jag + rand() * jag));
  return { vertices, offsets };
}

const bounds = { width: 2400, height: 1800 };
const out = { bounds, asteroids: [], bullets: [], clock: [], scores: [] };

// Asteroids: step for a while (wall bounces included), then split down the tree.
const seeds = [1, 7, 123456789, 4294967295, 2654435761, 99];
seeds.forEach((seed, n) => {
  const a = { id: 8 * (n + 1), x: 60 + n * 397, y: 1700 - n * 211, vx: Math.cos(n * 1.7) * 1.1, vy: Math.sin(n * 1.7) * 0.9,
              size: 3, radius: 40, rotation: n * 0.3, rotationSpeed: 0.013 * (n % 2 ? -1 : 1), seed, spawnTick: 1000 };
  const start = { ...a };
  WorldSim.advanceAsteroid(a, 250, bounds);
  const children = WorldSim.splitAsteroid(a, 1250);
  const grand = children.length ? WorldSim.splitAsteroid(WorldSim.advanceAsteroid({ ...children[1] }, 40, bounds), 1290) : [];
  out.asteroids.push({ start, stepped: { ...a }, children, grandchildren: grand,
                       shape: shape(seed, 40), childShape: children.map(c => shape(c.seed, c.radius)) });
});

// Bullets: default nose round and two weapon points, then 30 frames.
for (const weapons of [[], [{ x: -40, y: -20 }, { x: 40, y: -20 }]]) {
  const shot = { x: 500, y: 400, angle: 0.7, vx: 1.25, vy: -2.5 };
  const created = WorldSim.createShotBullets(shot, weapons);
  const stepped = created.map(b => {
    const c = { ...b };
    for (let f = 0; f < 30; f++) { c.x += c.vx; c.y += c.vy; c.speed *= 0.99; c.vx = Math.sin(c.angle) * c.speed; c.vy = -Math.cos(c.angle) * c.speed; }
    return c;
  });
  out.bullets.push({ shot, weapons, created, stepped });
}

out.clock = { samples: [{ sentAt: 1000, receivedAt: 1080, serverTime: 50000 }, { sentAt: 1100, receivedAt: 1140, serverTime: 50120 },
                        { sentAt: 1200, receivedAt: 1400, serverTime: 50300 }, { sentAt: 1300, receivedAt: 1330, serverTime: 50318 },
                        { sentAt: 1400, receivedAt: 1450, serverTime: 50420 }] };
out.clock.result = WorldSim.estimateClockOffset(out.clock.samples);

let board = [];
const msgs = [{ full: true, players: [['a', 'Ann', 300], ['b', 'Bo', 900]] }, { players: [['c', 'Cy', 500], ['a', 'Ann', 1200]] }, { left: ['b'] }];
for (const m of msgs) { board = WorldSim.mergeScores(board, m); out.scores.push({ msg: m, board }); }

fs.mkdirSync(path.resolve(__dirname, '..', 'ps5', 'testdata'), { recursive: true });
fs.writeFileSync(path.resolve(__dirname, '..', 'ps5', 'testdata', 'world_vectors.json'), JSON.stringify(out, (k, v) => v, 1) + '\n');
console.log('wrote world_vectors.json:', out.asteroids.length, 'asteroid trees');
