# Tile index

Part of the [tileserver documentation](index.md).

This document describes what a VTS tile index carries, how
`mapproxy-tiling` produces one, and how the tileserver assembles the
index it actually serves from a resource definition plus the generated
tiling. It records only facts verifiable in the source.

For the surrounding generation pipeline (survey, VRTWO, unified tiling
pass, serve-time warp) see
[tileserver-metatile-production.md](tileserver-metatile-production.md).
For reference-frame concepts see
[reference-frames.md](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/reference-frames.md).

Source paths below are relative to the repository root.


## What a tile index carries

A tile index is not geometry. It is a per-tile classification map.

### Structure

`TileIndex` (`externals/vts-libs/vts-libs/vts/tileindex.hpp`) holds a
`std::vector<QTree> trees_`, one quadtree per LOD, anchored at
`minLod_`. It is therefore a stack of sparse rasters keyed by
`TileId = (lod, x, y)`. Each `QTree`
(`externals/vts-libs/vts-libs/vts/qtree.hpp`) stores one `uint32` value
per cell and collapses uniform regions into a single node, so empty or
uniformly-flagged areas cost almost nothing. Lookup is
`get(tileId) -> value`. There is no separate existence bit: value 0 is
"no tile here", so clearing a tile's value and never setting it are the
same state.

It stores no mesh, heights, textures, or file offsets — only, for each
tile position, a small bitmask describing what kind of tile, if any,
exists there.

### The per-tile value

The value is a flag bitfield, defined in `tileindex.hpp` (`Flag` enum,
lines 47-80). The stored single-bit flags:

| Bit | Name | Meaning |
|---|---|---|
| 0x01 | `mesh` | a mesh exists here; this is the existence test |
| 0x02 | `watertight` | the mesh covers the whole tile pane, no holes |
| 0x04 | `atlas` | the tile has a texture atlas |
| 0x08 | `navtile` | the tile carries a navigation tile |
| 0x10 | — | unassigned |
| 0x20 | `alien` | shared its value with a reference tile in the past |
| 0x40 | `multimesh` | the mesh has more than one submesh |

A tile is "real" iff it has a mesh (`real = mesh`, line 56). Derived
masks built from the above: `content = mesh | atlas | navtile`,
`nonmeta = watertight | multimesh`, `any = 0xff`, `none = 0`. A meshless
alien tile is an `influenced` tile — a non-existent tile that inherits
its value from a coarser LOD (lines 70-73).

Because `watertight` means "the mesh covers the whole pane", it can only
accompany `mesh`: whenever `mesh` is cleared, `watertight` must be
cleared with it. `mapproxy-tiling --skipPartial` relies on this — it
clears the whole value of a partial tile rather than leaving a stray
flag.

Clearing a tile does not clear its descendants — and does not need to.
The hierarchy the client traverses is not stored in the index at all:
metatile child flags are derived from it per request, and an all-zero
subtree is never advertised, so a suppressed branch with no geometry
below is unreachable rather than a dangling leaf. See
[Child existence: derived, not stored](#child-existence-derived-not-stored).

The `reference` value reported by `getReference` (line 106) is **not**
in this byte; it is stored as `flags >> 16`, in memory only, and is
never serialised.

### Serialisation constraint

Only the low byte of the value is serialised, and `0xff` is reserved
(`tileindex.hpp` lines 44-45). `0xff` is the quadtree's gray-node
marker, `GrayNode` in `externals/vts-libs/vts-libs/vts/qtree.cpp`
line 50: in the V1 node encoding (`loadV1`, line 577) a node byte of
`0xff` means "this node is subdivided, read its four children," as
opposed to a leaf carrying an actual value. Reserving it keeps a leaf
value from ever colliding with the marker, which is why the flag set is
capped at 7 usable persistent bits (bit `0x80` is held clear). Of those
seven, six are assigned and `0x10` is free. The current `save`/`load`
path (lines 317-348) marks an internal node with a 2-bit type field
instead, but the 7-bit reservation keeps both encodings unambiguous.

### Per-tile versus metatile-level flags

`nonmeta = watertight | multimesh` (line 62) names the flags that are
**not** present in the metatile-level index; they exist only in the full
per-tile index.


## Production: `mapproxy-tiling`

`mapproxy-tiling --apply` produces the tile index. For a DEM the default
is the RFC 7 **unified pass** (`mapproxy/src/tiling/unified.cpp`): four
one-pixel-per-tile GDAL filter passes per division node plus a bottom-up
2×2 min/max mip loop emit the flag index and the paired metanode store in
one run, with `--prune` and `--skipPartial` shaping which tiles are
flagged. That pass is documented in
[tileserver-metatile-production.md](tileserver-metatile-production.md)
§3; the coverage→flag rules below describe the **legacy per-tile
analysis** (`mapproxy/src/tiling/tiling.cpp`), still reachable through
`mapproxy-setup-resource --legacyTiling` and used for imagery coverage
tiling.

### How coverage maps to flags (legacy analysis)

The `TreeWalker` class descends the tile tree depth-first. For each
candidate tile it warps a grid of `(tileSampling + 1)²` samples
(`tileSampling` defaults to 128, so 129 × 129 = 16 641 samples;
`tiling.hpp` line 44, `tiling.cpp` line 249) from the source into the
tile's own reference-frame-node SRS (`node.srsDef()`, lines 271-272),
inspects the coverage mask (`tileDs.cmask()`, line 308), and sets flags.
`checkMask` (lines 310-320) classifies the warped tile as `whole`,
`some`, or `none`:

- **`none`** (lines 364-371): empty, no children, descent stops.
- **`some`** (lines 352-360): partially covered. The tile is set to
  `mesh` only (no `watertight`); descent continues.
- **`whole`** (lines 322-349): fully covered. There are two sub-cases,
  and the split is on **source resolution**, not LOD:
  - `!wri.overview && wri.truescale >= 1.0` (line 326): the warp
    consumed the original dataset with no downscaling, so deeper
    sampling cannot reveal new holes. `fullSubtree()` (lines 207-217)
    sets the whole subtree from this LOD down to `lodRange.max` to
    `mesh | watertight`, and descent stops.
  - otherwise (lines 342-343): the tile was filled from downsampled
    source. Only **this** tile is set `mesh | watertight`; descent
    continues, because finer source data could still resolve holes.

`navtile` is added to `baseFlags` when `!upscaling` (lines 302-305).

### Other flag sources

- `config_.forceWatertight` (lines 313-317) reclassifies `some` as
  `whole`, marking partially-covered tiles watertight. Default false
  (`tiling.hpp` line 44).
- Invalid reference-frame nodes get a deliberate fake-watertight subtree
  (lines 221-237; the comment states it is a lie that "will not hurt
  anyone").


## The served index: `prepareTileIndex`

Source: `mapproxy/src/mapproxy/support/tileindex.cpp`.

The index the tileserver serves is not the on-disk tiling file directly.
`prepareTileIndex` rebuilds an index from scratch (`ti = {}`, line 42)
each time the resource is loaded, from two inputs.

### Synthetic lod/tile-range index

The first block is built purely from the resource's configuration — its
`lodRange` and tile ranges. For every LOD in `resource.lodRange` it
stamps the configured tile rectangle, clipped to productive
reference-frame nodes, with default flags. Default flags are `mesh`;
`watertight` is added only when there is **no** external tiling, and
`navtile` is added at `lodRange.min`. This index knows where the
resource declares coverage, not where data exists.

### Combining with the tiling

If an external tiling is present, it is loaded and combined with the
synthetic index via `combine`. The combiner is an intersection: it
returns 0 unless a tile is present in **both** inputs
(`if (!o || !n) return 0`), and otherwise unions the flag bits. The
combine is limited to `resource.lodRange`. A mask tree, if present, then
clips the result.

Because the synthetic index carries `watertight` only when there is no
tiling, with a tiling present every `watertight` bit in the served index
comes from the tiling. The intersection also means a tile the tiling
omits (or that `--skipPartial` cleared) is absent from the served index,
even though the synthetic index would default it to `mesh` — the tiling
has the final say on existence.

### LOD-range broadening

When `resource.lodRange.max` exceeds the tiling's maximum LOD, the tiling
is "too shallow" and is enlarged before the combine:

```cpp
datasetTiles
    .makeAvailable(vts::LodRange(0, resource.lodRange.max))
    .completeDownFromBottom(TiFlag::any, TiFlag::watertight);
```

- `makeAvailable` force-creates empty quadtrees for every LOD from root
  to the new max.
- `completeDownFromBottom` finds the finest non-empty LOD and merges its
  tree downward into the new empty LODs, copying flag bits but **clearing
  `watertight`** (the second filter argument): a watertight parent does
  not imply watertight children, so the synthesised deeper LODs keep
  coverage (`mesh`) without inheriting a watertight claim they cannot
  substantiate.

So broadening the **max** replicates the tiling's deepest footprint
downward into the added LODs as partial (non-watertight) coverage.

Note that a metanode-store-backed resource **rejects** runtime LOD
expansion: if `resource.lodRange.max` exceeds the paired tiling's max
LOD, preparation fails rather than broadening, because the store has no
payload for the synthesised tiles. Re-run `mapproxy-tiling` for the
wanted range instead. Broadening therefore applies only to legacy
warp-path resources.

There is no equivalent enlargement on the **min** side. The tiling
output is populated only from its own configured minimum LOD. Because
the combine is an intersection, at any LOD coarser than the tiling's
minimum the tiling side is empty and the result is empty there. The code
documents the assumption that this does not happen: `// NB: tiling
*should* be from root` (`support/tileindex.cpp`).


## Child existence: derived, not stored

A tile index holds no parent–child information. Each LOD layer is an
independent quadtree; nothing in the format links LOD *n* to LOD *n*+1.
The delivery hierarchy the client traverses exists in exactly one place —
the child flags of served metatiles — and those are computed per request,
never stored.

When mapproxy builds a metatile (store path
`generator/metatile-store.cpp`, warp path `generator/metatile.cpp` — both
identically), each node's four child bits are filled by asking the served
index `validSubtree(child)` (`support/mmapped/tileindex.cpp`): walk every
LOD layer from `child.lod` to the deepest, and test whether any nonzero
flag value exists inside the child's footprint at that layer. The
footprint query into a deeper layer is a trimmed quadtree descent
(`support/mmapped/qtree.cpp`): a uniform region answers with its value; a
region with internal structure answers `any` — correct because uniform
regions are collapsed, so internal structure implies a nonzero value
below. The cost is a short descent per layer, not a tile enumeration: an
all-zero subtree of any size is a single collapsed node per layer.

Two consequences are worth stating explicitly.

**Reachability closure.** The client learns of children only through
these bits; there is no other channel. Since every nonzero entry carries
`mesh`, `validSubtree` is literally "is there a mesh somewhere below" —
so every advertised branch terminates in geometry, and a subtree with no
mesh anywhere (for example one whose partial tiles `--skipPartial`
suppressed) is never advertised at all. The bottom-up closure over
suppressed tiles is therefore a serve-time derivation; the published
index needs none materialized. A suppressed tile on a branch that does
lead to deeper geometry is served as a *structural* metanode: zero own
flags, child bits pointing toward the geometry, and (store path) its
stored height range as `geomExtents` so client-side culling can
decide the descent.

**The invariant is discipline, not format.** `validSubtree` tests "any
flag", not "mesh": the format would happily hold a navtile-only entry,
and a writer emitting nonzero meshless entries would break the closure by
advertising branches with nothing at their end. Every nonzero entry must
stay mesh-bearing — which is also why `skipPartial` zeroes the whole
value instead of clearing the mesh bit alone.
