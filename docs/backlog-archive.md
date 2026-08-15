# Tileserver backlog archive

Backlog entries from [backlog.md](backlog.md) that are resolved, implemented,
or closed for another reason (superseded, subsumed by another change).
Entries keep the sequential number they were assigned in the active backlog;
numbers are not reused.

**Newly closed entries go directly below this line, newest first.**

## 22. `generatevrtwo` scales poorly beyond a few cores

**Opened:** 2026-08-14
**Status:** implemented 2026-08-15 — independent tile writes run
concurrently with GDAL 2.2 and newer.

On a many-core machine the tool leaves most cores idle. Two properties of
`createOverview`
([generatevrtwo.cpp](../mapproxy/src/generatevrtwo/generatevrtwo.cpp)) can
produce that, and it is not yet known which dominates:

- The GTiff write is serialized. When the mask type is not `band` — any
  input with an all-valid or nodata mask — `createOutputDataset` holds the
  `createOutputDataset` critical section across the whole of
  `GeoDataset::copy`, which is `GDALDriver::CreateCopy` and therefore the
  entire compression of the tile. Every worker contends on one lock for
  the compression of a full tile, while the warp that feeds it runs
  unlocked. The `band` branch below it holds the lock only for dataset
  creation and compresses outside it, so the fast path is the serialized
  one.
- Tile count bounds the parallelism per level. The unit of work is one
  tile of one overview, the level loop in `generate` is serial because
  each level warps from the previous one's VRT, and tile counts fall by
  four per level. Below the first level or two there are fewer tiles than
  cores, whatever the lock does.

The environment is the third possibility and the cheapest to eliminate:
there is no thread-count option, so libgomp sizes the pool from the
affinity mask, and a cgroup or `taskset` restriction produces the same
symptom directly.

A thread backtrace of a running job separates the first from the second —
workers parked in `GOMP_critical_name_start` against workers parked
nowhere. `Creating overview #N of M tiles` in the log gives the per-level
tile count. Take that evidence before changing anything.

**Resolution:** the critical section was the avoidable bottleneck. Its 2016
commit recorded no rationale, but GDAL 2.2 fixed the known block-cache
deadlock and thread-unsafe raster write paths in GDAL 2.1. Each overview
worker owns its source handle, in-memory warp target, and output filename,
so current GDAL's re-entrancy guarantee applies. The output critical
sections now compile only with GDAL older than 2.2; the short lock that
updates the shared overview VRT remains.

A complete pyramid regenerated with the unlocked path had byte-identical
GeoTIFF tiles and identical raster checksums at every overview level. An
explicit-mask regression also produced byte-identical tiles with one and
multiple workers. Coarse levels still have fewer tiles than cores, and the
OpenMP runtime still follows process affinity; neither is a correctness or
configuration bug.

## 7. PERF (tileserver): generatevrtwo wrap halo scales as 3·2^levels

**Opened:** 2026-06-13
**Status:** implemented 2026-08-13 — no dataset carries a wrap halo at
all; the wrap is supplied per warp. See resolution note below.

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

**Resolution:** neither idea recorded above was taken. Per-level pixel
padding cannot be expressed at all, since it implies per-level extents;
capping the padded levels would have left the same margin in the base.
Instead the halo is gone from the stored data and the wrap happens at
warp time: `geo::GeoDataset::warpInto` reads an x-periodic source
through a padded in-memory view whenever the destination reaches the
seam, which covers overview generation and request-time serving alike.
`generatevrtwo` writes every level at the extents of the input and
derives the periodicity from the raster, so the `--wrapx` option — and
with it the risk of feeding a baked halo back in as a large `wrapx` —
is gone.

## 20. TOOLS: pad the filter-pass source window in x only

**Opened:** 2026-08-13
**Status:** superseded 2026-08-13 — see resolution note below.

`filterPass` (`src/tiling/unified.cpp`) pads each warp chunk's source
window with GDAL's `SOURCE_EXTRA`, which applies to both axes. The strip
the padding exists to cover lies along the antimeridian, so only the x
axis needs it. On a wide source the padding is several hundred pixels and
a chunk is far wider than it is tall in source pixels, so the rows added
above and below dominate the extra reading and nothing uses them.

Removing the waste means computing the source window in `filterPass`
instead of asking GDAL for it — `GDALWarpOperation` with an explicit
chunk list rather than the `GDALWarp` utility call. Worth doing only if
tiling a wide source proves too slow in practice.

**Resolution:** the filter passes set no `SOURCE_EXTRA` at all; they warp
from a one-period view of a periodic source and GDAL's own antimeridian
handling keeps the seam inside every window (see the session log). With
the padding gone there is nothing left to narrow.

## 12. P1 CORRECTNESS: prune must not split child sets

**Opened:** 2026-07-02
**Status:** implemented 2026-07-02 (complete child-set pruning during
generation; `mapproxy-mnstore check` validation).

Pruning keeps or removes all siblings together;
`mapproxy-mnstore check` finds stores produced by the old per-tile rule.

## 11. P1 CORRECTNESS: `skipPartial` must remove geometry-less leaves

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

## 10. CORRECTNESS (tileserver): the store serve path must not depend on warp fallback

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

## 9. DOCS: audit and update `resources.md`

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

## 6. TOOLS (tileserver): per-node bottom lod for mapproxy-tiling

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

Relation to [4. PERF (tileserver): spatially varying bottom lod](#4-perf-tileserver-spatially-varying-bottom-lod--prune-subtrees-beyond-source-resolution):
this is the coarse, uniform-per-node version — one integer per node, no
per-tile signal — and it lands the high-latitude savings directly from data
calipers already produces. The spatial prune is the finer, latitude-
varying refinement; per-node bottom lod is a strict subset of it and
could be a stepping stone or be subsumed once the spatial prune ships.
Same leaf-triangle-budget caveat applies, but only where a node's own
native-resolution leaf is later viewed close up.

## 5. PERF (tileserver): spatially varying bottom lod — prune subtrees beyond source resolution

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

## 4. PERF (tileserver): pool unified-pass warps across division nodes

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

## 2. PERF/REDESIGN: coverage-mask `mapproxy-tiling`

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

This shares the data dependency and output format of **1. PERF: pre-built
metatile index**, below. The bottom-up reduction can carry per-node
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

## 1. PERF: pre-built metatile index eliminating serve-time DEM warps

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
