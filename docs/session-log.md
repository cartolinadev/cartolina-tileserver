# Session log

This log records significant work and non-obvious findings that apply only to
`cartolina-tileserver`. Project-wide and cross-repository work belongs in
`cartolina-js/docs/wiki/session-log.md`:
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>.

**New entries go directly below this line, newest first — never below an
existing entry, even one added earlier in the same session.**

## 2026-07-08 — fixed horizontal blur on dateline and polar tiles

Tiles whose extents touch the ±180° meridian of a global source (QSC back
face, plate-carrée datasets) rendered horizontally smeared; melown2015
polar tiles showed the same defect. Overview selection was ruled out — the
smeared tile picks the same overview as its sharp neighbours. GDAL's warp
kernel sizes its smoothing filter from the dst/src window ratio, and a
wrap-around footprint inflates the source window to nearly the whole
raster width, widening the kernel ~45× in x. Polar tiles hit the identical
mechanism through legitimately all-longitude windows.

Fixed in `externals/libgeo` `GeoDataset::warpInto` by setting the warp
option `XSCALE=FROM_GRID_SAMPLING`: the kernel scale is then derived from
local per-pixel derivatives, which ignore the wrap discontinuity. GDAL has
this rescue built in but auto-triggers it only at y/x anisotropy > 100;
these tiles sit well below that. The sentinel is an undocumented but
stable knob present in GDAL 3.2–3.12; GDAL 3.13 made grid sampling the
default and dropped the sentinel (any XSCALE parses as a number there),
so the option is version-bounded to that range. Interior tiles are
byte-identical before/after; dateline and polar tiles come out sharp,
matching full-resolution reference renders.

The served (delivery) index is the synthetic full-tileRange bootstrap
intersected with the paired tiling, but `TileIndex::combine` visits only
lods the tiling has trees for. A lod with no tiles at all is skipped, and
the bootstrap's blanket mesh flags survive into delivery there, advertising
tiles the store never stored — the serve-path integrity check then answers
500 ("store page has no payload for reachable tile"). `--skipPartial`
tilings produce exactly such lods routinely: at the top of the range every
tile is partial, so whole coarse lods carry structural store payload and no
flags.

Fix in `support/tileindex.cpp` (`prepareTileIndex`): force empty trees
across the whole resource lod range on the loaded tiling before combining,
so the combination is a true intersection. The served reachable set now
matches the store checker's contract — payload present exactly for the
tiles the index reaches — and coarse lods serve structural metanodes
(flags and children, stored height envelopes) as skipPartial intends. This
also closes the residual gap in the 07-02 backlog entry ("the store serve
path must not depend on warp fallback"): the resolution there assumed the
synthetic-index intersection was complete.

The code fix alone did not repair a deployed instance: the delivery
index is cached on disk and `delivery.index.src` recorded only the
source tiling digest, so a reopen against an unchanged tiling kept
serving the stale index built by the old code. The record now carries a
derivation revision next to the digest (`deliveryIndexDerivation` in
`support/tileindex.hpp`, bumped with this fix; writer/reader centralized
in `saveDeliveryIndexSource`/`deliveryIndexCurrent`), and a mismatch on
either token marks the cached index stale, triggering the existing
re-prepare path. Old-format records mismatch by construction, so every
store-backed resource rebuilds its delivery index once on first start
after deploy.

Verified on the dev instance against the existing store: first start
re-prepared the store-backed resources, rebuilt the delivery indexes,
and the failing metatile plus a full descent across the lod range serve
200; flagged tiles are byte-identical between tiling and delivery,
structural coarse tiles carry no flags, and store-backed earth resources
plus tms/geodata consumers are unchanged.

## 2026-07-07 — fixed 500 on metatiles above the resource lod range

Same failure shape as the 07-06 root-metatile fix, one dimension over.
The store domain is bounded not only spatially (bisection subtrees) but
also in lod: tiling emits payload from `lodRange.min` down, so metatiles
above the resource lod range are pure descent structure with no store
page — yet the serve path demanded one whenever the subtree beneath was
reachable, and returned 500. Any resource whose lod range starts deeper
than its reference frame's division nodes hits it on the metatiles
between the two (a QSC surface starting at lod 4 was enough).

Fix: serve metatiles above `lodRange.min` structurally (flags and
children only), the same way blocks outside the bisection domain are
served; the missing-page integrity check still guards the stored range.
Verified the previously failing metatile and a client-driven descent
across the whole lod range against a dev server.

## 2026-07-06 — fixed 500 on a reference frame's root metatile

The store only holds tiles from bisection nodes. The serve path was
querying it for manual and barren nodes too — the melown2015 root is a
manual node — finding nothing, and (now that the warp fallback is gone)
returning 500. Requesting metatile 0-0-0 was enough to trigger it.

Fix: only query the store for bisection nodes; serve the rest (manual
and barren) with flags and children, which is all those nodes carry.
Verified the melown2015 root metatile now comes out byte-for-byte the
same as the old warp path produced, with stored terrain metatiles
unchanged. See the RFC 7 addendum (2026-07-06).

## 2026-07-05 — removed dead surface-definition options

Removed four surface `definition` options that the audit found inert:
`nominalTexelSize`, `mergeBottomLod`, `metaBinaryOrder`, and `metaDepth`.
Each was a serve-time resource-definition input for a value that is either
informational or fixed at tiling time, with no functional effect in
mapproxy: `nominalTexelSize` fed only the `vts` info printout;
`mergeBottomLod` is read only by the vts-libs tileset-merge path mapproxy
never invokes; the metatile packaging pair could only ever be `(5, 1)` (the
serve/emit path addresses metatiles by the reference-frame order directly,
and `checkPackaging()` threw on anything else). Effective packaging is now
the reference-frame order and depth 1. The store header remains the
packaging authority; when shallow-subtree delivery makes per-surface
packaging real it belongs there, not in the resource definition — see the
RFC 7 addendum (2026-07-05) and the backlog entry. `resources.md` updated.

## 2026-07-05 — refreshed the resource-definition reference

Audited the established sections of [resources.md](resources.md) against the
resource parsers and documented the three production TMS generators added
after the upstream project ended: `tms-gdaldem`, `tms-normalmap`, and
`tms-specularmap`. Corrected field requirements, defaults, accepted values,
current introspection behavior, metatile packaging options, and supported
mesh inputs. Obsolete and experimental registered drivers remain
undocumented by design.

Rewrote the page as an operator guide organized around concrete tasks and
complete configuration examples. It defines project terms when they first
appear, explains what each setting changes, gives actionable defaults, and
keeps implementation libraries and source-tree paths out of the prose. The
review also corrected `tms-normalmap`: it derives bump detail from image
brightness, while surface normal maps are the ones derived from DEM geometry.
Removed the obsolete `ppx` and `ppy` URL expansions from the operator guide;
their retained compatibility implementation is marked obsolete and pending
deletion.
Removed the legacy `tms-bing` driver from the operator guide and marked its
retained compatibility definition obsolete and pending deletion.

Removed the stale warnings from the documentation index and README that
called the resource reference old or incomplete. The README now points users
to the local tileserver documentation for operator material and to the shared
cartolina-js wiki for project-wide documentation.

## 2026-07-05 — adopted metanode stores never fall back to warp

Removed the per-request warp fallback from the metanode-store serve path.
Store selection now happens once at resource preparation: a valid paired
store is the sole metatile source; without an adopted store, legacy warp is
available only when both `dem.min` and `dem.max` exist. Surface DEM and both
tiled-geodata generators follow the same rule.

`metatileFromStore` now returns a metatile or raises an error. Missing pages
and missing payload for any index-reachable node are broken-pair errors,
including structural ancestors with no own flags; legacy pyramids cannot
hide them. Derived-field conversion errors also propagate instead of
switching implementations mid-request.

The original coarse-metatile reproduction now returns 200 from the store:
the height-sidecar scrub already guaranteed payload for every index-reachable
node, while the served-index intersection removes synthetic range entries
absent from the paired flag index. A RelWithDebInfo build passed, the paired
store checker reported no violations, and representative store-backed
surface and tiled-geodata metatiles served without warp-fallback messages.

## 2026-07-05 — removed the completed texel-size calibration spike

Removed `mapproxy-texel-spike` from the source tree and tools package. It was
the one-off RFC 7 phase-1 harvester used to calibrate the metanode store's
texel-size heuristic. That calibration completed across 848,000 nodes and its
durable conclusions live in the RFC and serving implementation; the tool had
no operator workflow, test coverage, or in-tree consumer, and its sampling
logic had already diverged from the serving path. A future recalibration
should validate the then-current implementation directly.

## 2026-07-05 — removed the absolute bottom-LOD override

Removed `--bottomLod` from `mapproxy-tiling` and
`mapproxy-setup-resource`. Target GSD is now the sole resolution policy:
it determines the analyzed maximum LOD and, for DEMs, the spatial prune
floor. The associated internal `pruneExtraLods` offset is gone. Operators
who need terrain deeper than its native sampling for fine draped imagery
set a finer `--gsd` directly.

## 2026-07-05 — unified tiling: watertightness ends at the rf partition, not the cell

In reference frames with a manually partitioned root (melown2015-style),
division nodes physically overlap and the root's partitioning ranges decide
ownership: delivery never advertises tiles wholly outside a subtree's
constraint and clips partial tiles' meshes to it (metatile assembly gates
child flags on `NodeInfo` validity; mesh generation intersects the node's
constraint coverage mask). The mechanism is now documented in the
cartolina-js wiki (`reference-frames.md`, "How partitioning ranges act at
run time").

The unified pass ignored this: it inferred a parent's watertight flag from
the complete 2×2 child test, so parents along a partition boundary — fully
covered over their valid area — were flagged non-watertight on every lod
above the finest (in melown2015, rings around the ±85.05° circle in both
polar caps), costing clients needless fallback rendering, mask
materialization and framebuffer switches. The legacy per-tile walk had the
correct semantics all along via `NodeInfo::checkMask` ("fully covered by
dataset and by reference frame definition"); the mip reduction lost them.

The 2×2 reduction now exempts a missing child whose cell is invalid in the
reference frame (derived from the subtree root's node info, sharing its
constraint sampler); a missing child intersecting the valid area vetoes as
before, so dataset edges are unaffected. `mapproxy-mnstore check`'s prune
sibling rule is relaxed accordingly — children outside the reference
frame's valid area are exempt; the tool now loads the registry (the frame
id comes from the store header) and falls back to the strict rule with a
warning when the frame is unknown.

Verified: a mid-latitude sample re-tiles to its deployed pair bit-for-bit
(dataset-edge behavior unchanged); a synthetic wide-latitude dataset gains
watertight flags exactly on the polar-cap partition-boundary rings, leaf
lods and the pseudomerc side untouched; pair checks are clean with and
without `--skipPartial`. The client-side counterpart (pre-v6 watertight
inference from children) received the same exemption in cartolina-js.

Known remaining conservatism, recorded in the backlog: a *leaf* tile that
is rf-partial stays non-watertight when the dataset stops exactly at the
constraint, because the leaf test is mask-only at one pixel per tile.

## 2026-07-04 — geodata-vector-tiled: fix cross-tile data race on remote MVT

Fixed the long-standing race in which a `geodata-vector-tiled` resource
occasionally served a tile cleanly built from a *different* tile's source
data (or an empty tile), which a CDN then caches for the full max-age.

A bare `http(s)://…` tile URL is not claimed by our MVT driver (its remote
branch matches only `.mvt`/`.vector.pbf` names, and GDAL probes its generic
HTTP driver first regardless). GDAL's HTTP driver stages the download for
re-open, and because our driver reads through `std::ifstream` rather than
VSI, the staging falls back to a real temporary file named after the URL
basename alone — typically just `{locy}.pbf`, shared by all warper processes
for every tile with the same row. Concurrent requests truncate and unlink
each other's staging file; a request can then parse a neighbor's fully
written file (clean wrong tile), a truncated one (valid *empty* protobuf →
empty tile), or none (500).

The generator now prefixes remote datasets with `mvt:`, routing the download
into the MVT driver itself, which fetches straight into memory — no staging
file, no cross-process sharing, and one fetch instead of fetch+copy+reopen.
Reproduced before the fix by hammering same-row tiles concurrently; the same
load on the fixed binary shows no anomalies. `GeneratorRevision` is bumped
so deployed clients regenerate cached geodata (`?gr=1`).

For already-deployed binaries, prefixing the resource definition's `dataset`
URL with `mvt:` is an equivalent config-only workaround.

## 2026-07-04 — fix config parsing after startup-banner support

Kept config-file options out of the command-line-only list used to reparse
unrecognized arguments. The startup banner still receives both command-line
and config-file options. Mixing the lists made programs that support
unrecognized options reject a valid config file with Boost's "too many
positional options" error.

## 2026-07-04 — split SRS/geoid helpers out of mesh.cpp into srs.cpp

`support/srs.hpp` had never had its own `.cpp`; every function it declares
(`sds2phys`, `sds2nav`, `sds()`, `sdsg2sdsr`, `phys2sds`,
`validateGeoidGrid`, `physicalCorners`, `nodeTangentSpace`) was defined in
`support/mesh.cpp` instead, a convention dating back to when SRS handling
was first split out of the mesh code. None of them touch `geometry::Mesh`.
Added `support/srs.cpp` to hold them, matching the header, and left
`mesh.cpp` with only actual mesh-geometry code.

## 2026-07-03 — data tools: log directory and startup banner convention

Data-writing tools (as opposed to mere inspection/diagnostic tools) now get
a durable, self-describing run record. `service::Program` gained an
opt-in `operatingDirectory()` hook (in the shared `libservice` submodule):
when a tool overrides it, an unspecified `--log.file` defaults to
`<dir>/log/<tool>-<timestamp>.log`, and the tool logs a startup banner
(complete command line, working directory, resolved configuration in ini
form) before anything else. Tools that don't override the hook are
unaffected.

`mapproxy-tiling` is the first tool wired up, pointed at its dataset
directory. Only `--apply` runs get the log trace; a dry survey is a
diagnostic report only and stays silent, matching its read-only nature.

Other data-writing tools (e.g. `mapproxy-setup-resource`) are candidates
for the same convention but have not been converted yet.

## 2026-07-03 — height-sidecar semantic scrub

The semantic scrub landed, format-neutral (still v2):

- `NodeData::Coverage` deleted; the payload byte is now a reserved
  constant (see the `ReservedByte` comment in `support/mnstore.cpp`
  for why it must stay nonzero). Presence comes from the page codec's
  quadrant tags.
- The unified pass emits store payload for exactly the index-reachable
  set: one `served` bit propagated up the mip ascent; a `skipPartial`
  suppressed tile with no geometry below is written to neither
  artifact (`tiling/unified.cpp`).
- `--reflag` deleted (code, CLI, docs). Changing `skipPartial` or prune
  means re-tiling.
- `mapproxy-mnstore check` rewritten as a pair verifier: pairing
  digest, payload ↔ index-reachability agreement both ways,
  parent-range containment, and the prune sibling rule. The check does
  not support `forceWatertight` stores.

Verified on the RFC 7 sample dataset: a re-tiled pair reproduces the
pre-scrub artifacts exactly (height payload and pairing digest both
identical — tiling output is deterministic); the tested pre-scrub
ordinary stores pass the new check unchanged; a new `--skipPartial`
pair passes with the narrowed node set; the dev daemon serves every
store-backed resource from its store with no warp fallbacks.
`mapproxy-mnstore selftest` passes.

RFC 7 implementation notes amended in cartolina-js (deviation 11
superseded; 2026-07-03 addendum); operator guide, tile-index.md,
tileserver-tools.md, and tileserver-metatile-production.md scrubbed of
reflag and coverage-byte semantics.

## 2026-07-03 — metanode-store architecture review: coverage byte is design debt

Reviewed the RFC 7 store/index split against the implementation, prompted
by the head-scratcher that the store duplicates coverage information the
flag index already owns. RFC 7 deliberately stored mesh and watertight
beside heights while retaining the flag index; review recognized the
two-authority risk and added pairing. The implementation nevertheless
sourced delivered flags and child existence from the paired index from
the day it landed, so serving never used the store's partial/full
distinction. `--reflag` later became its only functional reader, a
marginal feature that did not justify keeping the duplicate semantics;
`mapproxy-mnstore check` had never been run in production.

Recorded as a backlog entry ("make the metanode store a pure height
sidecar") in two stages: a format-neutral semantic scrub now, and the
4-byte payload deferred until an actual store-format change rather than
churning the format for hygiene. The shallow-subtree v7 wire format does
not itself force a store-format change.

For the record: store format v2 (orthometric heights, `bdbc870`) replaced
v1 the same day it landed, pre-production, after review caught raw-SDS
storage defeating the quadtree collapse on filled-ocean planets; v2 is the
only format in the wild.

## 2026-07-02 — schedule unified tiling by critical path

The unified tiling pass now uses a bounded FIFO worker pool instead of one
thread per filter pass blocked behind a semaphore. The semaphore made pass
ordering nondeterministic: a mask thread could acquire a slot before an
earlier elevation thread, despite elevation being the longer operation.
All elevation passes across division nodes now enter the queue before the
lighter mask passes, largest destination grids first. This keeps the long work
on the critical path and leaves mask work to fill its tail.

Division-node reduction is still single-threaded because it writes the shared
tile index and store pages, but it no longer waits for nodes in definition
order. The main thread reduces the next node whose complete set of filter
passes finishes while the remaining warps continue in the worker pool.

Dry and apply runs now log the dataset path at `info4` before probing and
measuring it, so long surveys identify their input immediately. The completed
measurement report retains its dataset path and detected type.
They also log the resolved geoid grid at `info3`, including `none`, after
reference-frame defaults are applied and the selected grid is validated.

Default warp concurrency now uses all hardware threads reported by the
standard library. Operators can lower it with `--warpConcurrency`; systems
that do not report hardware concurrency retain the fallback of four.

Each filter pass now logs when a worker starts it, before the first 10-percent
progress milestone, making the scheduler's queue progression visible.

## 2026-07-02 — prune siblings together

Pruning and `--reflag` now keep or remove all siblings together. Added
`mapproxy-mnstore check` to find stores produced by the old per-tile rule;
the check does not support `--forceWatertight` stores.

## 2026-07-02 — skipPartial reachability analysis; structural-node extents fix

Re-examined the "geometry-less leaves" P1 backlog item against the whole
pipeline: tiling pass, delivery-index preparation, both metatile serve paths,
and the cartolina-js RFC 9 traversal. The premise does not hold. A suppressed
tile's index entry is zero — byte-identical to absence (the index has no
existence bit separate from the flags) — and metatile child flags are derived
per request via `validSubtree`, which never advertises an all-zero subtree.
Since every nonzero entry carries `mesh`, every advertised branch terminates
in geometry: the bottom-up closure the item demanded already exists as a
serve-time derivation. Verified down to the mmapped quadtree trimmed-descent
semantics (internal structure implies a nonzero value below, by uniform
collapse). The mechanism is now documented in
[tile-index.md](tile-index.md) ("Child existence: derived, not stored").

The review did brush against a real defect, latent since the store serve
path was introduced: `metatileFromStore` emitted structural metanodes
(suppressed partial tiles on branches leading to deeper geometry) with
default `geomExtents` — ±inf/NaN heights. cartolina-js builds its culling
volume from the division-node span plus exactly those heights, so rendering
survived only because NaN comparisons fail open; `skipPartial` created the
first such nodes deep enough to be actually culled (coarse unproductive-block
nodes sit above the client's minimum culling depth). The warp path was never
affected — it fills extents from DEM samples independently of index flags.
Fixed: any payload-bearing node now serves its stored coverage envelope,
whose mip-built min/max aggregates the tile's full coverage and is therefore
a conservative superset of every descendant mesh envelope. Resolved the
backlog item with the corrected diagnosis.

## 2026-07-02 — prune final-range and reflag safety follow-up

Closed three review findings in the merged calipers/tiling workflow. A spatial
prune can remove every tile at the surveyed maximum LOD; the applied tiling and
generated resource configuration now use the actual final tile-index maximum,
so the store validator does not reject an otherwise valid hierarchical
resource. The dry survey remains an estimate and the apply run reports any
reduction. `--bottomLod` is now applied to the report and resource template as
well as the tiling run. Reflagging rejects a store whose header reference frame
does not match the requested frame before interpreting or rewriting its tiles.
Documentation now describes pair publication accurately: the two artifacts
are replaced sequentially, and their digest makes an interrupted mixed
generation detectable rather than making the two renames atomic.
`mapproxy-tidiff` now compares the union of both complete indexes by default;
LOD and tile ranges remain optional filters instead of required operator
inputs.
`--reflag --prune false` now fails explicitly with the required re-tiling
instruction. Documentation distinguishes the authoritative served flags in
the tile index from the store's raw coverage and min/max height. The existing
store byte is now named `coverage` (`none`, `partial`, `full`) throughout code
and diagnostics; its binary values and format are unchanged.
Review flagged `skipPartial` as unsafe for clients on the theory that the
tiling pass must materialize a bottom-up-closed delivery hierarchy; a
same-day re-examination found the premise invalid and resolved the backlog
item (see the entry above).

## 2026-07-02 — merge calipers into mapproxy-tiling; prune, skipPartial, reflag

Folded the `mapproxy-calipers` measurement into `mapproxy-tiling`. The tool now
measures the dataset itself, so a bare invocation is a dry survey (report plus a
resource-config template) and `--apply` runs the tiling — no more transcribing
`range<SRS>` lines into `--lodRange`/`--tileRange`. The standalone
`mapproxy-calipers` binary is gone; the `mp-calipers` library stays.

Three new capabilities on the DEM/metanode-store path:

- `--prune` (default on): spatially-varying bottom LOD. A tile is dropped once
  its subdivision passes the source resolution at its own location, using the
  calipers depth formula (`½·log2(A_ground/(tileArea·G²))`) evaluated per tile
  centre from the node's `geo::SrsFactors` area scale rather than only at the
  node centre. It also caps each node's leaf LOD, so a projection's area
  inflation no longer over-generates deep tiles. Verified on a synthetic
  wide-latitude strip: at the leaf LOD ~94% of the high-latitude tiles are
  pruned while coarse LODs stay byte-identical; equatorial depth matches
  calipers by construction. Subsumes the per-node-bottom-lod backlog item.
- `--skipPartial`: clears the flag-index entry of non-watertight tiles (mesh
  and watertight cleared together — a meshless node cannot be watertight),
  sacrificing valid partial content to remove boundary cracks and framebuffer
  switches. A global base surface makes that tradeoff practical. The metanode
  store retains the raw `partial` coverage classification, which makes the
  choice reversible without another warp.
- `--reflag`: flips `--skipPartial` or retro-prunes an existing pair in place,
  reading the store as the coverage witness, re-pairing and sequentially
  republishing both artifacts with no re-warp. skipPartial on→off round-trips
  the index byte-for-byte (navtile recomputed); retro-prune matches
  generation-time prune.

Parity: a `--prune false`, no-skip run reproduces the prior artifacts (identical
flag index and store pairing/pages; only the non-load-bearing sourceHash stamp
differs). The GSD model is simplified to native `gsd` + derived `targetGsd`
(dropped `gsdOverride` and the `invGsdScale` concept). `mapproxy-setup-resource`
gains `--prune`/`--skipPartial`. Serving is unchanged — both options shape only
the flag index. Serve-time child flags derive reachability from that index;
the reachability analysis above establishes that this derivation is the
bottom-up closure, so no tiling-time pass is needed.

Moved the tile-index reference from the wiki into these tileserver docs (it is
server-side). Filed a backlog item: the store serve path must not depend on the
warp fallback, since new DEM resources are normal-only and the legacy warp path
will be retired.

## 2026-07-01 — calipers: per-node LOD from node-centre scale (QSC face symmetry)

`mapproxy-calipers` reported a different maximum LOD per QSC cube face on a
global plate-carrée dataset, even though the six `earth-qsc` faces are
geometrically congruent (equatorial side faces one LOD shallower than the
top/bottom polar faces). `Node::sample()` derived per-node depth from the
projected area of one source pixel taken at the grid sample nearest the dataset
centre. Side faces contain the equator so they were probed at latitude ≈ 0°;
polar faces were probed at their lowest-latitude edge. Because a plate-carrée
source pixel covers ground ∝ cos φ, the higher-latitude probe sat on denser
ground pixels, inflating the LOD, and `std::ceil` snapped the fractional gap to
an integer split. This is not cosmetic: the final tiling caps every node to the
maximum over per-node maxima, so the inflated polar LOD propagates to all faces
and tiles the whole body one LOD (4× the tiles) deeper than warranted.

Depth is now derived from a single representative target GSD and each node's own
projection scale sampled at the **node centre** via `geo::SrsFactors` (PROJ
factors): `L = ½·log2(A_ground / (tileArea·G²))`, with `A_ground =
area(node.extents()) / arealScaleFactor(centre)` and `G` the target floor GSD.
Congruent nodes yield an identical depth by construction, with no polar
singularity (a QSC face centre is a regular point of its SRS). `computeGsd()`
now returns the larger projected pixel edge (the least-magnified direction — the
honest native resolution) instead of `sqrt(area)`; a no-op for an
equator-centred dataset. `invGsdScale` is retained only for `sourceBlockLimit`
and native/override reporting, and the LOD math takes the target GSD directly.

Verified against the rebuilt binary: `earth-qsc --gsd 10` goes from a merged
`2,15` (sides 14 / poles 15) to a uniform `2,14`; `mars-qsc --gsd 10` from `2,14`
to a uniform `2,13`; the `melown2015` merged range is unchanged while its
polar-stereographic caps drop the same way (they were tiling the ~10× plate-carrée
longitudinal over-sampling above ~80° as if it were real detail — ~1.6 LODs,
rounded to 2). Only `calipers.cpp` changed; single-file recompile.

## 2026-06-30 — Metanode store self-heal and metatile URL revision fix

A metanode-store-backed DEM resource could log a delivery-index mismatch on
every start yet still come up "ready", because a revision-only bump is
classified `Changed::safely` and never sets `changeEnforced()`. The reopen
path (`SurfaceBase::loadFiles`) then reused the stale `delivery.index.src`
and the generator went ready via the warp path, so bumping the revision never
regenerated the delivery index — clearing the resource cache was the only
working workaround.

`openMetanodeStore()` now distinguishes the recoverable case (the store is
valid but the cached delivery index was derived from an older flag tile index)
from genuine warp-fallback conditions, reporting it through a `needsReprepare`
out-flag. `SurfaceDem` refuses readiness on that flag, so the generator
re-prepares, rebuilds the delivery index from the paired tiling, and adopts
the store. The pairing-digest mismatch (store built from a different tiling)
deliberately stays a warp fallback, since a re-prepare cannot fix it.

The legacy min/max warp pyramids are now probed only when no store is serving,
and `legacyDemMetatileInputsAvailable()` checks for the files before opening
them, removing the err2 "failed to open" noise emitted for store-backed
datasets that intentionally lack those pyramids.

Fixed a duplicated revision in surface metatile URLs (`.meta?<rev>gr=...`).
The vts-libs `fileTemplate()` opened the query string with the bare revision
value before appending `gr=`/`&r=`; it now opens with `?` only. Pulled in via
the vts-libs submodule.

Recorded a code-style rule in `AGENTS.md`: no `else if` chains; prefer
independent guard `if`s, early returns, or `switch`.

## 2026-06-30 — Establish tileserver documentation ownership

Created [index.md](index.md) as the tileserver documentation entry point and
moved the metanode-store operator guide, tools inventory, and metatile
production guide from the shared cartolina-js wiki. Their old wiki paths now
contain relocation links so historical references remain valid.

The documentation split is now explicit: operational and implementation
documentation specific to the tileserver lives here; project-wide RFCs,
architecture, backlog, session history, and frontend/backend interface
documentation remain in the shared wiki.

The resource-definition document remains available but is identified as an
older, incomplete reference that must be checked against the implementation.

Created [backlog.md](backlog.md) for work confined to the tileserver and
moved the applicable entries from the shared backlog without changing their
content. Added a top-posted task to audit all of `resources.md` against the
current implementation; documenting missing generators is one part of that
audit.

The migration includes the spatially varying bottom-LOD task. Although that
entry describes client effects, its proposed implementation and acceptance
work are confined to tileserver tiling and resource configuration. Its
relationship to the per-node bottom-LOD task now uses an explicit link.

Applied the same public-documentation boundary used by `cartolina-js`: public
docs may contain software facts and references used by committed demos or
examples, but not private integrations, APIs, hosts, paths, datasets, or
validation details. Replaced the dataset-specific tile URL in `AGENTS.md`
with placeholders while preserving the filename example.

Extended the committed pre-commit hook to require `docs/session-log.md` in
the staged changes, matching the cartolina-js workflow. Trivial changes can
bypass the check with `SKIP_SESSION_LOG=1`; the existing sensitive-content
scan and its `SKIP_SENSITIVE=1` override remain unchanged. Added the email
and recovery-code checks present in cartolina-js so both repositories protect
the same common PII and secret patterns.

Added RFC 7 to the design-history section at the bottom of
[index.md](index.md) and linked it from [backlog.md](backlog.md) as the
specification behind the metanode-store and unified-tiling work.
