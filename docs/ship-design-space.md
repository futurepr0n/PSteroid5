# Ship editor design space (evidence first)

Written 2026-10-23 before any editor code. Sources: the production public gallery (33
custom ships, `tools/ship-corpus.js`; geometry in `ps5/testdata/ship_corpus.json`,
passphrases stripped), and CSSTeroids-2 at 34b2418 (read-only):
- `public/js/entities/ship.js`: drawing at 0.25 and line collision;
- `public/js/shared/world-sim.js`: weapon points become bullet origins;
- `utils/helpers.js`: `validateShipData`, `sanitizeMMOShipData`;
- `routes/ships.js`: save and passphrase;
- `public/js/ship-customizer/`: the browser editor.

## What the browser stores
- Design space: a 400x400 canvas with the origin at its centre, so coordinates run ±200. The
  nose points up (−y). Lines are `{startX, startY, endX, endY, color}`. Points are `{x, y}`.
- In game every coordinate is multiplied by 0.25, so ±200 design units = ±50 px.
- **The drawn lines are the hitbox** (`Ship.checkCustomShipCollision`), plus a 15 px
  centre-radius fallback.
- Weapon points are bullet origins. Two points use 0.75x speed and 0.7x range
  (`createShotBullets`).
- Limits: `validateShipData` allows at most 3 thrusters and 2 weapons, a non-empty name, and
  at least one line for type custom. `sanitizeMMOShipData` drops the whole shipData above 500
  lines and caps strings at 32 characters.
- `POST /api/ships` **with an existing passphrase overwrites that ship**; without one it
  creates a ship with a new adjective-noun-verb passphrase.

## What players actually build (33 custom ships)
| Measure | Result | Consequence for a controller editor |
| --- | --- | --- |
| Lines per ship | median 11, p90 43, max 73 | 500 is never the binding limit; 10–75 lines must be quick |
| Extent | median ±101 x ±143; 1 ship beyond ±200 | a ±200 canvas holds 32/33 as drawn |
| Endpoints shared with another line | median 17%; 8 ships mostly connected | people draw **loose strokes**, so segments are the primitive, not polygons |
| Mirror-symmetric lines | median 60%; 15 ships ≥ 80% | mirror on by default, one-button toggle |
| Endpoints exactly on the browser's 10-unit snap grid | 0% of ships | everything is freehand: a coarse grid cannot reproduce it |
| Colours | median 1, max 5; 6 multi-colour; custom hex common (`#ca3f3f`, `#1f0f0f`, `#04fb2d`) | per-line colour, presets plus custom colour, exact hex preserved |
| Weapons | 0/1/2: 7/8/18 ships, median y −83 | both slots used; place at the front |
| Thrusters | 0/1/2/3: 5/1/20/7 ships, median y +109 | pairs are the norm, so mirror placement |

## Decisions
- **Grid 5 design units by default (1.25 px in game; worst case 0.9 px off), with a fine
  1-unit step.** A stick can land on points exactly, and any freehand corpus ship can be
  rebuilt within under a pixel.
- Primitives: independent segments drawn start → end. While drawing, the cursor snaps to
  existing endpoints, so outlines are easy but not required.
- Tools: LINE, MOVE (endpoint drag), ERASE, PAINT, THRUSTER, WEAPON. Mirror (x → −x) applies to
  every tool and is on by default.
- Undo/redo for every change.
- Invalid ships cannot be made: 500 lines, 3 thrusters, 2 weapons, points within ±200, name
  1–32 characters.
- Live truths in the editor: a 1:1 in-game-size preview flying in place, the collision outline
  (the lines themselves plus the 15 px fallback circle), weapon firing directions, thruster
  flames, plus a test flight.
- Ownership: ships created here or loaded by passphrase are updated in place. Gallery ships
  are saved as a copy, so nobody else's ship is overwritten.
- Input: DualSense stick cursor plus D-pad grid steps; optional touchpad works like a laptop
  trackpad (finger moves the cursor, pressing the pad = mouse button, hold and move = drag).

## Found while reading (not part of this task)
`GET /api/ships/public` returns every public ship's **passphrase**. Because a POST with a
passphrase overwrites the ship, anyone can overwrite any public ship. Reported to the
owner; no server change made.
