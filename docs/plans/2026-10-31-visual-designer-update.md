# PSteroid5 01.003.000 update plan

Reference: read-only CSSteroids-2 at `4a9fa48`. Baseline port: `c1b29f8`,
01.002.001, title PPSA77305. Keep that title/content identity and native toolchain.

1. **Preserve the release.** Archive the existing folder with hashes before building.
2. **Designer update.** Adapt the reference's neon tool rail, contextual inspector,
   rotation-centre marker, clear Ship / Test navigation and status to TV/controller
   use. Add undoable whole-ship scale and local draft recovery. Browser editor-document designs save as PS5 copies to
   preserve their original fill recipes. Keep controller and
   touchpad placement. Preserve newer curve, thickness and baked pattern-line data on
   load/save, draw curves and thickness in gameplay, exclude pattern decoration
   from collisions, and relay those fields online. Full pattern authoring/library
   is a separate feature; this update retains and displays imported baked patterns.
3. **Space update.** Port the three star layers (depth .06/.15/.30), twinkle,
   tinted pulsars, and 6–16 second arcing shooting stars. Drive travel from actual
   ship velocity in single player, test flight, co-op and MMO, independent of camera.
4. **Trail update.** Bounded trails at up to three nozzle anchors, nearest hull
   colour plus palette accents; pixels normally, ribbon for rapid, embers for spray
   (rapid wins). Apply to local/remote pilots. Clear on death, warp, ship replacement
   and mode transitions; avoid long connecting lines across discontinuities.
5. **Verify/build.** Focused deterministic tests for effects, geometry, editor,
   lifecycle and remote relaying; existing simulation/network/audio/editor suite;
   native link, SELF integrity, runtime hash and both folder validations. Record
   source hashes and package checksums. Keep reference and platform sources intact.
6. **Console test deployment.** Preserve remote title evidence/rollback, transfer
   the complete validated folder to its existing scan location, verify readback.
   Do not touch saves, app.db, firmware or unrelated loader settings. Console
   launch and visual/controller/performance checks are distinct from transfer.
7. **Store promotion after hardware acceptance.** Prepare release notes, archive,
   source/notice evidence and the existing catalog update workflow. Publish the
   tested artifact only after the console test results are confirmed.

Hardware acceptance: designer readability and navigation; load a multicolour
curved/pattern ship; scale/undo/draft restart; test flight/return; movement in
single/co-op/MMO; stationary twinkle and curved shooting stars; pixel/ribbon/ember
trails, expiry and rapid+spray precedence; remote trails and camera following;
death/respawn/warp/disconnect; pause/interception; audio; stable frame pacing.

Status: implemented; final build and deployment verification in progress. Hardware verification and store publication
remain pending until explicitly recorded against the built package hash.
