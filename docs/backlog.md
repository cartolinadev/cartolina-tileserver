# Tileserver backlog

This backlog contains work confined to `cartolina-tileserver`. Tasks that
change frontend behavior, define a frontend/backend contract, or require an
RFC remain in the
[cartolina-js backlog](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/backlog.md).

The metanode-store and unified-tiling work in this backlog is specified by
[RFC 7](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md).

Entries are numbered in the heading (`## N. ...`) in the order they were
opened; the number is permanent and is never reused or renumbered. When
closing an entry (resolved, implemented, or superseded by another change),
move it to [backlog-archive.md](backlog-archive.md) — keep its number.

**New entries go directly below this line, newest first — never below an
existing entry, even one added earlier in the same session. Assign the next
entry the number one higher than the highest number used so far across this
file and [backlog-archive.md](backlog-archive.md).**

## 19. Publish surface credits in the map configuration

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

Implement the tileserver and client changes together. The client must apply
every surface-level credit while that surface is visible; registering its
definition without applying it is insufficient.

## 18. Navtiles on partial-coverage tiles are served without coverage

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

## 17. Per-surface metatile packaging, if ever needed, goes in the store header

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

## 16. Leaf-lod watertightness is blind to the rf partition boundary

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

## 15. REDESIGN: retire the in-tree MVT driver in favor of GDAL's upstream driver

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

## 14. REDESIGN (tileserver): package the flag index and height sidecar as one file

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

## 13. REDESIGN: make the metanode store a pure height sidecar

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

## 8. TOOLS: background-color keying does not reach the VRTWO mask band

**Opened:** 2026-06-30
**Status:** open

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

## 7. PERF (tileserver): generatevrtwo wrap halo scales as 3·2^levels

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

## 3. BUG: surface generator emits a zero-submesh mesh

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
