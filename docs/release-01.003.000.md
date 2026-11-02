# PSteroid5 01.003.000 — designer and space visuals

- Controller designer: neon tool rail, context inspector, rotation-centre marker,
  clearer help, undoable scaling and recoverable local drafts.
- Newer ship designs: curve controls, line thickness, coloured baked patterns,
  hull-only collision and multiplayer style relay. Up to 5,000 lines locally;
  online decoration is shed above the server's 2,000-line limit.
- Three movement-driven parallax star layers, twinkling coloured pulsars and
  occasional curved shooting stars in single player, co-op, MMO and test flight.
- Short ship-coloured trails for local and remote pilots: pixels normally,
  ribbons during rapid fire, soft embers during spray. Rapid takes precedence.
  Trails clear on death, discontinuities, ship changes and mode changes.
- Same PPSA77305 identity, save locations, native runtime and server protocol.

Browser designs with editable pattern documents save as PS5 copies to preserve
their originals. The port retains baked artwork; full pattern authoring and curve
bending are outside this update. Soft glow uses layered HUI alpha geometry.

## Verification and promotion

Portable effects/geometry tests plus the existing gameplay, power-up, network,
editor, browser parity, audio and multiplayer fixtures run through Build-PS5.ps1.
The build also checks native imports, SELF integrity, the runtime hash and both
staged/final folder packages. The release preparation records the exact executable,
package archive and source archive hashes.

Console acceptance remains pending until recorded for this candidate. Transfer
verification is not a launch or gameplay test. Do not promote this candidate to
the store before accepting the designer, effects, multiplayer and frame pacing
on the console. The previous 01.002.001 package and remote title remain available
for rollback.
