// Judges the PS5 ship editor with code it did not write: CSSteroids-2's server validators
// (utils/helpers.js) and the browser's own ship.js, loaded read-only from ../CSSTeroids-2.
// Input: the JSON written by ps5/ship_evidence.cpp. Usage: node tools/ship-evidence.js <evidence.json>
const fs = require('fs');
const path = require('path');
const ref = path.resolve(__dirname, '..', '..', 'CSSTeroids-2');
const { validateShipData, sanitizeMMOShipData } = require(path.join(ref, 'utils', 'helpers.js'));

// ship.js expects a browser: give it just enough (no DOM is touched by draw/collision).
global.window = {};
const quiet = console.log; console.log = () => {}; // ship.js logs on every construction
global.localStorage = { getItem: () => null, setItem() {} };
const Ship = new Function(fs.readFileSync(path.join(ref, 'public', 'js', 'entities', 'ship.js'), 'utf8') + '; return Ship;')();

// A 2D context that records what ship.js strokes and fills, in world coordinates.
function recorder() {
  let m = [1, 0, 0, 1, 0, 0];
  const stack = [];
  let path = [], start = null, cur = null;
  const segments = [], fills = [], dots = [];
  const tx = (x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];
  const ctx = {
    strokeStyle: '', fillStyle: '', lineWidth: 1, globalAlpha: 1, font: '', textAlign: '',
    save() { stack.push(m.slice()); }, restore() { m = stack.pop() || [1, 0, 0, 1, 0, 0]; },
    translate(x, y) { m = [m[0], m[1], m[2], m[3], m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]]; },
    rotate(a) { const c = Math.cos(a), s = Math.sin(a); m = [m[0] * c + m[2] * s, m[1] * c + m[3] * s, m[0] * -s + m[2] * c, m[1] * -s + m[3] * c, m[4], m[5]]; },
    beginPath() { path = []; start = cur = null; },
    moveTo(x, y) { start = cur = tx(x, y); path.push({ move: cur }); },
    lineTo(x, y) { const p = tx(x, y); if (cur) path.push({ seg: [cur, p] }); cur = p; },
    closePath() { if (cur && start && (cur[0] !== start[0] || cur[1] !== start[1])) path.push({ seg: [cur, start] }); cur = start; },
    arc(x, y, r) { const c = tx(x, y); path.push({ arc: [c[0], c[1], r * Math.hypot(m[0], m[1])] }); }, fillText() {}, fillRect() {}, strokeRect() {},
    createLinearGradient() { return { addColorStop() {} }; }, createRadialGradient() { return { addColorStop() {} }; },
    stroke() { path.forEach(p => p.seg && segments.push({ a: p.seg[0], b: p.seg[1], color: String(ctx.strokeStyle).toLowerCase() })); },
    fill() {
      const first = path.find(p => p.move);
      if (first && path.some(p => p.seg)) fills.push(first.move);
      path.forEach(p => p.arc && dots.push({ c: p.arc, color: String(ctx.fillStyle).toLowerCase() }));
    }
  };
  return { ctx, segments, fills, dots };
}

// What the server stores and hands back: JSON columns round-trip through strings.
function storedShip(body) {
  const b = JSON.parse(body);
  const rt = v => JSON.parse(JSON.stringify(v || []));
  return { name: b.name, type: b.type, color: b.color, customLines: rt(b.customLines), thrusterPoints: rt(b.thrusterPoints), weaponPoints: rt(b.weaponPoints) };
}

const CSS = { white: '#ffffff', red: '#ff0000', cyan: '#00ffff', lime: '#00ff00', yellow: '#ffff00', orange: '#ffa500', magenta: '#ff00ff', blue: '#0000ff', green: '#008000' };
const normColor = c => { c = String(c || '').toLowerCase(); return CSS[c] || c; };
const near = (a, b, eps) => Math.abs(a - b) <= eps;

function judge(entry) {
  const problems = [];
  const body = JSON.parse(entry.body);
  const v = validateShipData(body);
  if (!v.valid) problems.push('server rejects: ' + v.message);
  const mmo = sanitizeMMOShipData(body);
  if (!mmo) problems.push('MMO sanitizer drops the ship');
  else if (mmo.customLines.length !== body.customLines.length || mmo.weaponPoints.length !== body.weaponPoints.length || mmo.thrusterPoints.length !== body.thrusterPoints.length)
    problems.push('MMO sanitizer trims the ship');

  const stored = storedShip(entry.body);
  let segChecks = 0, hitChecks = 0, hitSkipped = 0, flameChecks = 0, dotChecks = 0;
  for (const pose of entry.ps5.poses) {
    const ship = new Ship(pose.x, pose.y, { canvas: { width: 1920, height: 1080 }, isMMO: () => false });
    ship.applyCustomization(stored);
    Object.assign(ship, { x: pose.x, y: pose.y, angle: pose.angle, visible: true, exploding: false, invulnerable: false, thrusting: true });
    const rec = recorder();
    ship.draw(rec.ctx);
    // outline: same multiset of world segments (either direction) and colours
    const want = rec.segments.slice();
    if (want.length !== pose.segments.length) problems.push(`segment count ${pose.segments.length} vs browser ${want.length}`);
    for (const [x1, y1, x2, y2, color] of pose.segments) {
      const i = want.findIndex(s => ((near(s.a[0], x1, 1e-3) && near(s.a[1], y1, 1e-3) && near(s.b[0], x2, 1e-3) && near(s.b[1], y2, 1e-3)) ||
        (near(s.a[0], x2, 1e-3) && near(s.a[1], y2, 1e-3) && near(s.b[0], x1, 1e-3) && near(s.b[1], y1, 1e-3))) && normColor(s.color) === normColor(color));
      if (i < 0) { problems.push(`segment ${[x1, y1, x2, y2].map(n => n.toFixed(2))} ${color} not drawn by the browser`); break; }
      want.splice(i, 1); segChecks++;
    }
    // endpoint dots (radius 1 at both ends of every custom line)
    const wantDots = rec.dots.slice();
    if (wantDots.length !== pose.dots.length) problems.push(`dot count ${pose.dots.length} vs browser ${wantDots.length}`);
    for (const [dx, dy, dr, color] of pose.dots) {
      const i = wantDots.findIndex(d => near(d.c[0], dx, 1e-3) && near(d.c[1], dy, 1e-3) && near(d.c[2], dr, 1e-6) && normColor(d.color) === normColor(color));
      if (i < 0) { problems.push(`dot ${dx.toFixed(2)},${dy.toFixed(2)} not drawn by the browser`); break; }
      wantDots.splice(i, 1); dotChecks++;
    }
    // thruster flame anchors
    if (rec.fills.length !== pose.flames.length) problems.push(`flame count ${pose.flames.length} vs browser ${rec.fills.length}`);
    for (const [fx, fy] of pose.flames) {
      if (!rec.fills.some(([bx, by]) => near(bx, fx, 1e-3) && near(by, fy, 1e-3))) { problems.push(`flame at ${fx.toFixed(2)},${fy.toFixed(2)} not in browser`); break; }
      flameChecks++;
    }
    // collision: the browser's checkCollision on the same circles (skip float-boundary ties)
    for (const [cx, cy, r, ps5Hit] of pose.hits) {
      const hit = ship.checkCollision({ x: cx, y: cy, radius: r });
      if (hit !== ps5Hit) {
        const margin = ship.customLines && ship.customLines.length
          ? Math.min(...ship.customLines.map(l => {
              const c = Math.cos(ship.angle), s = Math.sin(ship.angle), k = 0.25;
              const ax = ship.x + l.startX * k * c - l.startY * k * s, ay = ship.y + l.startX * k * s + l.startY * k * c;
              const bx = ship.x + l.endX * k * c - l.endY * k * s, by = ship.y + l.endX * k * s + l.endY * k * c;
              return Math.abs(ship.distanceToLineSegment(cx, cy, ax, ay, bx, by) - r);
            }), Math.abs(Math.hypot(cx - ship.x, cy - ship.y) - ship.radius))
          : Math.abs(Math.hypot(cx - ship.x, cy - ship.y) - (ship.radius + r));
        if (margin < 1e-3) { hitSkipped++; continue; }
        problems.push(`collision differs at (${cx.toFixed(1)},${cy.toFixed(1)}) r${r}: PS5 ${ps5Hit}, browser ${hit}`);
        break;
      }
      hitChecks++;
    }
  }
  return { problems, segChecks, hitChecks, hitSkipped, flameChecks, dotChecks };
}

const file = process.argv[2];
const ev = JSON.parse(fs.readFileSync(file, 'utf8'));
const byKind = {};
let segs = 0, hits = 0, skipped = 0, flames = 0, dots = 0, failed = 0;
const failures = [];
for (const e of ev.ships) {
  const r = judge(e);
  const k = byKind[e.kind] || (byKind[e.kind] = { ships: 0, accepted: 0, identical: 0, problems: [] });
  k.ships++;
  const rejected = r.problems.some(p => p.startsWith('server rejects') || p.startsWith('MMO sanitizer'));
  if (!rejected) k.accepted++;
  if (!r.problems.length) k.identical++;
  else { failed++; if (failures.length < 12) failures.push(`${e.kind} "${e.label}": ${r.problems[0]}`); k.problems.push(r.problems[0]); }
  segs += r.segChecks; dots += r.dotChecks; hits += r.hitChecks; skipped += r.hitSkipped; flames += r.flameChecks;
}
const summary = { editor: ev.summary, judged: ev.ships.length, segmentsMatched: segs, endpointDotsMatched: dots, flameAnchorsMatched: flames, collisionSamplesMatched: hits, collisionTiesSkipped: skipped,
  byKind: Object.fromEntries(Object.entries(byKind).map(([k, v]) => [k, { ships: v.ships, serverAccepts: v.accepted, browserIdentical: v.identical }])) };
quiet(JSON.stringify(summary, null, 1));
if (failures.length) quiet('first problems:\n  ' + failures.join('\n  '));
// Property and rebuild ships are produced by the editor and must all pass; corpus ships are
// other people's drawings and may legitimately exceed the editor's canvas (reported, not failed).
const mustPass = ['property', 'rebuild'];
const bad = Object.entries(byKind).filter(([k, v]) => mustPass.includes(k) && v.identical !== v.ships);
process.exit(bad.length ? 1 : 0);
