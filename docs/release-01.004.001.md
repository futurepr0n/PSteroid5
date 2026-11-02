# PSteroid5 01.004.001 — fine view zoom and hull choices

- Ship-editor D-pad UP/DOWN now supports small zoom nudges and steady hold
  adjustment, with a visible 25–400% slider and percentage instead of preset jumps.
- OPTIONS > SHIP TYPE / STARTING HULL offers editable Classic, Triangle, Diamond,
  Meridian, Manta, Delta and Blank outlines with thumbnails.
- Browser study hulls are fitted uniformly to the drawing square. Choosing is
  undoable, clears previous fills/hardpoints, and retains the weapon sound.
- Pattern-editor controls and explicit saved-geometry scaling remain available.

Automated checks cover hold timing across frame rates, zoom bounds, scaled view
framing, closed starter regions and full undo restoration. Browser study shape
comparisons use the read-only browser sample() function. Actual controller feel,
slider readability and hull picking still require console acceptance.
