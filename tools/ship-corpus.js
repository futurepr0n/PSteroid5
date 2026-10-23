// Ship corpus analysis for the PSteroid5 editor design (read-only GET of the production gallery).
// Writes ps5/testdata/ship_corpus.json (geometry only: passphrases are stripped, since a
// passphrase is the key to a ship) and prints the statistics used in docs/ship-design-space.md.
// Usage: node tools/ship-corpus.js [https://asteroids.futurepr0n.com]
const fs = require('fs');
const path = require('path');
const https = require('https');
const server = process.argv[2] || 'https://asteroids.futurepr0n.com';

function get(url) {
  return new Promise((resolve, reject) => https.get(url, res => {
    let body = ''; res.on('data', c => { body += c; }); res.on('end', () => resolve({ status: res.statusCode, body }));
  }).on('error', reject));
}
const parse = v => (typeof v === 'string' ? (() => { try { return JSON.parse(v); } catch { return []; } })() : v) || [];
const pct = (n, d) => (d ? (100 * n / d).toFixed(0) + '%' : '-');
const median = a => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : 0; };
const quant = (a, q) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(q * s.length))] : 0; };

(async () => {
  const res = await get(server + '/api/ships/public');
  if (res.status !== 200) throw new Error('gallery: HTTP ' + res.status);
  const ships = JSON.parse(res.body).map(s => ({
    name: s.name, type: s.type, color: s.color,
    customLines: parse(s.customLines), thrusterPoints: parse(s.thrusterPoints), weaponPoints: parse(s.weaponPoints)
  }));
  const custom = ships.filter(s => s.type === 'custom' && s.customLines.length);
  const stats = custom.map(s => {
    const L = s.customLines.filter(l => [l.startX, l.startY, l.endX, l.endY].every(Number.isFinite));
    const pts = L.flatMap(l => [[l.startX, l.startY], [l.endX, l.endY]]);
    const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]);
    const key = (x, y, q) => `${Math.round(x / q)},${Math.round(y / q)}`;
    // endpoints shared with another segment within 3 units = drawn as connected outlines, not loose strokes
    const counts = new Map(); pts.forEach(([x, y]) => counts.set(key(x, y, 3), (counts.get(key(x, y, 3)) || 0) + 1));
    const joined = pts.filter(([x, y]) => counts.get(key(x, y, 3)) > 1).length;
    // mirror symmetry: a segment whose reflection (x -> -x) also exists within 3 units, either direction
    const near = (a, b) => Math.abs(a - b) <= 3;
    const mirrored = L.filter(l => L.some(m => (near(m.startX, -l.startX) && near(m.startY, l.startY) && near(m.endX, -l.endX) && near(m.endY, l.endY)) ||
      (near(m.endX, -l.startX) && near(m.endY, l.startY) && near(m.startX, -l.endX) && near(m.startY, l.endY)))).length;
    const onGrid = g => pts.filter(([x, y]) => near(Math.round(x / g) * g, x) && near(Math.round(y / g) * g, y)).length;
    const exactGrid = g => pts.filter(([x, y]) => Math.abs(x - Math.round(x / g) * g) < 0.01 && Math.abs(y - Math.round(y / g) * g) < 0.01).length;
    const lengths = L.map(l => Math.hypot(l.endX - l.startX, l.endY - l.startY));
    const colors = new Set(L.map(l => (l.color || s.color || 'white').toLowerCase()));
    return { name: s.name, lines: L.length, extentX: Math.max(...xs.map(Math.abs)), extentY: Math.max(...ys.map(Math.abs)),
      minY: Math.min(...ys), maxY: Math.max(...ys), joined: joined / pts.length, mirror: mirrored / L.length,
      grid10: onGrid(10) / pts.length, exact10: exactGrid(10) / pts.length, exact5: exactGrid(5) / pts.length,
      medianLen: median(lengths), shortLines: lengths.filter(x => x < 8).length / L.length,
      colors: colors.size, weapons: s.weaponPoints.length, thrusters: s.thrusterPoints.length,
      weaponY: s.weaponPoints.map(p => p.y), thrusterY: s.thrusterPoints.map(p => p.y) };
  });
  const col = k => stats.map(s => s[k]);
  const report = {
    server, fetched: new Date().toISOString(), ships: ships.length, custom: custom.length,
    typeCounts: ships.reduce((m, s) => (m[s.type] = (m[s.type] || 0) + 1, m), {}),
    lines: { median: median(col('lines')), p90: quant(col('lines'), 0.9), max: Math.max(...col('lines')) },
    extent: { medianX: median(col('extentX')), medianY: median(col('extentY')), maxX: Math.max(...col('extentX')), maxY: Math.max(...col('extentY')),
      over200: stats.filter(s => s.extentX > 200 || s.extentY > 200).length },
    joinedEndpoints: { median: median(col('joined')), shipsMostlyJoined: stats.filter(s => s.joined >= 0.8).length },
    mirror: { median: median(col('mirror')), shipsMostlySymmetric: stats.filter(s => s.mirror >= 0.8).length },
    grid: { within3of10: median(col('grid10')), exactly10: median(col('exact10')), exactly5: median(col('exact5')),
      shipsExactlyOn10: stats.filter(s => s.exact10 >= 0.95).length },
    segments: { medianLength: median(col('medianLen')), shipsWithManyTinySegments: stats.filter(s => s.shortLines >= 0.3).length },
    colors: { median: median(col('colors')), max: Math.max(...col('colors')), multiColour: stats.filter(s => s.colors > 1).length },
    weapons: { histogram: [0, 1, 2].map(n => stats.filter(s => s.weapons === n).length), frontY: median(col('weaponY').flat()) },
    thrusters: { histogram: [0, 1, 2, 3].map(n => stats.filter(s => s.thrusters === n).length), rearY: median(col('thrusterY').flat()) },
    perShip: stats.map(s => ({ name: s.name, lines: s.lines, mirror: +s.mirror.toFixed(2), joined: +s.joined.toFixed(2),
      exact10: +s.exact10.toFixed(2), extent: [Math.round(s.extentX), Math.round(s.extentY)], colors: s.colors, weapons: s.weapons, thrusters: s.thrusters }))
  };
  const out = path.resolve(__dirname, '..', 'ps5', 'testdata');
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'ship_corpus.json'), JSON.stringify({ server, fetched: report.fetched, note: 'public gallery geometry; passphrases stripped', ships }, null, 1) + '\n');
  fs.writeFileSync(path.join(out, 'ship_corpus_stats.json'), JSON.stringify(report, null, 1) + '\n');
  const { perShip, ...summary } = report;
  console.log(JSON.stringify(summary, null, 1));
})().catch(e => { console.error(e); process.exit(1); });
