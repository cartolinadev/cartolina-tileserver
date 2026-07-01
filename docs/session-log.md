# Session log

This log records significant work and non-obvious findings that apply only to
`cartolina-tileserver`. Project-wide and cross-repository work belongs in
`cartolina-js/docs/wiki/session-log.md`:
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>.

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
