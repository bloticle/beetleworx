# Movement abilities

The modern controller preset uses a B tap for dash and a B hold for the retail
interact/companion-call action. A tap starts dash on release. After 250 ms,
holding B sends the original held B input without starting a dash. RB remains
available for immediate interaction. Custom bindings outside this preset keep
their existing mapping. Menu A/B inputs remain physical controller buttons.

Mickey's Rebrushed animations were supplied by `@llenarox` on Discord. Oswald
uses his existing animation resources. The converted Mickey clips retain the
original 95-bone rig and run through the game's native Havok sampler.

## Archive dependency order

`_Dynamic.pak` registers resources sequentially. Processing
`characters/mickey/mickey_default.hkw` initializes the animation bindings at
that point. Imported clips must precede this record. Appending them to the end
of the archive makes them unavailable during initialization, leaving the new
states bound to the three-second reference pose even after the pack finishes
loading.

`scripts/build-pc-menu.py` inserts the clip resources before this consumer,
preserving the relative order and contents of every existing resource. The
`ability-resource-order` CTest covers sequential initialization, a real PAK
roundtrip and rejection of ambiguous or replacing inputs.

## Verification and limits

The private `em2_qa_emr_clip_directory` diagnostic checks both detached fixtures
and the resources actually bound to Mickey's native character. It samples into
private output buffers without activating a state or writing an actor pose.
The old archive order fails the live binding check. The corrected order passes
all six clips, 28 sample times and 95 bones, within the import tolerances.

These checks establish resource discovery, native binding and pose sampling.
Movement transitions, landing damage, all 2D/3D areas, EM1 compatibility and
Oswald still require broader gameplay acceptance. The source changes do not
constitute a new public release.
