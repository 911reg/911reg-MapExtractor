# 911reg MapExtractor

Standalone C++20 / Windows x64 / CPU only. No editor runtime or map renderer.

## Use

1. Open `dist/MapExtractor.exe` and choose your game folder.
2. Keep the default `Output` folder or choose another results folder.
3. Pick your server type at the top, select **Doors**, **Water** or **Geodata**, then check the maps on the right.
4. Click **Start**. Read exported/skipped counts and reasons in Activity, then choose whether to open the output folder.

Server type, the combined-file preference and recent clients are saved in `settings.ini`. An existing `911reg.ini` is moved to `settings.ini` only when `settings.ini` is absent. One combined XML file is the default. The window has three tasks: Doors, Water and Geodata. There is no Advanced switch and no Map check button. `scan` remains a command-line inventory for the checks. The window does not write debug reports; the command line does, unless `--simple-output` is set.

Single-server exports contain copy-ready files directly inside the task folder, with `README_DOORS.txt`, `README_WATER.txt` or `README_GEODATA.txt`. Geodata writes `L2J/*.l2j` and `L2OFF/*_conv.dat` from the same cells. Copy one of those folders.

Choose the client, Maps or System folder and use the checklist to select files. Results go directly into Doors, Water or Geodata under the chosen output folder. No game/server file is
changed. The small language selector supports English, Greek, Spanish,
Brazilian Portuguese, Albanian, Romanian and German.

The game-folder field is an editable recent-client dropdown. The ten most recent
folders are saved locally in `settings.ini` beside the executable; the latest is
restored on launch. Maps/System selections share the same client entry. Paths
with spaces or Unicode characters are supported. Stale paths can be edited or
replaced using Choose. No client locations are built into the application.

## Door results

- Selected server format: L2JServer, aCis, L2Scripts, L2jMabios or L2OFF text. The L2jMabios label is the Mobius XML layout. The saved profile id and `--server` value stay `mobius`. The hidden `all` profile, used by the UI checks, writes one folder per format.
- Combined XML by default, or per-map XML when the combined-file option is disabled.
- With debug files enabled, `raw/<map>.tsv`: original RangeDeltas, location, world nodes, height and whether
  bundled standard defaults were needed. These facts are independent of server XML.
- With debug files enabled, `911reg-door-report.txt`: counts, rejected actors and limitations.

Door XML contains geometry **templates**, not recovered server gameplay rules.
HP, defenses and gameplay flags are explicit editable defaults in `profiles`.
Retain the target server's ownership, schedules, scripts and appropriate stats
when merging. Some servers load a single configured door file, so per-map output
must be merged into it; this tool does not install files into a live server.

`l2off/<map>.txt` contains UTF-16LE/CRLF `door_begin` records for
`Script/doordata.txt`. Merge geometry by `editor_id`; preserve the server's
names, NPC/script references, opening rules and stats. Generated identifiers
use a sanitized client name plus ID to prevent collisions. `normal_type`,
`by_npc`, HP and defenses are editable profile defaults. L2OFF height uses
ceil(RangeHeight), while position and individual range nodes round to nearest.

Normal and Classic files are independently selectable. Classic results use a
separate `Classic` folder with normal numeric filenames. The CLI also retains
its older `--classic` preference when `--all-variants` is absent.
With debug files enabled, `911reg-map-sources.tsv` records each selected source and output name.
Only exact numeric region Classic suffixes are treated as variants.

Missing Mover properties use compiled standard defaults: RangeHeight 150 and
four RangeDelta corners at (+/-80, +/-80, 0). Serialized map properties always
win. Door and water extraction do not open Engine.u or require System packages.
Water defaults include bWaterVolume=true for known WaterVolume classes and
inherited Priority=0 for known WaterVolume/Engine.PhysicsVolume classes.
Unknown subclass priority remains unresolved rather than guessed. Only defaults
used by extraction are bundled; geometry still comes from the selected maps.

Current extraction supports Engine.Mover door actors; L2NMover candidates with
door metadata are reported as unsupported rather than assigned Mover defaults.
Invalid/degenerate ranges, missing IDs and unresolved duplicate IDs
are reported, not invented. Mesh-bound fallback and custom Mover subclasses
remain pending. See [FORMATS.md](FORMATS.md).

## Water results

Water extraction reads actual `Gameplay.WaterVolume` / `Engine.WaterVolume`
actors and their collision brushes. It does not infer water from textures,
legacy ZoneInfo regions or generic physics volumes.

Four server-profile folders contain one XML file per map. `raw/*.faces.tsv`
preserves world-space brush faces; `raw/*.zones.tsv` records accepted footprints
and source Z limits. `911reg-water-report.txt` lists accepted/rejected volumes.
Concave vertical prisms and separate components are supported. Sloped brushes,
holes, incomplete geometry and potentially overriding dry volumes are reported;
they are never silently replaced with large boxes. General dry-volume
subtraction and custom class inheritance remain pending.

Server coordinates are integers: XY uses nearest rounding, while minimum Z is
rounded up and maximum Z down to remain inside the source's vertical interval.
Review incomplete extraction reports before merging data. The aCis profile's
water XML must go through its WaterZone loader; file placement differs by server.

L2OFF water lives in `Script/areadata.txt`, as `type=water` areas with `map_no`
and `water_range` minimum/maximum XYZ. `l2off/<map>.txt` contains UTF-16LE/CRLF
area fragments. Axis-aligned concave footprints split into exact rectangles;
rotated/slanted footprints are omitted from L2OFF with a clear report entry,
while the four XML profiles retain them. No bounding rectangle is substituted.
Map numbers come from numeric region filenames, including Classic selection.
These are fragments for merging, not replacements for the full server scripts.
The reference records and server binaries are not bundled or modified.

## Window

The charcoal UI uses RGB(32,32,32) with orange highlights, three task tiles,
server-format selection, a searchable map checklist on the right and an activity
panel. Check exactly which files to process; Select all and Clear affect the
whole list, while filtering preserves checks. Recent clients remain available.
Existing results prompt for confirmation before the selected task folder is
cleared. Clear results performs the same operation without starting extraction;
other task folders remain intact. The command-line `scan` task clears only `map-details.json`.
Linked/junction output trees are refused. Dated run folders are no longer created.
Hover over controls for translated dark tooltips. Activity shows the current
map, stage, progress and errors. With `--simple-output`, which the window always passes,
completion shows counts and skipped reasons in Activity. Debug runs also write
`911reg.log`, `Summary.txt` and `SelectedMaps.txt` in the output root.

Only region files named `XX_XX.unr` or `XX_XX_Classic.unr` are eligible for doors, water and geodata; sky, lobby and other named maps are ignored. Explicit non-region extraction inputs are rejected. `scan` can still inspect other UNR files.

**Prefer normal** and **Prefer Classic** uncheck the other version only when both versions of a region are selected. Unpaired maps stay selected, including when the list is filtered. These buttons never add unchecked maps.

Both normal and Classic maps are listed independently. The UI exports both when
checked. Normal geodata goes into `Geodata/L2J` and `L2OFF`; Classic goes
into `Geodata/Classic/L2J` and `L2OFF`. Both use normal numeric filenames.
Door/water XML places Classic data in `Classic` under the selected output (inside each profile folder for all-profile exports).

For doors and water, choose L2JServer, aCis, L2Scripts, L2jMabios or L2OFF.
Geodata ignores that choice and writes L2J and L2OFF. Combine XML produces
one `doors.xml` or `water.xml` per selected XML profile and variant instead of
per-map XML. The two data types retain their own server schemas. L2OFF retains
script fragments. Duplicate door IDs across maps reject combined XML.

Within one map, stacked doors sharing an ID and the same mesh/XY footprint use
the upper placement. Each discarded placement produces a warning in the door
report and a row set in `raw/<map>.duplicates.tsv`. Activity ends with a summary
of unique duplicate IDs per map. Unrelated conflicting footprints remain errors.
Export scans the live Level actor list rather than orphaned export-table actors.

RangeDelta corners that cross are ordered around their convex hull. If nearest
integer rounding collapses a thin footprint, the exporter chooses an enclosing
integer quadrilateral using only each corner's floor/ceiling coordinates.
Every adjusted X/Y stays within one unit of its source corner. Raw deltas/world
coordinates remain unchanged; `exportX/exportY/exportZ` record serialized nodes.

Every door run also saves `saticobject-e_ClientDat.txt` and
`staticobject-e_FileEdit.txt` at the door-output root, with a separate pair in
`Classic` when needed. These UTF-8/CRLF files contain sorted door IDs and the
StaticMesh object's leaf name, independently of a MoverName display override.
Missing/unrepresentable mesh names or conflicting ID/name pairs are omitted
and reported; names are never invented.

CLI tasks accept `--all-variants`, `--server <all|l2jserver|acis|l2scripts|mobius|l2off>`
and `--combine-xml` for the same export behavior. `--map-list <file>` accepts
UTF-8 with one absolute source UNR path per line. Empty lists and foreign maps
are rejected. UI runs save their list in `SelectedMaps.txt`. The older CLI
`--classic` preference remains available when `--all-variants` is absent.

Geodata reads `.unr` maps and their referenced client assets directly. Keep the
client's Maps, System, StaticMeshes and Textures directories together.
The CPU core builds 16-unit cells, stacked floors, headroom and four movement
flags, then writes L2J and L2OFF in separate folders. Both files store the same
heights and the same north, south, east and west bits. Diagonal movement is not written.

- `L2J/XX_YY.l2j`: packed height plus four movement directions.
- `L2OFF/XX_YY_conv.dat`: PTS header, block/layer counts and the same packed heights.

A region is 2048 by 2048 cells. Its origin is `((mapX - 20) * 32768, (mapY - 18) * 32768)`.
Each triangle is clipped to the cell. A flat clip that lies on one cell edge is dropped;
a thin diagonal sliver is kept. Solid spans merge when the gap is 3 units or less.
A lower span is dropped when the next floor leaves less than 46 units of headroom.
Heights are the surface plus a retail offset, truncated toward zero onto a multiple of 8:
terrain +26.296875, static mesh +26.5, model +26.875, and +29.375 on the top model span.
A stored step of 24 or less stays open. A taller step stays open only on gentle terrain
(walk angle 45.573 degrees, slope at most 0.3, rise at most 3.125 before the slope test).
The top of every column is kept, including a roof. Other floors stay when a PlayerStart
or an open direction can reach them. An empty cell is one closed layer at -16352.
An open 8 by 8 block may be stored as one height when the cells differ by at most 24.
World BSP planes at -16384 or 16384 that cover the whole region are removed.
The same rules are commented in `src/geodata.cpp` and `src/collision.cpp`. See [FORMATS.md](FORMATS.md).

Native import uses the level's actor database and BSP model, terrain heightmaps
with visibility/diagonal masks, stored static-mesh collision triangles and
simple collision models, and blocking-volume BSP. Actor transforms, collision
flags and per-material EnableCollision are applied. Dynamic movers/animated
meshes are excluded and counted, rather than frozen permanently into geodata.

Missing packages, malformed collision, unsupported blocking subclasses and
cylinder overrides fail the affected tile and are reported. A tile with known
missing collision is never published. Other selected maps may still finish.
A set direction bit is what the server walks. It does not measure the step again.
Test the result in game before replacing a live geodata folder. This is not a claim
that every retail cell matches.

## Build and command line

Run `BUILD.bat` with Visual Studio 2022 C++ tools and CMake installed. Keep the
executable, `Languages` and `profiles` together when moving the `dist` folder.
No workspace-specific source, binary or absolute path is needed by the build.

```powershell
.\MapExtractor.exe doors .\Client --output .\Output\Doors --threads 4
.\MapExtractor.exe doors .\Client\Maps\24_16.unr --output .\Output\OneMap
.\MapExtractor.exe water .\Client --output .\Output\Water --threads 4
.\MapExtractor.exe scan .\Client --output .\Output\map-details.json
.\MapExtractor.exe water .\Client --output .\Output\ClassicWater --classic
.\MapExtractor.exe geodata .\Client\Maps\24_16.unr --output .\Output\Geodata --threads 4
.\MapExtractor.exe geodata .\Client --output .\Output\ClassicGeodata --classic
```

Exit codes: 0 complete, 1 fatal/command failure, 2 incomplete extraction with
reported errors. Existing output is refused. Default concurrency is capped at
four logical CPU workers; `--threads` accepts 1–64. Each worker may hold a decoded
package up to 512 MiB; use higher concurrency deliberately.

## Translate

Edit UTF-8 `Languages/strings.tsv`: rows are stable string IDs, columns are
language codes, and `language_name` supplies each selector label. Add a column
to add a language. Empty translations fall back to English. Preserve `{0}` and
`{1}` count placeholders; `\n` inserts a line break. Preferences are stored in
`settings.ini` beside the executable. Detailed parser diagnostics remain English.

## Verify

`VERIFY.ps1 -Map <map.unr>` checks map scanning, errors and the UI workflow.
`VERIFY-GEODATA.ps1` checks the L2J and L2OFF layouts, every cell's shared height
and cardinal movement, PTS counters, deterministic
workers, malformed input and Classic preference/fallback.
`VERIFY-NATIVE.ps1` checks bounded native BSP/static-mesh collision fixtures,
material flags, simple collision precedence, transforms, masks, both
formats, deterministic workers, client scope, Classic selection and native UI.
Optional `-Map <client/Maps/24_16.unr>` also audits and generates a real map,
checks every output cell, and verifies missing-package refusal and source hash.
The OBJ-based `geodata-fixture` command is an internal geometry test harness;
the application and normal `geodata` command accept native client maps.
`VERIFY-L2OFF.ps1 -DoorMap <map.unr> -WaterMap <map.unr>` checks L2OFF text
grammar, encoding, range/position/height, water area/Z/map number and source
hashes. Optional `-ReferenceScript <Script-folder>` compares shared door IDs and
counts active reference water areas without copying original records. These
checks do not launch or validate an L2OFF server runtime.
`VERIFY-DOORS.ps1 -Map <24_16.unr> -RangeMap <22_22.unr>` checks the four outputs,
Classic names, single-map UI, translations and the supplied RangeDelta fixture.
Optional `-L2jServerSchema` and `-MobiusSchema` validate against supplied XSDs.
`CompareDoors.ps1` accepts raw TSV plus four reference XML files and compares
the same door's XY nodes and vertical ranges without modifying the references.
`VERIFY-WATER.ps1 -Map <water-map.unr>` checks geometry, four XML profiles, map/
client scopes, Classic names, escaping, determinism, UI and source-file hashes.
Optional `-L2jServerSchema`, `-MobiusSchema`, `-L2ScriptsDtd` validate the water
outputs. `-Executable` selects a side-by-side build when the main app is open.
All paths are supplied at runtime; no reference installation is required to run
the extractor.

## Next

Mesh fallback, custom classes, an FandC profile, non-prismatic water and cylinder
overrides on blocking meshes are still open. Test generated geodata in game
before replacing a live server.
