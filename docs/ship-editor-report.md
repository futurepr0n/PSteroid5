# Ship editor: what the evidence showed

Milestone 6, 2026-10-23. Design rationale: [ship-design-space.md](ship-design-space.md).
The numbers below are regenerated on every build (`ps5/build.sh` → `ps5/testdata/ship_evidence_report.json`).

## The judges (code this project did not write)
- **CSSteroids-2 server validators:** `validateShipData` (what `POST /api/ships` accepts) and
  `sanitizeMMOShipData` (what the shared world keeps), loaded read-only from `../CSSTeroids-2`.
- **The browser's own `ship.js`:** `draw()` run against a recording 2D context, and
  `checkCollision()` on sample circles, after the same JSON round trip the database applies.

## Results
| Evidence | Result |
| --- | --- |
| Property test: 400 random editor sessions, 16,169 edits (4,055 correctly refused) | the editor never left the valid space; 384 finished ships: **384/384 accepted by the server**, none dropped or trimmed for MMO |
| Same ships drawn and collided by the browser's `ship.js` | **identical**: 24,507 outline segments, 49,014 endpoint dots, 3,543 flame anchors and 162,000 collision samples matched (0 boundary ties) |
| Public gallery (33 custom ships) loaded into the editor and saved back | **33/33 lossless**; server accepts and browser draws all 33 identically |
| Gallery rebuilt from scratch with editor operations on the 5-unit grid | **32/33**, worst endpoint shift 3.41 design units = **0.85 px in game** (49 tiny strokes needed the fine grid) |
| The 1 gallery ship drawn beyond the editor's ±200 canvas | still editable and savable as it is ("no new violations" rule) |

## What the evidence disproved or changed
1. **Freehand, not grids.** No gallery ship lies on the browser's 10-unit snap grid, so the
   editor uses a 5-unit grid (visually lossless) plus a 1-unit fine step.
2. **Loose strokes, not outlines.** Only 17% of endpoints are shared, so segments are the
   primitive. Snapping to existing points is offered, not required.
3. **Dots are real.** 20 gallery lines (3 ships) have zero length: a click without a drag,
   drawn as dots. The first editor refused them and could not rebuild "sillywtf". The editor
   now makes a dot when a line's start and end coincide.
4. **The PS5 renderer was missing the browser's endpoint dots on every ship.** The first
   recorder ignored `arc()` and missed it. The recorder now checks dots, and the PS5 draws
   them (49,014 matched).
5. **Whole-ship canvas checks locked legacy ships.** A gallery ship already beyond ±200 could
   not be edited at all. Edits are now refused only when they add a violation.

## Found, not changed (CSSteroids-2)
`GET /api/ships/public` returns every ship's passphrase, and `POST /api/ships` with a
passphrase overwrites that ship, so anyone can overwrite any public ship. The PS5 editor only
updates ships made on the console or loaded by passphrase, and saves gallery ships as copies.
The server needs its own fix; ask before changing it.

## Not yet proven (needs the console or a live write)
- **The touchpad byte layout.** It follows the SDK pad-data layout anchored by the platform's
  own asserts. Implausible values read as "no touch", and the first touch is logged to klog:
  `PSteroid5 editor: touchpad active (...)`.
- **A real save to production and a load in the browser.** That is a live write to the
  public database, so it is left for you to do on the console (step 6 below). Every part of
  it short of the network write is covered above.

## Controller test plan (for players who did not build it)
Time each task from opening SHIP EDITOR. No help beyond the on-screen hints.
1. **Arrow** (target under 60 s): nose line from (0,−60) to (−40,50) with mirror on, a rear
   bar, 2 weapons at the nose, 2 thrusters at the rear.
2. **Rebuild "two"** from the Hangar preview (target under 4 min): any colours. Compare the
   3x collision outline with the original.
3. **Touchpad only** (target under 90 s): draw three lines by press-drag-release on the
   touchpad, move one point by press-drag, undo once.
4. **Custom colour:** make `#ca3f3f`, paint one line, check the swatch matches.
5. **Test flight:** fly, confirm bullets leave the weapon points and the hitbox matches the
   collision outline (touch an asteroid with a wingtip).
6. **Round trip:** SAVE, note the passphrase, load it in the browser customizer/game and on
   the PS5 Hangar, and confirm both look the same. Then edit on the PS5 and SAVE again: the
   same passphrase updates.

Record: time per task, wrong-tool presses, undo count, and anything a tester could not find.
