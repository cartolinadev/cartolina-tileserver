# Tileserver backlog

This backlog contains work confined to `cartolina-tileserver`. Tasks that
change frontend behavior, define a frontend/backend contract, or require an
RFC remain in the
[cartolina-js backlog](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/backlog.md).

The metanode-store and unified-tiling work in this backlog is specified by
[RFC 7](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md).

**New entries go directly below this line, newest first — never below an
existing entry, even one added earlier in the same session.**

## Publish surface credits in the map configuration

**Opened:** 2026-07-31
**Status:** open

A `tms` resource publishes its credits as string-keyed definitions in
`boundlayer.json`, which the client reads directly. A `surface` resource
publishes nothing equivalent. `MetatileBuilder` calls
`node.updateCredits(credits)`
([metatile.cpp](../mapproxy/src/mapproxy/generator/metatile.cpp),
[metatile-store.cpp](../mapproxy/src/mapproxy/generator/metatile-store.cpp)),
writing the resource's credit ids into the metatile credit table as
16-bit numeric ids, while only the definitions reach the map
configuration's top-level `credits` registry.

Three consequences follow from that encoding:

- Every credit used by a surface needs a numeric id that is unique
  across the installation and present in the registry. A `tms` credit
  needs neither; see the numeric-id requirement in
  [resources.md](resources.md).
- A missing or mismatched numeric id fails silently. The client
  resolves each metatile credit id through the registry and drops the
  attribution when the lookup misses, with no warning and no symptom
  beyond an absent credit line.
- Whole-surface attribution — the common case, where every tile carries
  the same credits — pays for a per-tile bitmask it does not need, and
  cannot be read without fetching a metatile.

### Proposal

A `surface` resource also publishes its credits on its map
configuration surface entry, in the same string-keyed form
`boundlayer.json` uses. The metatile credit table stays for the case it
was designed for, per-tile attribution in server-merged surfaces, and
existing deployments are unaffected.

The client side already landed: cartolina-js `TerrainSource` accepts a
surface-level `credits` field, as an id list or a definition table, on
the same terms as `RasterSource`.

## BUG: surface generator emits a zero-submesh mesh

**Opened:** 2026-06-06, in the cartolina-js backlog; moved here 2026-07-11.
**Status:** deferred
**Related:** client workaround landed in cartolina-js
[draw-tiles.js](https://github.com/cartolinadev/cartolina-js/blob/main/src/core/map/draw-tiles.js)
(the `surfaceMesh.loadState == 2` early return in `drawSurfaceTile`)

### Symptom

A `surface-dem` resource can serve a tile whose metatile metanode has the
`geometry` flag set while the matching `.bin` mesh contains zero
submeshes. The mesh is structurally valid (the VTS header reports
`numSubmeshes = 0`), so it is not malformed in the format sense, but it
is inconsistent with the metatile that advertises geometry.

Observed on a global sparse DEM surface whose tile index was extended
upward with `vts complete-tileindex-up`.
The melown2015 root splits at lod 1 into three division-node subtrees
(pseudomerc, north UPS, south UPS). The north-UPS root tile `1-0-1` is
flagged with geometry and real height extents in the metatile, but its
mesh decodes to 14 bytes — a valid header with zero submeshes.

### Root cause

The metatile geometry flag comes from the tile index and the metatile's
own 8×8 per-tile sampling
(`metatileSamplesPerTile = 1<<3`), which finds valid heights for the
node. The mesh is built separately at 128×128 via
`Operation::demOptimal`, and the delivery path adds a submesh only when
the sampled local mesh has vertices:

`mapproxy/src/mapproxy/generator/surface.cpp` (`SurfaceBase::generateMesh`)

```cpp
vts::Mesh mesh(false);
if (!lm.mesh.vertices.empty()) {
    auto &sm(addSubMesh(mesh, lm.mesh, nodeInfo, lm.geoidGrid, textureMode));
    ...
}
```

When that warp yields no vertices the output mesh keeps zero submeshes.
The two products (metatile, mesh) sample the dataset differently and can
disagree about coverage for a coarse node, producing the mismatch.

(A raw 128×128 warp of `1-0-1` actually returns ~22.8% valid pixels, so
the node is not even genuinely empty — the empty mesh is itself
questionable, separate from the consistency issue.)

### Impact

A geometry-flagged tile backed by a zero-submesh mesh can never become
render-ready on the client, because the mesh has nothing to draw. The
legacy topdown traversal descends the root only when every division-node
sibling is ready, so one such sibling stalls the whole surface (blank
globe). Worked around client-side for the legacy path; the recursive
path tolerates it.

### Suggested direction

A surface generator should always emit exactly one submesh. The cleanest
fix is in `SurfaceBase::generateMesh`: add the submesh unconditionally
(an empty submesh with zero faces is acceptable), or otherwise reconcile
the metatile `geometry` flag with the actual mesh content so the two are
never inconsistent. Empty submeshes need a sane bounding box — an empty
`extents()` returns inverted/sentinel values — and the coverage-mask,
normal-map, and watertight/multimesh paths must tolerate the empty case.

### Relevant files

| File | Note |
|---|---|
| `mapproxy/src/mapproxy/generator/surface.cpp` | `SurfaceBase::generateMesh` — submesh added only if `!lm.mesh.vertices.empty()` |
| `mapproxy/src/mapproxy/generator/surface-dem.cpp` | `generateMeshImpl` — 128×128 `demOptimal` warp |
| `mapproxy/src/mapproxy/generator/metatile.cpp` | metatile geometry flag from index + 8×8 sampling |
| `mapproxy/src/mapproxy/support/mesh.cpp` | `addSubMesh`, `meshFromNode` |

## Navtiles on partial-coverage tiles are served without coverage

**Opened:** 2026-07-10
**Status:** open — decide whether partial-coverage tiles should carry a
navtile at all, and what their uncovered texels should hold.
**Related:** the cartolina-js backlog entry "coverage-aware point terrain
queries"; client mitigation landed there 2026-07-10 (a height answer must
lie within the claiming metanode's geometry bbox).

A navtile pane spans its tile's whole extent, but nothing delivered to the
client says which texels are backed by data:

- The tiling pass advertises the navtile flag on every emitted tile whose
  sampling is coarser than the source, watertight or not
  (`src/tiling/unified.cpp`, `emit()`: `trueScale(center, samplePx) < 1.0`
  sets `TiFlag::navtile` independently of the watertight bit).
- The stored navtile format does carry a quadtree coverage mask
  (`vts-libs/vts/navtile.hpp`, `NavTile::coverageMask`), and
  `SurfaceDem::generateNavtile` fills it faithfully from the warped DEM's
  validity. The delivered flavor strips it: `serializeNavtileProper()`
  writes the height image only (`vts-libs/vts/navtile.cpp`).
- Texels outside coverage are never written by the sampling loop in
  `SurfaceDem::generateNavtile`, and `opencv::NavTile::createData()`
  allocates the image uninitialized — uncovered texels of a delivered
  partial-coverage navtile hold undefined values.

A height query that samples such a pane outside the covered part reads
filler it cannot distinguish from terrain. The client now rejects samples
far from the metanode's geometry bbox, but that heuristic accepts filler
whenever the geometry's bbox spans the tile (diagonal coverage).

Options, not mutually exclusive:

- Do not set `TiFlag::navtile` on non-watertight tiles. Simple; matches
  the navtile's navigation purpose (a back surface or a coarser ancestor
  answers instead). Costs navtile resolution near dataset fringes.
- Define the uncovered texels: extend the covered values outward so
  near-edge samples decode to something anchored to real data, and the
  pane is at least deterministic.
- Deliver the coverage mask (serve the raw flavor or a v6-era format
  extension) and teach the client to honor it. The correct fix, and the
  most work; needs a frontend/backend contract, so an RFC in cartolina-js.

## Per-surface metatile packaging, if ever needed, goes in the store header

**Opened:** 2026-07-05
**Status:** open, no action until the client shallow-subtree milestone —
non-default packaging is unservable today, so there is nothing to configure.

The surface-definition `metaBinaryOrder`/`metaDepth` options were removed
(2026-07-05): they were a serve-time resource-definition input for a value
fixed at tiling time, could only ever be `(5, 1)` or throw, and reached
nothing but the mapConfig advertisement. See the RFC 7 addendum
(2026-07-05). When shallow-subtree delivery makes per-surface packaging
real, drive it from a **tiling-time parameter stamped into the store
header** — the header already carries `metaBinaryOrder`/`metaDepth` — with
serving and mapConfig sourced from that header. Do not resurrect the
resource-definition field: the store, not the config, is the authority.

## Leaf-lod watertightness is blind to the rf partition boundary

**Opened:** 2026-07-05
**Status:** open, low priority — only bites datasets whose footprint stops
exactly at a partition boundary (e.g. a polar dataset cut at the ±85.05°
circle); global and interior datasets are unaffected.

The 2026-07-05 fix made the unified pass's parent watertight inference
honor the reference frame's partitioning; the *leaf* test still does not.
A leaf tile's watertightness is `maskMin == 255` from the
one-pixel-per-tile min filter over the full physical cell, so an
rf-partial leaf whose data ends exactly at the constraint reads
non-watertight even though it is complete over its valid area — and that
veto then propagates up through the (now correct) parent inference. The
legacy per-tile walk got this right by comparing the warped mask against
the node's constraint coverage (`NodeInfo::checkMask`); the unified pass
has no sub-tile mask resolution to do the same. Fixing it means either a
constraint-aware leaf pass for rf-partial cells (a second, clipped mask
warp, or per-cell `checkMask` on the boundary strip) or accepting the
conservatism. Do not bolt validity into the world tile ranges instead —
existence and watertightness are different questions.

## REDESIGN: retire the in-tree MVT driver in favor of GDAL's upstream driver

**Opened:** 2026-07-04
**Status:** deferred, low priority — the cross-tile staging race it
enabled is fixed at the generator level (`mvt:` prefix, 2026-07-04);
nothing is currently broken. Revisit when touching the driver or the
heightcoding pipeline anyway.

The in-tree MVT driver (`src/gdal-drivers/mvt.cpp`) predates GDAL's
built-in MVT driver (GDAL ≥ 2.3, 2018) and now survives on inertia, not
on merit. Its weaknesses, plainly:

- it reads through `std::ifstream`, not VSI, so it cannot open
  `/vsimem/` or `/vsicurl/` paths — this is what forced GDAL's HTTP
  driver into the shared-basename temp-file fallback behind the
  wrong-tile race;
- its `Identify` claims *every* file unconditionally, so its `Open` is
  probed for every vector open in the process and rejection relies on
  protobuf parse failure — and an empty/unreadable input parses as a
  valid empty tile;
- its geometry decoding is ad-hoc (clockwise-ring heuristics) rather
  than MVT-spec 2.1 winding rules;
- registration deregisters the upstream driver by name, so the process
  silently loses upstream's MBTiles/directory/metadata.json support.

What keeps it alive: the `MVT_SRS` and `MVT_EXTENTS` open options that
place a single tile into the node's SDS SRS and extents. This matters
less than it looks. A tiled source must share the reference frame's
subtree tiling, and the MVT producer ecosystem is WebMercator-locked,
so tiled-MVT resources live in the pseudomerc subtree where the SDS
*is* EPSG:3857 — upstream's assumption. Upstream's single-tile options
(`GEOREF_TOPX/TOPY/TILEDIMX/TILEDIMY`) cover the extents. The one real
delta to verify: `MVT_SRS` receives `sds(nodeInfo, geoidGrid)`, i.e.
3857 with the geoid grid baked into the SRS string, and the
heightcoding worker reads the layer SRS back off the dataset for
vertical adjustment; upstream reports plain 3857. The pipeline already
has an SRS-independent path for this (`vectorGeoidGrid` →
`config.vectorDsSrs`), so the migration is confirming that path fully
replaces the tag, after which the in-tree driver can go and remote
fetch becomes upstream's native `MVT:/vsicurl/…` path.

## REDESIGN (tileserver): package the flag index and height sidecar as one file

**Opened:** 2026-07-03
**Status:** deferred — an operator simplification, not a correctness or
runtime need. Revisit only when managing paired publication causes real
problems or another format change makes the migration cheap.

A DEM currently publishes two paired artifacts: the compact flag tile
index and its height sidecar. This contract is coherent and can remain
indefinitely, but one container could make it simpler for operators:

- one atomically replaced `tiling.<rf>` file;
- the existing compact flag qtrees as one section;
- optional height pages as another section (absent for coverage-only
  imagery indexes); and
- no cross-file pairing, mixed-generation state, or two-file rollback.

The two sections must keep their current independent encodings. Flags
collapse extremely well and need cheap random lookup; height ranges do
not. Combining flags and heights into one quadtree node value would
destroy that useful separation. Resource-specific `delivery.index` and
`tileset.index` files remain derived caches.

This is not the required destination of the height-sidecar cleanup. It
does not improve serving correctness or performance, and the current
pairing already detects interrupted publication. Prefer the existing
two-file design until operational evidence justifies a container and
its migration path.

## REDESIGN: make the metanode store a pure height sidecar

**Opened:** 2026-07-03
**Status:** stage 1 (semantic scrub, format-neutral) implemented
2026-07-03; stage 2 (drop the reserved byte) is deferred until an
actual store-format change. Do not bump the format for this alone.

RFC 7 deliberately stored mesh and watertight state alongside heights
while retaining the existing flag tile index. Review identified the
resulting two-authority risk and introduced pairing. The implementation
then continued to source all delivered flags and child existence from
the paired index, leaving the store byte unused by serving from the day
it landed. `--reflag` later repurposed the distinction as raw coverage
versus delivery policy, but that marginal feature did not justify the
ongoing duplicate semantics.

The redundancy, spelled out:

- The serve path never reads partial/full. It derives existence and
  child flags from the index (`validSubtree`) and uses the store
  strictly as a height lookup for nodes the index already decided to
  serve (`generator/metatile-store.cpp`). For everything delivery
  does, the byte is dead weight.
- The only functional reader of the distinction is `--reflag`
  (`tiling/unified.cpp`). The policy flips it buys (skipPartial either
  way, retro-prune) are one re-tile away on the unified pass: ~1 h for
  a planet, minutes for a regional dataset.
- The other reader, `mapproxy-mnstore check`, has never been run in
  production. Not once.

The result: what should have been a clean two-artifact contract —
index owns flags, store owns heights — needs a page of explanation,
and every reader of `mnstore.hpp` gets to wonder which of the two
truths wins.

The fix decomposes into a semantic scrub (a code change,
format-neutral) and a layout cleanup (a real format change); only the
second waits for a forced break.

Stage 1 — semantic scrub, no format change (implemented 2026-07-03):

- `NodeData::Coverage` is deleted from the code. The byte stays in
  the v2 layout as a reserved field (see the `ReservedByte` comment in
  `support/mnstore.cpp` for why it must stay nonzero). Post-scrub
  readers take presence from the page codec's empty/uniform/internal
  tags, where it structurally lives.
- The store's node set is exactly the served set: payload is emitted
  iff the node is index-reachable (own flags, or a descendant's — the
  structural-node case under `skipPartial`). One reachability bit
  propagated up the mip ascent; a suppressed leaf with nothing below
  it is not written. The stored semantics become one sentence: *for
  every node the index serves, the source height range over its
  cell.* Old and new servers can serve both pre-scrub and post-scrub
  v2 stores; old stores' extra nodes are inert because traversal is
  index-gated. Post-scrub stores do not preserve the deleted old-tool
  `--reflag` semantics. No re-tiles are required for serving.
- `--reflag` is deleted: code, CLI, and its operator-guide section.
  Changing `skipPartial` or prune means re-tiling (~1 h for a planet).
- `mapproxy-mnstore check` verifies the pair instead: store-payload ↔
  index-reachability set agreement and the aggregation invariant
  (parent range contains children's). It validates the post-scrub
  node-set contract; a pre-scrub `--skipPartial` store can report its
  now-inert surplus payload. Stronger than the coverage check, and it
  needs no byte.
- The sidecar invariant is stated where people read:
  `support/mnstore.hpp`, [tile-index.md](tile-index.md), the
  [operator guide](metanode-store-operations.md), and the RFC 7
  implementation notes (amended in place; deviation 11's framing is
  superseded).

Stage 2 — layout cleanup, deferred until an actual store-format change:

- Node payload shrinks to `{minZ, maxZ}` — 4 bytes; the reserved byte
  is dropped and the pre-scrub compatibility concern expires with the
  version bump. The shallow-subtree v7 wire-format work does not itself
  force a store-format change. The format is a contract with production
  data: hygiene does not drive a version bump.

## P1 CORRECTNESS: prune must not split child sets

**Opened:** 2026-07-02
**Status:** implemented 2026-07-02 (complete child-set pruning during
generation; `mapproxy-mnstore check` validation).

Pruning keeps or removes all siblings together;
`mapproxy-mnstore check` finds stores produced by the old per-tile rule.

## P1 CORRECTNESS: `skipPartial` must remove geometry-less leaves

**Opened:** 2026-07-02
**Status:** resolved 2026-07-02 — the premise did not hold; the adjacent
real defect (degenerate structural-node extents) is fixed instead.

The premise was that suppressed tiles survive in the delivery hierarchy as
flagged-but-meshless entries, so a branch with no geometry below ends in a
geometry-less leaf the client can reach. Neither half holds:

- A tile index entry has no existence bit separate from its flags.
  `skipPartial` zeroes the whole entry, and a zero entry is byte-identical
  to a tile that never existed. Every nonzero entry carries `mesh`.
- The client learns of children only through metatile child flags, and both
  serve paths compute those per request as `validSubtree(child)` — "any
  nonzero entry anywhere below this child". Because nonzero implies mesh,
  every advertised branch terminates in geometry, and an all-zero subtree is
  never advertised. That is exactly the bottom-up closure this entry asked
  for, derived at serve time; the published pair needs none materialized.
  The mechanism is documented in [tile-index.md](tile-index.md).

The concern did brush against a real defect: the store serve path emitted
structural metanodes (zero flags, store payload present — i.e. suppressed
partial tiles on branches leading to deeper geometry) with default
`geomExtents` (±inf/NaN heights). cartolina-js builds its culling volume
from the division-node span plus exactly those heights, so correct
rendering depended on NaN comparisons failing open — one client refactor
away from culling all boundary detail. The warp path was never affected (it
fills extents from DEM samples independently of index flags). Fixed in
`generator/metatile-store.cpp`: any payload-bearing node now serves its
stored coverage envelope, which the mip ascent built as an aggregate over
the tile's full coverage — a conservative superset of every descendant mesh
envelope.

## CORRECTNESS (tileserver): the store serve path must not depend on warp fallback

**Opened:** 2026-07-02
**Status:** resolved 2026-07-05 — an adopted store is now the sole
metatile source; per-request warp fallback is gone.

`metatileFromStore` (`generator/metatile-store.cpp`) returns `boost::none`
— falling back to the serve-time DEM warp — whenever a tile the *served*
(delivery) index marks real (`geometry || navtile`) has no payload in the
metanode store. Today that is treated as a safe degradation. It is not, for
much longer: new production DEM resources build normal-only (no
`dem.min`/`dem.max`, RFC 7 §7.1), and old ones have those pyramids removed
after migration, so the warp path will be **unavailable**. Once the legacy
path is sunset, any store-path fallback is a hard failure (500 / missing
metatile), not a slower answer. The store must serve every metatile its
paired index can declare.

Concrete gap observed 2026-07-02: `prepareTileIndex` force-adds `navtile`
to the whole configured `tileRange` at `lodRange.min` (its synthetic index),
so a coarse tile with no coverage — hence no store node — can still be
marked real in the served index. Its metatile then falls back to warp
(`store page L-x-y has no payload for real tile ...`). On a normal-only
resource that is a 500. The same happens for any tile the synthetic index
marks real but the tiling/store omits.

Directions (pick during design): make the tiling/store cover every tile the
served index can mark real (e.g. carry navtile-only nodes with a height
range in the store), or stop `prepareTileIndex` from marking a tile real
where the paired store has no node, or let `metatileFromStore` synthesise a
navtile-only metanode without a store read. Whichever: the invariant is that
a valid store + index pair serves every metatile in range with no warp.

Resolution: the later height-sidecar semantic scrub made the store carry
payload for every node the paired flag index reaches, including structural
ancestors. `prepareTileIndex` intersects its synthetic range with that flag
index, so synthetic-only mesh/navtile entries do not survive into delivery;
the original coarse-metatile reproduction now serves from the store. The
remaining unsafe mechanism was the optional return from `metatileFromStore`:
a missing page, missing reachable-node payload, or derived-field exception
could still divert an already adopted store to the legacy warp path.

`metatileFromStore` now returns a metatile or raises an error. Surface DEM
and both tiled-geodata callers return its result directly. Legacy warp is
selected only at resource preparation, when no valid store is adopted and
both `dem.min` and `dem.max` are available. A missing payload is a broken
paired artifact and fails visibly; keeping legacy pyramids cannot hide it.

## DOCS: audit and update `resources.md`

**Opened:** 2026-06-30
**Status:** resolved 2026-07-05

[resources.md](resources.md) is an older reference and may lag the current
resource parsers, registered drivers, defaults, and runtime behavior. Review
the complete document against the implementation and make it accurate.

The audit includes:

- documenting the production TMS generators added since 2023:
  `tms-gdaldem`, `tms-normalmap`, and `tms-specularmap`;
- verifying every documented field, type, default, constraint, and accepted
  JSON shape against the parser and definition type;
- correcting stale descriptions and examples;
- removing unsupported claims or identifying retained legacy behavior
  explicitly; and
- improving organization where the current structure obscures the resource
  model.

Treat the implementation under `mapproxy/src/mapproxy/definition/` as the
source of truth. Preserve useful operational guidance and links while fixing
technical inaccuracies.

Resolution:

The established resource sections were checked against their parsers and the
three post-2023 TMS generators were added. Obsolete and experimental drivers
were deliberately not promoted into the reference. Examples now use accepted
configuration shapes, and the document no longer carries a general warning
that it is incomplete or outdated.

## TOOLS: background-color keying does not reach the VRTWO mask band

`generatevrtwo --background <color>` (mapproxy-setup-resource imagery
path) is documented as keying out tiles that consist entirely of the
background color. That keying currently affects only **overview tile
emptiness**, not the base-resolution GDAL mask band: the `dataset`
VRT's `GetMaskBand` still reports `255` (valid) over a solid
background region. There is a standing `// TODO` at
`generatevrtwo.cpp:697` ("we are using a background color: need to
check content for ...").

Discovered while verifying the RFC 7 imagery coverage-tiling mode
(unified mask pass for orthophoto, store-less). Both the legacy tiling
and the new coverage pass read the same VRTWO mask, so neither
excludes background-keyed regions from the flag tile index — they
agree, and the behavior is unchanged by the coverage work. It is
harmless today because tms-raster applies background transparency
per-pixel at tile-generation time, independent of the coverage index.

It would matter only if coverage-driven existence is ever expected to
honor background keying (e.g. to skip generating wholly-background
tiles). The fix is to populate the per-dataset mask band from the same
block/background comparison `generatevrtwo` already runs for overview
emptiness, so `GetMaskBand` reflects keyed-out regions at base
resolution. Verified with a synthetic black-background RGB cut: VRTWO
mask = 255 over the keyed region.

## PERF (tileserver): generatevrtwo wrap halo scales as 3·2^levels

**Opened:** 2026-06-13
**Status:** open; needs a per-level wrap design.

generatevrtwo's x-wrap padding scales with the overview count, not the
seam width. mapproxy-calipers reports an *engaged* `wrapx` for any
x-periodic source whose extent reaches ±180° — value 0 when the seam is
exact, since both overhangs are 0. setup-resource forwards it
(`config.wrapx = cm.xOverlap`), and generatevrtwo gates on
`if (!config.wrapx)`: an engaged optional(0) is truthy, so it enters the
wrap branch and pads the base by `xPlus = 3·2^(overview levels)` px per
side regardless of the overlap value (the 0 only zeroes the sampling
shift).

The intent is sound — give the coarsest overview 3 px of lanczos wrap
context — but projecting that need down to base resolution makes the
halo grow with pyramid depth. A global source with N overviews gains
3·2^N px of halo per side; on a deep pyramid this exceeds the data width
itself, and the padded base and its whole overview pyramid are then
stored and processed at the inflated width.

Worked example: a seamless global source at 3 arc-sec (~432000 px wide)
with 17 overviews gains 6·2^17 = 786432 px of padding — more than the
data — to give a ~10 px top overview its 3 px margin, leaving the stored
result ~2.8× the necessary size. Any source that runs through
generatevrtwo with wrap enabled is affected; a source served as a plain
VRT without generatevrtwo is not.

A side effect: re-running calipers on a generatevrtwo output re-reads the
baked halo as a large `wrapx`. That value is an artifact, not a
mapproxy-tiling input, and must never be fed back into another
generatevrtwo run, or the halo doubles.

Idea: build the wrap halo per overview level — each level wraps 3 px from
its own opposite edge — instead of padding the base by 3·2^levels, so the
base carries only the actual seam overlap (often 0) plus a small fixed
margin. Alternatively cap the padded levels, accepting a non-wrapped
margin on the few coarsest overviews where 3 px already spans a large
distance.

## TOOLS (tileserver): per-node bottom lod for mapproxy-tiling

**Opened:** 2026-06-13
**Status:** implemented 2026-07-02, subsumed by the spatial prune.

`mapproxy-tiling` now measures the ranges itself (the merged
`mapproxy-calipers`) and, with `--prune`, caps each division node's leaf
at `node.id.lod + maxFloorDepth` — the deepest LOD any tile of the node
survives the prune. That is exactly the per-node bottom lod this item
asked for (and finer, since the prune also varies within the node). No
separate per-node CLI slot was needed.

`mapproxy-tiling` takes a single `--lodRange`, so every spatial-division
node descends to `--lodRange.max` regardless of its own native
resolution. The leaf is global: `leafLod = lodRange_.max` for all nodes
(`unified.cpp` `prepareNode`), with only the floor varying per node
(`max(lodRange_.min, node.id.lod)`).

`mapproxy-calipers` already computes the per-node bottom lod — it prints
it as the first token of each `range<SRS>:` line — but the tiling
command line has no slot for it. The second token (`LOD/tileRange`)
becomes one `--tileRange`, and its LOD prefix is consumed only as a
footprint anchor for rescaling during descent, not as a depth limit. See the
[metanode-store operator guide](metanode-store-operations.md#manual-creation),
"Manual creation." The per-node bottom lod is therefore discarded, and nodes
whose native resolution tops out shallower are tiled and stored one or more
lods past their useful resolution. Concrete case: a melown2015 dataset
where calipers gives `pseudomerc` lod 15 but the polar `steres`/`steren`
nodes lod 14 — with `--lodRange 1,15` the poles still get a lod-15 leaf.


Idea: let each `--tileRange` entry's leading LOD (or a separate
`--nodeLodRange`-style option) cap that node's descent, defaulting to
`--lodRange.max` when absent, and have calipers/setup-resource fill it
from the per-node bottom lod. Implementation is a per-node `leafLod` in
the unified pass instead of the single `lodRange_.max`.

Relation to [spatially varying bottom lod](#perf-tileserver-spatially-varying-bottom-lod-prune-subtrees-beyond-source-resolution): this is the
coarse, uniform-per-node version — one integer per node, no per-tile
signal — and it lands the high-latitude savings directly from data
calipers already produces. The spatial prune is the finer, latitude-
varying refinement; per-node bottom lod is a strict subset of it and
could be a stepping stone or be subsumed once the spatial prune ships.
Same leaf-triangle-budget caveat applies, but only where a node's own
native-resolution leaf is later viewed close up.

## PERF (tileserver): spatially varying bottom lod — prune subtrees beyond source resolution

**Opened:** 2026-06-12
**Status:** implemented 2026-07-02 (`mapproxy-tiling --prune`, default on).

Shipped as `--prune` in the merged `mapproxy-tiling` (and
`mapproxy-setup-resource --prune`), tied to `--gsd`: a tile is dropped
once its subdivision passes the source resolution at its own location,
computed per tile from the node's projection area scale (the calipers
depth formula per tile centre) rather than the `truescale` bit sketched
below. The target GSD is the complete resolution policy; operators can
set it finer than the DEM to leave room for draped imagery.
Changing the prune policy means re-running `mapproxy-tiling --apply`.
The leaf-triangle-budget caveat below still applies.

Pseudomercator's sec(lat) inflation means same-lod tiles cover ~11x
less ground at 85 deg than at the equator, so a global lodRange keeps
high-latitude subtrees descending several lods past the source's
native resolution. Both client and server then traverse, request, and
generate tiles that add no terrain information (interpolated meshes,
upsampled normals) — wasted bandwidth and cycles on both ends.

Idea: prune the tile tree spatially during tiling — stop emitting
children once per-tile sampling reaches the source resolution. The
RFC 7 unified pass already computes the signal per tile (the
`truescale` measure driving the navtile bit); the prune is a cutoff in
the emission loop, and the metatile tree, tile index and store stay
consistent by construction. Clients handle spatially varying leaf
depth the same way they handle today's lodRange bottom.

Design refinement: the bound-layer headroom is *relative*, not
absolute. Draped imagery is textured per surface tile id, so an
orthophoto finer than the DEM needs surface tiles past terrain-native
resolution — but the imagery's tiles live on the same pseudomerc grid
and stretch by the same sec(lat) factor, so the needed margin is a
latitude-invariant resolution ratio. The operator expresses that ratio
through the target GSD: for imagery twice as fine as the DEM, use a
target GSD half the DEM's native GSD. The surface cannot infer what will
be draped on it, so setup must supply that target.

Remaining caveat: **leaf triangle budget** — mesh simplification
budgets faces per tile, so a native-resolution leaf stretched over a
close-up view renders coarser geometry than today's re-meshed
interpolated children. May need a larger face budget on pruned
leaves.

## PERF (tileserver): pool unified-pass warps across division nodes

**Opened:** 2026-06-12
**Status:** implemented 2026-06-12 — the earth-qsc planetary run
(62m56s, six faces serialized) met the entry's own decision
criterion. One pool over all (node, pass) warps, gated by
`--warpConcurrency` (default min(12, hardware threads)), elevation
passes scheduled first; reduce/emit stays sequential in node order,
so artifacts are bit-identical (verified on the sample and on the
earth-qsc planet). Measured 62m56s -> 53m38s at concurrency 6 on the
dev laptop, limited there by per-warp `NUM_THREADS=ALL_CPUS`
over-subscription rather than IO; capping per-warp threads when the
pool is wide is the follow-up if planetary cadence demands more.

The RFC 7 unified tiling pass runs its four filter passes (mask
min/max, elevation min/max) concurrently *within* one reference-frame
division node, but division nodes are processed sequentially. Treating
all `(division node, pass)` warps as one task pool with a small
concurrency cap (~6; the work is source-read/decompress bound, so more
would queue on IO) would overlap the node tails — a bounded ~15-20%
win on melown2015 (the pseudomerc node dominates, polar caps are
small), but potentially much more on **earth-qsc**, whose six
similar-sized QSC faces currently serialize. The refactor is
contained: split `processNode` (`mapproxy/src/tiling/unified.cpp`)
into a warp stage and a reduce/emit stage and gate the pool with a
semaphore; per-node grids would coexist, so mind peak memory on
planet-scale leaf grids (~0.7 GB per melown2015-sized node).

Decide after measuring the earth-qsc planetary tiling wall time; if
it is acceptably short, this stays deferred (premature-optimization
rule).

## PERF/REDESIGN: coverage-mask `mapproxy-tiling`

**Opened:** 2026-05-29
**Status:** implemented (2026-06-12) as part of RFC 7
([rfc-metanode-store.md](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md) §4); the unified pass
is the default `mapproxy-tiling` mode (legacy analysis behind
`--legacy`). §4.5 assumptions verified on the test sample; residuals
characterized in the RFC implementation notes. The notes here are
retained as the originating discussion.

### Goal

Replace the per-tile, per-LOD GDAL warp in `mapproxy-tiling` with a
single native-resolution coverage pass plus a bottom-up reduction.
The tile index produced must be identical in meaning to today's
output (existence, watertight, navtile flags).

### Background

See [tile-index.md](tile-index.md) for what the tile index carries and
how `mapproxy-tiling` produces it today, and
[tileserver-metatile-production.md](tileserver-metatile-production.md)
for the pipeline cost.

The current tool (`mapproxy/src/tiling/tiling.cpp`) warps a 129 × 129
sample grid per tile and descends the whole tree, classifying each tile
as whole / some / none. Its watertight seal engages only once the warp
reaches native resolution, so a fully-covered but downsampled region is
warped at every LOD down to the resolution floor. On a planet-scale
dataset this runs for days to weeks. The only output is a per-tile
flag bitmask; the warped raster is discarded.

This redesign also retires the watertight-under-broadening limitation
documented in [tile-index.md](tile-index.md): because truth is computed
at native resolution and reduced upward, there is no coarse watertight
value to over-trust.

### Basis: the GDAL mask band (RFC 15)

The mechanism rests on GDAL's per-band/per-dataset mask band, defined in
GDAL RFC 15 — "RFC 15: Band masks"
(<https://gdal.org/en/stable/development/rfc/rfc15_nodatabitmask.html>).
`GetMaskBand()` always returns a `UInt8` band where **0 means nodata and
255 means valid**, and GDAL **synthesizes** it when no explicit `.msk`
file exists:

- `GMF_NODATA` — generated on the fly from the source's nodata value;
- `GMF_ALPHA` — the alpha band, which may hold values other than 0/255;
- `GMF_ALL_VALID` — an all-255 fallback when the source declares no
  nodata.

So the data-availability layer is not something this tool derives — it
is the mask band GDAL already produces. This is the entire basis of the
existence / watertight test: warp the **mask band** (not the elevation),
reduce min/max per output cell, and

- `max > 0` ⇒ at least one valid source pixel ⇒ the tile **exists**;
- `min > 0` ⇒ every source pixel valid ⇒ the tile is **watertight**.

For a binary mask (`GMF_NODATA` or `GMF_ALL_VALID`) the values are
strictly 0 or 255, so `min > 0` is identical to `min == 255` — exactly
"fully covered." A gap-free source yields `GMF_ALL_VALID`, i.e. 255
everywhere, so existence and watertight fall out with no scan of data
values at all.

**Design rule — warp the mask band with no nodata.** Do not pass
`-srcnodata` and do not set a nodata value on the mask band being
warped. A mask band has no *invalid* pixels — 0 and 255 are both valid
mask *values* — so by default the warper excludes nothing and min/max
see every pixel, including the 0s that signal holes. GDAL only excludes
source pixels when told to, via `-srcnodata`, a band nodata value, or
the band's own mask (which for a mask band is all-valid). Declaring 0 as
nodata would make the warper drop exactly the hole pixels and report
false watertight. The rule is simply not to do that.

### Proposed algorithm

1. Take the source **mask band** (`GetMaskBand`, RFC 15). No manual
   0/1 derivation, no nodata bookkeeping — the mask band is the dense
   availability raster by construction.
2. Per reference-frame division node, warp that band into the node grid
   at the resolution floor (the native-resolution LOD, which calipers
   already computes from source GSD).
3. Reduce two statistics per output cell during the warp, using GDAL's
   min/max resampling (`GRA_Min` / `GRA_Max`):
   - `max` over the cell → existence (any source pixel present);
   - `min` over the cell → watertight (all source pixels present).
   This can be one warp at sub-tile sampling reduced in code, or two
   warps (one extra source read, still far cheaper than the current
   tool). The destination is initialised to 0 so cells outside the
   source extent reduce to not-existing / not-watertight.
4. Build coarser LODs bottom-up with pure bit operations, no further
   sampling:
   - existence: `parent = OR(children)`;
   - watertight: `parent = AND(children)`.
   up to the root.
5. AND in reference-frame node validity separately (the deliberate
   fake-watertight in invalid areas). Positional flags — `navtile` at
   the analysis minimum, `atlas` rules — are set by position, not by
   sampling.

### Why it is faster

Every source pixel is read and resampled **once**, instead of being
re-resampled at each pyramid level plus overview construction. That is
the `O(levels × area)` → `O(area)` collapse where the current runtime
goes. The coarser-LOD reduction touches no source data at all.

### Parallelism

Use CPU parallelism wherever available; the work is well suited to it.

- **GDAL multi-threaded warping.** The native-resolution warp is the
  dominant cost and GDAL can multi-thread a single warp across blocks
  (`gdalwarp -multi`, warp option `NUM_THREADS=ALL_CPUS`, or the
  equivalent `GDALWarpOptions`). Enable it.
- **Across reference-frame nodes.** The per-node warps are independent
  and can run concurrently.
- **Block reduction.** The streamed blocks of the native-resolution
  mask, and the bottom-up OR/AND reduction over quadrants, are
  embarrassingly parallel; a parallel block pipeline overlaps warp I/O
  with reduction.

The current tool already parallelises its per-tile descent with OpenMP
(`mapproxy/src/tiling/tiling.cpp` lines 178-183); the redesign should
keep at least that level of CPU utilisation while removing the redundant
work. If GDAL's own threading covers the warp, additional task
parallelism need only cover the reduction and the per-node fan-out —
confirm the two layers do not oversubscribe cores.

### Assumptions to test before committing

These are the load-bearing claims; the RFC should verify each
empirically (e.g. `gdalwarp -r min` / `-r max` on a small DEM tile,
diffed against the current tool's flags for the same extent):

- **GDAL min/max resampling aggregates over the full destination
  footprint** for a downsampling warp, not a subsample. Needs
  confirmation at extreme downsample ratios.
- **Boundary / straddle semantics**: whether a source pixel straddling
  a tile edge is counted by overlap or by centre. This affects
  watertight exactly at tile edges. Verify against a hand-reduced
  reference.
- **Alpha masks**: for `GMF_ALPHA` sources the mask may hold values
  between 0 and 255, so `min > 0` no longer equals "fully valid." Such
  sources need a threshold (e.g. `min == 255`) or explicit handling.
  DEMs are typically `GMF_NODATA` / `GMF_ALL_VALID`, where this does not
  arise.
- **Read-once floor**: 1 px/tile output does not reduce source reads
  (the warper still scans every source pixel); the saving over a
  high-resolution mask is intermediate size and memory, not source I/O.
  The saving over the current tool — reading the source once instead of
  per level — is the real win and is unaffected.
- **Empty-region pruning**: the current descent skips empty areas
  (ocean) cheaply. A full-extent native pass must recover this, e.g.
  bound by the source footprint and/or a coarse existence pre-pass, or
  it will process empty area it does not need to.

### Relation to other items

This shares the data dependency and output format of **PERF: pre-built
metatile index** (this file). The bottom-up reduction can carry per-node
height-range min/max in the same pass — the VRTWO min/max pyramids are
the input either way — producing the extended index that item needs.
Sequencing of the two is open.

### Open questions

- Whether GDAL's stock min/max resampling is trustworthy enough or a
  custom warp kernel (emitting both stats in one pass) is warranted.
- Streaming strategy: the native-resolution coverage band for a planet
  cannot be materialised whole; it must be processed in blocks reduced
  into the pyramid, as overview construction already does.
- Output format: whether to keep the current QTree format or move to
  the extended per-node format from the pre-built metatile index item.

---

## PERF: pre-built metatile index eliminating serve-time DEM warps

**Opened:** 2026-05-16
**Status:** implemented (2026-06-12) — RFC 7
([rfc-metanode-store.md](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md)) landed on
`feature/metanode-store`; see its implementation notes for results
(store-served metatiles ~25 ms vs ~700 ms warp on the test sample) and
deviations. The notes here are retained as the originating discussion.

### Goal

Eliminate the GDAL DEM warp from the metatile request path by
pre-computing all metatile data at resource setup time and serving
it from a flat lookup.

### Background

See [tileserver-metatile-production.md](tileserver-metatile-production.md)
for a full description of the current pipeline.

The short version: each metatile request triggers a GDAL warp of
the VRTWO (the virtual dataset with min/max-filtered overviews).
This costs 100–500 ms per request on a warm server. The VRTWO and
the tile index together already contain all the information a
metatile carries — tile existence, watertight flags, and height
ranges. The per-request warp re-derives that information instead
of reading it from a pre-built store.

The serve-time warp is separate from the client-side ping-pong
problem (sequential metatile round-trips before geometry loading
starts). Eliminating the warp reduces per-request latency; a
manifest endpoint (a possible later stage) would reduce round-trip
count. Both improvements are independent.

### Proposal

Extend the tile index format to carry per-node height range data,
and extend `mapproxy-tiling` to populate it during the same walk
it already does. The VRTWO min/max pyramids are already the input
to the tiling step; sampling height range min/max per node adds
one read from an already-open dataset. No separate pass is needed.

At serve time, the metatile handler reads the extended tile index
and serialises the result directly. No GDAL warp occurs.

**CDN compatibility is preserved.** Metatile URLs remain keyed on
tile ID and are stable. The only change is that the origin server
answers cold misses in milliseconds instead of hundreds of
milliseconds.

### Extended tile index format

The extended index must carry, per tile node:

| Field | Source at generation time |
|---|---|
| Existence, child flags, watertight | Already in tile index (QTree) |
| Height range min/max | VRTWO min/max pyramids, read during tiling |
| Texel size | Analytical: LOD + reference frame resolution |
| SDS horizontal extents | Analytical: tile ID + division node |

The existing QTree binary format has no per-node payload beyond
flags. The new format must support per-node numeric fields. This
is a format version bump; backward compatibility requires the
server to detect which format is present and fall back to the
current on-the-fly warp path when only the old index exists.

### Relation to mapproxy-tiling redesign

`mapproxy-tiling` already takes days on large datasets due to
per-tile GDAL warps against the VRTWO. Extending it to also record
height ranges adds negligible cost to each node visit, since the
VRTWO is already open and the min/max values come from the same
sample grid the tool computes for coverage analysis.

A deeper redesign of `mapproxy-tiling` — addressing its overall
per-tile warp cost and serial bottlenecks — is a separate work
item, but it shares the same data dependency and the same output
format. A redesigned tool would produce the extended index
naturally.

### Staged rollout

1. **Pre-built metatile index** (this item). No client changes.
   Serve-time warp eliminated. CDN behaviour unchanged.

2. **Manifest endpoint** (deferred). A position-parameterised
   endpoint returning the full visible metatile tree in one
   response. This busts CDN (each position is a unique key) and
   is only viable if metatile generation is already fast — i.e.,
   after stage 1 is complete. Requires client changes to issue
   the manifest request at startup and fall back to per-tile
   fetches for incremental camera movement.

### Open questions

- **Extended index format.** Exact binary layout, versioning
  strategy, and whether the numeric payload section is
  mmap-friendly. The format should carry a version field so the
  server can detect old-format indexes and fall back to the
  current warp path during a rolling upgrade.
- **mapproxy-tiling redesign scope.** Extending the existing tool
  to write height ranges is low-risk. Whether a broader redesign
  of the tiling tool (addressing its overall per-tile warp cost)
  is done first, in parallel, or after is an open sequencing
  decision.

---
