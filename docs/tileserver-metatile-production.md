# Tileserver metatile production

See [index.md](index.md) for the tileserver documentation contents.

This document describes how the cartolina-tileserver generates
surface-DEM metatiles and where the computational cost lies. Since
[RFC 7][rfc-7] (the metanode store, implemented
2026-06-12), the default DEM pipeline precomputes metatile payload
offline and serves it without a GDAL warp. The old serve-time-warp
model survives as a legacy fallback; this document covers both and is
explicit about which is which.

For the binary format and client-side usage of metatiles, see
[surface-metatile.md][surface-metatile]. For the operator HOWTO —
setup, migration, validation, rollback — see
[metanode-store-operations.md](metanode-store-operations.md).


## Two pipelines

A DEM resource is in one of two modes, decided at generation:

- **Metanode-store mode** (the default since RFC 7). `generatevrtwo`
  builds a single normal overview pyramid; a unified `mapproxy-tiling`
  pass precomputes per-tile flags and height ranges into a *metanode
  store* alongside the flag tile index; the server assembles metatiles
  by reading the store, with **no warp**.
- **Legacy warp mode** (`--legacyTiling`). `generatevrtwo` builds three
  pyramids (normal, min-filtered, max-filtered); `mapproxy-tiling`
  runs the old per-tile analysis; the server warps the VRTWO at request
  time to derive height ranges. This is the pre-RFC-7 model, kept for
  resources that intentionally use the warp path and as the
  rolling-upgrade fallback.

The sections below describe the metanode-store pipeline first, then the
legacy warp path it replaced and the structural reason the change was
worth making.


## Generation pipeline (metanode-store mode)

Resource setup has three stages; each depends on the previous output.
`mapproxy-setup-resource` chains them for the easy path (metanode-store
mode by default). The measurement and tiling stages are one tool,
`mapproxy-tiling`; VRTWO generation sits between them.

### 1. Survey (measurement)

`mapproxy-tiling`'s default dry run — the `mapproxy-calipers` library
folded into it — reads the source dataset and measures its geographic
extent, producing a `Measurement` carrying the optimal LOD range, the
per-division-node tile ranges, the target floor GSD, x-overlap for
datasets that wrap the antimeridian, and a suggested position. This
stage is fast; it does not read raster data values. `mapproxy-setup-resource`
calls the same `calipers::measure` library entry point.

### 2. VRTWO generation — `generatevrtwo`

Source: `mapproxy/src/generatevrtwo/generatevrtwo.cpp`.

`generatevrtwo` builds a virtual GDAL dataset with pre-computed
overview levels. The output directory contains:

- `dataset` — a VRT file that is the entry point for all
  subsequent reads.
- `original` — symlink to the original input dataset.
- One subdirectory per overview level, each containing tiled
  GeoTIFF files (`x-y.tif`) and a level-local VRT.

In metanode-store mode, the DEM gets **one** overview pyramid (the
normal one). The min-filtered and max-filtered pyramids of the legacy
path existed for exactly one purpose — supplying the height range to
the serve-time warp — and the unified tiling pass (step 3) computes
that range directly. Building one pyramid instead of three is a ~3× →
~1× cut on this multi-hour step. `mapproxy-setup-resource` builds the
three-pyramid layout only under `--legacyTiling`.

**Why this step exists.** GDAL cannot efficiently sample a raw
large-scale DEM at arbitrary LOD levels without pre-computed
overviews. The VRTWO provides overview tiles in a layout that
allows fast random access for any LOD and geographic region,
avoiding full-resolution reads for coarse tiles. Meshes are still
built from the normal pyramid (`demOptimal`) and navtiles from `dem`,
so the normal pyramid stays mandatory.

**Global datasets.** A dataset in an x-periodic SRS is stored at
exactly one period, and every level of the pyramid keeps the extents
of the input. `generatevrtwo` detects the periodicity from the raster
itself and crops any whole-column overlap (such as a duplicated
antimeridian column) to reach one period. Resampling across the
±180° seam needs source data from the opposite edge; libgeo supplies
it per warp, so no dataset carries a stored wrap margin. See
"Antimeridian handling" below.

**Cost.** For each overview level, every tile is warped from the
level above using GDAL `warpInto()`, parallelised by tile with
OpenMP. For a planet-scale DEM this step takes multiple hours: the
dominant cost is GDAL warping and writing compressed GeoTIFF files to
disk. Dropping to a single pyramid removes two thirds of that work.

### 3. Unified tiling pass — `mapproxy-tiling`

Source: `mapproxy/src/tiling/tiling.cpp`, `mapproxy/src/tiling/unified.cpp`.

The unified pass is what `mapproxy-tiling --apply` runs for a DEM
(`mapproxy-setup-resource --legacyTiling` selects the old per-tile
analysis instead). It produces, in one run, both:

- the flag **tile index** — the compact, policy-applied delivery view:
  mesh, watertight, navtile and reachable subtrees (see
  [tile-index.md][tile-index]); and
- the **metanode store** — a parallel, separate quadtree holding raw
  per-tile metadata: source coverage (`partial` or `full`) and min/max
  height, the artifact the serve path reads instead of warping.

**Method.** Per reference-frame division node, the pass runs **four
one-pixel-per-tile GDAL filter passes** at the analysis maximum LOD,
each reading the source at base resolution with overview selection
disabled (`-ovr NONE`):

1. **Mask max** (`max(cell) > 0` → tile exists);
2. **Mask min** (`min(cell) == 255` → tile watertight);
3. **Elevation min** (`minZ`);
4. **Elevation max** (`maxZ`).

One destination pixel per tile lets the GDAL warp kernel do the leaf
reduction; the tool does not materialise a sub-tile sample grid. The
nodata rule is opposite per band: mask passes warp with **no**
`srcnodata` (a 0 is a hole, not absence; destinations are
`INIT_DEST=0`), elevation passes set `srcnodata` so the int16 nodata
sentinel cannot poison the range. Coarser LODs are then built bottom-up
by an **in-tool 2×2 min/max mip loop** with no further source sampling
(existence = OR, watertight = AND, min, max), and the same ascent emits
both artifacts. The two are staged to temporary names, fsynced, replaced
sequentially, and bound by a **pairing digest**. Mapproxy rejects a mixed
generation if publication is interrupted or overlaps resource preparation.

The four passes call GDAL's `GDALWarp()` utility API (libgeo's
`warpInto` degenerated at the one-pixel-per-tile ratio and could
silently substitute an averaging overview, defeating the `-ovr NONE`
requirement). They run concurrently — pooled across all `(division
node, pass)` warps under `--warpConcurrency` — while reduction and
emission stay sequential in node order, so output is deterministic.

**Emission policy.** Two options shape which tiles reach the flag index
during the ascent. `--prune` (default on) drops tiles whose subdivision
passes the source resolution at their own location — the per-tile
version of the calipers depth measure, evaluated from the node's
projection area scale, so a projection's area inflation no longer
forces over-generation past the source. Pruning keeps or removes all
siblings together.
`--skipPartial` clears the mesh
flag on non-watertight tiles, sacrificing their valid partial content to
eliminate boundary cracks and renderer framebuffer switches. Pruning removes
the tile from both artifacts; `skipPartial` zeroes the flag-index entry
and keeps store payload only while the branch still leads to geometry
(the structural-node envelope below). Changing either policy means
re-tiling. `mapproxy-mnstore check` verifies a published pair (see the
operator guide).

**Cost.** Far below the legacy per-tile-per-LOD warp. On the 1.94 Gpx
test sample the pass runs in ~1 min vs ~14 min for legacy analysis;
the planetary melown2015 store builds in under an hour where the legacy
planetary tiling ran for days. The intermediate data volume drops by
`samplesPerTile²` (the old pass sampled a 129×129 grid per tile);
source I/O and warp-kernel work remain `O(source pixels)` per pass.

**The metanode store it emits.** A height sidecar of the flag index:
it carries, for every node the index serves, the source height range
over that node's cell — and nothing else. Paged and mmapped, with a
directory mapping each metatile root block `(lod, x, y)` to a page.
Each page encodes per-level local quadtrees with uniform-quadrant
collapse and a 5-byte node payload: one reserved byte (constant;
dropped at the next format version) plus a `half` `minZ`/`maxZ`, biased
outward (conservative for culling). Node presence is defined by the
page encoding's quadrant tags; the store has no flags of its own — the
paired index is the sole authority for existence and delivered flags.
Heights are stored in the
**orthometric** (geoid-shifted SDS) vertical datum (format v2), which
is what lets flat water and filled ocean collapse to `(0, 0)` and keeps
the store mmappable — a planetary melown2015 store is ~750 MB even with
the ocean filled, against ~1.4 GB dense. The header carries the format
version, the metatile packaging (`metaBinaryOrder`/`metaDepth`), the
reference frame, a source hash, the pairing digest, and the resource's
`geoidGrid` and `heightFunction`, so a stale or mismatched store is
detected at load. See [rfc-metanode-store.md][rfc-7] §3
for the format and §4 for the generation method.


## Antimeridian handling

Source: `wrapPadSource` in `externals/libgeo/geo/geodataset.cpp`.

A raster in an x-periodic SRS is continuous across ±180°, but GDAL
resampling kernels are not: at the raster edge a kernel reads only the
pixels that are there, so a destination just east of the seam and one
just west of it interpolate from unrelated data and disagree along
their common border.

`geo::GeoDataset::warpInto` closes the gap for every caller. When the
source covers exactly one x-period (`geo::xPeriodOverlap`, which also
requires an upright geotransform) and the destination reaches the
seam, the warp reads through an in-memory VRT of the source widened by
a few columns wrapped in from the opposite edge. The pad is sized from
the resampling kernel's radius and the downsampling ratio, and is
built after overview selection, so it wraps whichever level the warp
actually reads.

The destination test is four corner transforms, the same ones the
source-window estimate already performs; a destination away from the
seam skips the padding entirely. This covers both overview generation
and request-time serving, since both go through `warpInto`.

Two consequences worth knowing:

- No dataset needs a stored wrap margin. Padding a dataset instead
  would have to be done in ground units at the *coarsest* overview,
  because GDAL overviews share the base extents — which for a deep
  pyramid approaches one whole period of margin per side.
- A stored margin is actively harmful to the tiling pass, whose
  source-window estimate relies on GDAL's own rule of widening a
  wrapped window to the full raster; margins keep the window below
  that threshold. `mapproxy-tiling` therefore restricts its filter
  passes to one period as well.


## Metatile serving

Source: `mapproxy/src/mapproxy/generator/metatile-store.cpp`
(`metatileFromStore`); store open/validation in
`mapproxy/src/mapproxy/generator/surface-dem.cpp`.

When a client requests a metatile URL on a store-backed resource, the
server:

1. Reads the block's page from the mmapped store (validated at resource
   load: reference frame, packaging, `geoidGrid`, `heightFunction`,
   mask absence, pairing digest against both the `tiling.<rf>` file and
   the cached delivery index, and that tiling coverage reaches the
   configured LOD range).
2. For each node, takes the policy-applied mesh/watertight/navtile flags from
   the paired delivery index and min/max height from the store, then serialises
   those with the derived-at-delivery fields into the v6 metatile. A node whose
   index entry is zero but whose store payload exists (a `skipPartial`
   suppressed tile) is a structural node: it is serialised with its stored
   height range, which bounds every descendant mesh, so client-side
   culling can decide the descent toward the deeper geometry. **No warp.**
3. Returns the v6 binary as the HTTP response.

Fields not stored are produced at delivery: the **surrogate** is the
range midpoint; **horizontal extents** are the analytic full-cell SDS
bounds (cartolina-js ignores the sampled extents anyway); **texelSize**
is a calibrated relief heuristic over the stored range (`c = 0.5`, with
the relief/edge ratio clamped against source-data defects); the
**navtile flag** is derived from the resource's navtile LOD range; and
the stored orthometric range is shifted back to the raw-SDS vertical the
v6 wire expects by adding the geoid undulation, sampled on a per-block
lattice whose density follows the geoid grid's own pixel pitch. Child
flags come from the paired flag/delivery index (the same `validSubtree`
queries the warp path uses), not the store tree, so the parity diff
compares like with like.

The metatile format is unchanged **v6**, byte-compatible with the
existing client; the store emits the same bytes the warp produced, with
two intentional, characterised differences (surrogate is the midpoint
rather than the sampled mean; extents are full-cell rather than
sampled). RFC 7 deliberately did not break the format.

**Latency.** The store path answers in milliseconds — measured store
p50 ~25–31 ms vs warp p50 ~700 ms on the same resource — and uses no
GDAL on the request path. The handful of global coarse metatiles
(LOD ≤ 5, one per planet) sit at ~230 ms, bounded by per-block NodeInfo
setup on the constrained polar subtrees rather than by store cost. The
model stays CDN-compatible: metatile URLs are keyed on tile ID and
stable, so CDN caches absorb repeats; the difference from the old model
is that cold origin misses are now cheap.

**Warp fallback.** Store selection happens at resource preparation. A store
that fails validation is rejected, and the resource uses the serve-time warp
only when the legacy `dem.min`/`dem.max` pyramids are present. A normal-only
resource with no valid store cannot warp, so it fails to prepare instead of
silently degrading. Once a store is adopted, every metatile request uses it;
a missing page or reachable-node payload is a broken paired artifact and
fails visibly rather than falling back. Masked resources always use the warp
path. Tiled-geodata freelayers that read the same DEM share the validated
store path. See
[metanode-store-operations.md](metanode-store-operations.md) for the
full validation matrix and failure-mode log messages.

### Legacy warp path

In `--legacyTiling` mode the server retains the original behaviour, in
`metatileFromDemImpl()` (`mapproxy/src/mapproxy/generator/metatile.cpp`):
decompose the metatile into blocks, **warp the three-pyramid VRTWO**
into a sample grid per block (`Operation::valueMinMax`) to derive height
ranges and extents, reduce per node, serialise to v6. The GDAL warp is
the dominant cost — 100–500 ms warm, substantially worse cold — and is
exactly the redundant serve-time work the metanode store removes.


## The structural problem — and why it is now solved

Before RFC 7, the server re-derived every metatile field at request
time by warping the VRTWO, even though resource setup had already
produced everything a metanode carries:

| Metatile field | Already available from |
|---|---|
| Tile existence, child flags, watertight | Tile index (QTree) |
| Height range min/max | VRTWO min/max pyramids |
| Texel size | Analytic from LOD + reference frame (modulo relief) |
| SDS horizontal extents | Analytic from tile ID and division node |

The per-request warp re-computed a sub-problem of a computation already
finished offline: a fast, cacheable, content-addressed HTTP response was
produced by the single most expensive operation the server performed.
The obvious fix — folding height ranges into the flag tile index — does
not work, because a quadtree is compact only when neighbouring cells
share a value, and float height ranges essentially never do; widening
the flag node would balloon a megabyte index into gigabytes and tax
every flag lookup (and vtsd). See [rfc-metanode-store.md][rfc-7] §1 for the
full argument.

The metanode store resolves this by keeping the height range in its
**own** quadtree, separate from the flag index, stored orthometrically
so flat regions still collapse. The expensive operation moves back into
the offline generation phase where it belongs, the serve path becomes
"read bytes, derive a few analytic fields, emit bytes", and the min/max
VRTWO pyramids — dead weight whose only consumer was the serve-time
warp — disappear at both build and serve time. This was tracked as the
backlog item **PERF: pre-built metatile index eliminating serve-time
DEM warps**, now implemented; the tiling-side redesign was the
**PERF/REDESIGN: coverage-mask `mapproxy-tiling`** item, also subsumed
by RFC 7 §4.


## Client-side impact — ping-pong

The client fetches metatiles in a sequential descent: it fetches the
root metatile, reads the child flags, fetches those children's
metatiles, and so on until the visible tiles at the target LOD are
known. Each step is a network round-trip. For a surface at LOD 15, the
descent from the root to the visible tiles can require up to ~15
sequential metatile requests before geometry loading starts.

This is a **separate problem from serve-time warp cost, and RFC 7 did
not solve it.** Even now that each metatile is served from the store in
milliseconds, the round-trip count itself delays initial rendering. The
fix is to stop serving single-LOD blocks and serve **shallow subtrees**
(a metatile spanning several LODs), cutting a LOD-15 descent from ~16
fetch phases to ~4. RFC 7 was explicitly designed not to preclude this:
the store keeps raw node payload (not pre-serialised metatiles) and a
page shape equal to the resource's metatile packaging
(`metaBinaryOrder`/`metaDepth`), both of which a future rebrick can
change without re-running the DEM tiling pass. The packaging values are
parsed, validated, and advertised in mapConfig server-side today, but
current cartolina-js clients still consume hardcoded terrain
`metaBinaryOrder = 5` and `metaDepth = 1`, so datasets served to them
must keep those effective values.

The remaining client work — consuming the advertised packaging,
requesting multi-LOD metatiles, and the operator-grade repackaging tool
— is deferred to its own RFC. See [backlog.md][backlog]:
**CLIENT/REDESIGN: shallow-subtree metatile delivery**.

[backlog]: https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/backlog.md
[rfc-7]: https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md
[surface-metatile]: https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/surface-metatile.md
[tile-index]: tile-index.md
