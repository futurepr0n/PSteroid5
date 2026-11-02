# Complete the PS5 pattern designer — 01.004.000

This corrects the scope omission in 01.003.000: rendering imported pattern lines
was insufficient. Pattern authoring is part of the requested designer update.
Keep the existing stars/trails, PPSA77305 identity, saves, native runtime, and
read-only CSSteroids-2 reference. Retain the prepared/deployed 01.003.000 package.

1. Port planar region detection, holes, clipped repeating tiles and all six
   procedural starters from browser geometry.mjs. Bound dense generation and
   reject an edit atomically if it exceeds the ship/tile budgets.
2. Integrate FILL, a side-by-side tile/ship preview, controller/touchpad drawing,
   move/erase/paint, colours/widths, mirror, undo/redo, preset/blank artwork,
   repeat scale, zoom/pan/minimap, apply/cancel/remove and test flight.
3. Preserve stable hull IDs and editable fill recipes. Refit fills after hull
   edits/scaling, remove orphaned artwork, and make these changes undoable.
4. Persist pending tile work in local drafts. Add a local pattern shelf and
   browser-compatible CSP1 file import/export. Do not enable the reference's
   test-database cloud pattern library on the production server.
5. Use the existing versioned editor save/update endpoints so the browser can
   reopen editable fills; keep asynchronous save snapshots separate from newer
   local changes. Gallery copies and ownership rules continue to apply.
6. Run native model tests, independent browser geometry/code/document comparisons,
   HTTP create/update/load fixtures and the existing regression suite. Require
   native SELF integrity, runtime hash, WSL/final package validation and source hashes.
7. Deploy the complete candidate with readback and rollback, then obtain actual
   console observations before store promotion. Compilation and FTP verification
   do not establish UI readability, successful launch or gameplay acceptance.

Hardware checks: closed/open/curved hulls, divided areas and holes; six starters;
custom mirrored multicolour tile; width, move, erase, undo/redo; scale density
failure; apply/cancel/remove; zoom/pan/minimap; local shelf; CSP1 exchange; restart
and pending draft recovery; browser save/load/edit round trip; hull edits/refit;
test flight and multiplayer rendering; existing stars/trails; frame pacing.

Status: implemented; final regression build and console deployment pending.
