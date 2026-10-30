# 911reg extraction and profile contract

## Client facts

The bounded package reader supports the current UE2 table profiles 118–133,
licensee versions through 40, and plain / Lineage2Ver111 / 120 / 121 envelopes.
Recognizing an envelope alone does not establish support for every native class.

Door identification requires Engine.Mover and serialized
`L2ServerObjectType=1`. The server ID is read directly. Deleted actors are
excluded. Missing IDs are rejected rather than derived from a guessed map grid.

The four RangeDeltas are world-axis offsets: `node = Location + RangeDelta`.
They must not be rotated a second time using actor Rotation. Raw floating-point
deltas are preserved separately; the world addition uses float precision as
the client does. XML node rounding is nearest integer, ties away from zero.
Vertical bounds use floor(minimum node Z) and ceil(maximum node Z + RangeHeight).
This conservative interval can be one unit taller than RangeHeight.

Omitted RangeHeight/RangeDelta values use bundled standard Mover defaults:
height 150, corners (-80,-80,0), (80,-80,0), (80,80,0), (-80,80,0).
Serialized actor values take precedence. Engine.u is not opened for defaults.
The raw clientDefaults column remains for compatibility and marks use of these
bundled inherited values.

Non-finite coordinates, unusable heights and unrecoverable footprints reject.
Crossed corners are reordered; collapsed integer footprints use enclosing
floor/ceiling coordinates with raw originals preserved. Stacked duplicate IDs
within a map keep the upper placement when mesh and relative geometry match,
with discarded placements retained in raw audit data and a final warning.
Other duplicate conflicts remain errors. Normal and Classic variants can both
export, with Classic in a separate folder and normalized numeric stems.
Custom subclasses, mesh-bound fallback and general class inheritance remain
unsupported. The report separates rejected actors, warnings and failed maps.

WaterVolume classes inherit bWaterVolume=true and PhysicsVolume Priority=0;
explicit map flags and priorities override those compiled defaults. Known
Engine.PhysicsVolume priority also defaults to zero. Unknown subclasses retain
unresolved priority for conservative overlap handling. Brush geometry is read
from the map, without looking up engine/gameplay class packages.

## Geodata formats

The generator writes L2J and L2OFF/PTS from the same collision result, one folder
per format. `--geo l2j` (the default), `--geo both` or `--geo l2off` chooses which
folders are written. The Geodata tile uses the same choice. Only the four cardinal
directions are stored. East is bit 1 (+X),
West is bit 2 (-X), South is bit 4 (+Y), North is bit 8 (-Y). A set bit is walkable.
All fields are little endian. L2J uses
byte block tags 0/1/2, raw signed flat height and packed complex/multilayer
height with low four NSWE bits. Multilayer counts are bytes, limited to 125.

The walk height includes surface offsets, then truncates toward zero onto a multiple of 8. Default offsets are terrain +26, static mesh +26.5, model +26.875, and +29.375 for the top model span. Headroom is 46. Spans closer than 3
units merge. Mesh rises use an unquantized surface-rise limit of 16.32; terrain applies its steep-slope check before granting movement. Steep terrain uses walk angle
45.573 degrees and slope 0.3, with a 3.125 surface rise before that slope is
considered. Exposed column tops and floors reachable from them or a PlayerStart are retained; disconnected lower layers are removed. This does not prove that every retained roof is accessible, and isolated areas entered only by teleport need a suitable seed. Unknown cells are closed at -16352. A flat block is emitted only when all 64 cells have the same height and all directions open. Exact flat serialization preserves the canonical cell heights. Generation first compacts fully open single-floor blocks spanning at most 32 units, changing each height by at most 24. `--exact-heights` disables that generation-only approximation.

Client surface offsets are the default; `--world-heights` exposes raw surfaces for diagnostics. Edge directions are validated against the neighbouring map's collision (same variant); missing neighbours are closed. Only boundary blocks are rasterized for neighbours and a bounded cache avoids retaining whole clients.

L2OFF uses an 18-byte header: byte region X/Y, word 128, word 16, dword stored
nonflat layer count, dword flat+complex block count, dword flat block count.
Word block tags are 0 (two equal raw height bounds), 64 (complex), or the total
layer count (multilayer). Multilayer cell counts are words. Payload heights and
cardinal flags use the L2J packing. Filename is `XX_YY_conv.dat`.
Unknown space is represented by a closed layer at -16352; no empty cell is made freely walkable.

Binary contracts were checked against the supplied L2J/Mobius and L2Scripts
loaders and this public format implementation. These are format references;
their runtime/editor code is not included in the executable:

- [PTS converter format](https://github.com/zcxv/fgeodata_converter/blob/master/src/conv/ConvType.java)

Proprietary/compressed variants and live gameplay validation remain pending.
This is not a claim of compatibility with every
historical or custom loader.

Native collision is decoded independently from client packages. The level's
native actor database identifies live map actors; its model reference selects
world BSP, avoiding orphaned brush models. BSP planes set face winding and
NotSolid/Portal surfaces are excluded. Terrain reads native ToWorld coordinates,
G16/P8 height mips, QuadVisibilityBitmap and EdgeTurnBitmap, including Inverted.
Static meshes use serialized collision-face indices/planes with material
EnableCollision, or the referenced collision Model when UseSimpleBoxCollision
selects it. Render triangles are not substituted for missing collision.
Transforms include Location, PrePivot, DrawScale, DrawScale3D, quantized UE2
rotation and mirrored winding. Blocking volumes use their transformed Models.

StaticMeshActor cylinder overrides use Location, CollisionRadius and
CollisionHeight (half-height) directly in world space. Circular cell overlap
and the complete vertical solid span are rasterized analytically, with no
mesh package dependency or polygon approximation. Terrain XY clipping applies.

Native layouts are bounded to the supported UE2 package profiles. Missing
static-mesh packages exclude affected actors with warnings. Missing required
terrain/model assets, malformed relevant arrays and unrecognized potentially
blocking mesh subclasses remain errors, preventing publication of that tile. Doors and animated/nonstatic meshes are excluded from the static
environment and counted in per-map reports. No editor code, executable,
renderer, headers or runtime is linked or shipped with the extractor.

## XML profiles

Profiles are independently authored structural templates based on supplied
reference layouts; no original datapack or machine-specific location is bundled.

| Profile | Door geometry | Water layout template |
| --- | --- | --- |
| L2JServer 2.6.3.0 layout | Attributes: pos separated by semicolons; node1–4 by commas; nodeZ and height | WaterZone / NPoly, uppercase X/Y nodes |
| aCis-style layout | position, four coordinates/loc elements, stats height | WaterZone loader file; NPoly; lowercase x/y nodes; no type attribute |
| L2Scripts layout | pos and shape ax/ay through dx/dy, minz/maxz | type=water, polygon/coords loc with x y minZ maxZ |
| Mobius layout | nodes/nodeZ, four node elements, location/height | WaterZone / NPoly, uppercase X/Y nodes |
| L2OFF Interlude script layout | door_begin, editor_id, position and four XYZ range nodes, height | area_begin, type=water, map_no, water_range Min/Max XYZ |

Water profiles now export extracted WaterVolume geometry. Cuboids use their
four-corner footprint rather than a separate Cuboid template.
FandC has no verified reference profile yet.

The L2OFF layout was checked against user-supplied Interlude `Script/doordata.txt`
and `Script/areadata.txt`. Only independently authored structural templates are
shipped. Original records, server binaries and their installation paths are
excluded. Runtime compatibility is not established by text validation alone.

L2OFF emits UTF-16LE with BOM and CRLF. Per-map `.txt` fragments are merged into
the corresponding global script. Door identifiers are sanitized client names
with editor-ID suffixes; existing server names/references must be reconciled
when merging. Door height is ceil(client RangeHeight), range XYZ and location
are nearest integers. Server stats and normal_type/by_npc are template defaults.

Water minZ is ceil(bottom), maxZ is floor(top), and XY uses the same rounded
footprint as XML. Orthogonal footprints are decomposed into non-overlapping
rectangles by horizontal strips, merging identical adjacent intervals. Total
rectangle area must equal polygon area. Slanted edges, nonnumeric map names
and excessive complexity cause L2OFF-only omissions, reported separately and
included in incomplete exit status 2; XML output remains available.

## Water geometry

Only the two recognized WaterVolume classes are export sources. An explicit
false bWaterVolume or deleted actor is excluded. A serialized positive water flag also enables an explicit custom/PhysicsVolume source with brush geometry; unknown inherited flags are not guessed.

The brush Model is decoded through its bounded vectors/points/nodes/surfaces/
vertex-pool prefix. Actual BSP collision faces are reconstructed. Location,
PrePivot, DrawScale, DrawScale3D and UE2 quantized Rotator values transform those
faces into world space. Old MainScale/PostScale editor fields do not replace
the runtime actor transform. No editor or rendering library is used.

Flat top/bottom faces must yield the same boundary edges. Internal cap edges
cancel; remaining degree-two loops form footprints, including concave shapes.
Cap areas and side-wall coverage must agree with those footprints. Holed footprints decompose along native cap faces. Slopes, incomplete walls, cap-area mismatches and invalid loops are rejected. Separate components become separate named zones. Geometry tolerances are
0.01 world units; XY edge keys use 1/1024-unit resolution.

Rounded footprints must remain simple, positively oriented polygons. Exported
minZ=ceil(source minimum), maxZ=floor(source maximum); an empty interval is
rejected. Original transformed faces and source Z limits remain in raw TSV.

Nearby ordinary PhysicsVolume/AirVolume brushes are inspected only for dry
overrides. Higher-priority dry prisms are subtracted in XY and Z. Overlapping tied or unknown priorities remain explicit errors because their precedence is not established. Dry regions are not exported as water. General subclass inheritance remains unsupported.
Legacy water ZoneInfo regions and water materials are intentionally excluded.

Templates expose only XML-escaped geometry/name fields and structural content.
Replacement values are not reinterpreted as template syntax. HP=158250,
PDef=644 and MDef=518 in the initial door templates are editable example values,
not client facts or a recommendation for any particular door. Other gameplay
attributes are omitted or explicit template defaults. Never overwrite a server's
gameplay definitions wholesale with client-derived geometry.

Schema locations are server-relative XML metadata, not local machine paths.
The target XSD/DTD is not copied into the distribution. Integration must use the
destination loader's file placement and existing schema files.

## Regression evidence

The supplied Goddard door 24160001 in 24_16 was extracted from three tested
client installations with identical raw rows: 23 total doors in that map.
Against the selected client's world nodes, the supplied server records show
maximum XY component deviations of 1.359375 units for L2JServer/aCis/L2Scripts
and 0.44921875 for Mobius. Different vertical bounds in those server records are
preserved in comparison output rather than treated as extraction truth.

The supplied 22_22 Mover6 RangeDeltas are checked to their six-decimal published
precision. Native float values can differ below the last supplied decimal.
The L2JServer and Mobius outputs passed their supplied XSDs. All four layouts
were independently read back to verify their integer XY nodes against raw facts.

## Pathnodes

The legacy `.pn` writer follows the supplied PathnodeGenerator data layout: 256 rows (Y), each containing 256 columns (X); each entry has a one-byte node count. A node contains a signed big-endian 16-bit height and eight one-byte links in N, NE, E, SE, S, SW, W, NW order. Link zero means no route, otherwise the value is the target node layer index plus one. Layers are ordered by descending height at cell (blockX*8+3, blockY*8+3). Empty collision cells have zero nodes, rather than routes across unknown space.

Links follow NSWE cell transitions; diagonals require both orthogonal passages to agree on the target layer. Region borders require neighbouring data. The writer is independently implemented; reference application code and rendering libraries are not included.

Vertical-prism water is shared between XML export and geodata movement checks. Holes are decomposed along native cap faces; higher-priority dry prisms are subtracted in XY and Z. Geodata can additionally classify non-prismatic brush water using serialized BSP node planes, front/back links, solidity flags and RootOutside. Its planes are transformed with the actor's inverse transpose, so rotated, reflected, scaled, concave and warped-polygon brushes use their native BSP classification rather than a bounding box or fitted vertex plane. On this path, dry/wet priority is evaluated at each sampled floor point. Malformed/cyclic BSP, unknown or tied overlapping priorities and legacy ZoneInfo remain explicit failures. XML formats that require vertical prisms continue to reject unsupported shapes. Geodata uses water to classify submerged floors; water surfaces are not fabricated as solid floors. Server swimming still requires the matching water-zone definitions.

G3DEditor is a read-only format reference: https://github.com/MobiusDevelopment/G3DEditor/tree/master/G3D-Editor. No GPL implementation or renderer was copied into this executable. Client reference dumps are local test inputs, not shipped assets.

World bounds: horizontal world-BSP caps at exactly Z=-16384 or +16384 are excluded even when split, clipped or only partially covering the region. Terrain and mesh surfaces are not excluded by this rule. Vertical BSP/blocking walls whose top is outside the encodable height range remain solid collision spans but do not create an out-of-range walkable top. All finite floor candidates outside the packed range [-16384, 16384) are omitted from exported layers, with a per-map count in the Console and debug report. Their solid spans remain in the rasterization data for occlusion and headroom; other representable floors are retained. The encoder remains strict, and no height is clamped or wrapped.

Bellion matching-client calibration: a 256-sample texture with serialized full 256-quad sector coverage needs a clamped extra vertex row/column; bitmap indices retain the texture-width stride. Terrain centre samples take precedence over off-centre triangle fragments. Downward-facing surfaces supply collision/ceilings, not walkable floors. Consistently wound watertight mesh collision contributes solid interior spans; open meshes are not filled. The matching client dump writes direction bits least-significant first; test import normalizes that text convention before comparing L2J masks. Exact client parity remains pending; cell/layer differences are recorded under `_tests`.

Collision triangles are two-sided. Movement chooses the lowest neighbouring span with shared headroom, uses unquantized surface rise (16.32 units onto gentle surfaces; 3.125 onto steep surfaces), and checks directional steep-terrain slope. Reachability retains exposed column tops and PlayerStart-connected lower floors. These rules apply to every mesh and map; there are no asset exclusions.
