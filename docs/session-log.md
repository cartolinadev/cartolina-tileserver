# Session log

This log records significant work and non-obvious findings that apply only to
`cartolina-tileserver`. Project-wide and cross-repository work belongs in
`cartolina-js/docs/wiki/session-log.md`:
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>.

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
