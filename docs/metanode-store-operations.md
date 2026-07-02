# Metanode store operator guide

This guide covers creation, migration, verification, and rollback of DEM
resources backed by the metanode store. Design rationale is in
[RFC 7][rfc-7].

## Artifact contract

A DEM dataset directory contains these artifacts:

| Artifact | Purpose |
|---|---|
| `dem` | Normal vrtwo pyramid used for meshes and navtiles |
| `tiling.<referenceFrame>` | Tile flags: existence, watertight, navtile |
| `metanodes.<referenceFrame>` | Height ranges and flags used for metatiles |
| `dem.min`, `dem.max` | Legacy metatile-warp inputs |

`tiling.<rf>` and `metanodes.<rf>` form a pair. One `mapproxy-tiling` run
writes both through temporary files, fsyncs them, and renames them into
place. The store header contains the MD5 digest of the paired flag index.

Mapproxy accepts a store only when all of these values match the resource:

- reference frame;
- effective `metaBinaryOrder` and `metaDepth`;
- `geoidGrid` and `heightFunction`;
- pairing digest of the current `tiling.<rf>`;
- pairing digest recorded in the cached `delivery.index.src`;
- maximum LOD covered by the artifacts;
- store format version, currently 2.

Resources with a `mask` do not use the store path.

If validation fails, mapproxy logs a `W3` warning. A three-pyramid dataset
can continue through the legacy warp path because it has `dem.min` and
`dem.max`. A normal-only dataset cannot and fails to prepare.

Inspect a store with:

```sh
mapproxy-mnstore info <dataset>/metanodes.<rf>
mapproxy-mnstore dump <dataset>/metanodes.<rf> --page <lod>-<x>-<y>
mapproxy-mnstore selftest
```

## Create a DEM resource

### Using `mapproxy-setup-resource`

Metanode-store mode is the default for TIN resources:

```sh
mapproxy-setup-resource \
    --dataset dem.tif \
    --referenceFrame melown2015 \
    --resourceType TIN \
    --group <group> \
    --id <id> \
    --attribution "{copy}..." \
    --tin.geoidGrid egm96_15.gtx \
    --mapproxy.dataRoot <datasets-root> \
    --mapproxy.definitionDir <definitions-dir> \
    --mapproxy.ctrl <mapproxy-ctrl-socket>
```

The command creates the normal `dem` pyramid, runs the unified tiling pass,
publishes the paired artifacts, writes the resource definition, and waits
for mapproxy to serve it. It does not create `dem.min` or `dem.max`.
`--legacyTiling` selects the legacy three-pyramid pipeline.

Use `--gsd <meters>` to set the floor resolution. For example, this lets a
roughly 90 m DEM carry a 10 m overlay:

```sh
mapproxy-setup-resource ... --gsd 10
```

A value finer than the source resolution deepens `lodRange.max`; a coarser
value caps it. The option applies to both DEM and imagery resources and
replaces the deprecated DEM-only `demToOphotoScale` setting.

### Manual creation

Build one vrtwo pyramid, then survey and tile with a single command —
`mapproxy-tiling` measures the dataset itself, so there are no lod/tile ranges
to transcribe:

```sh
generatevrtwo <input> <dataset>/vrtwo.cubicspline \
    --resampling cubicspline
ln -s vrtwo.cubicspline/dataset <dataset>/dem

# dry survey: measure and print the report + a resource-config template
mapproxy-tiling <dataset> <rf> --gsd 10 --geoidGrid egm96_15.gtx

# once the survey looks right, tile and publish the paired artifacts
mapproxy-tiling <dataset> <rf> --gsd 10 --geoidGrid egm96_15.gtx --apply
```

The default run is a **dry survey**: it measures native and target GSD,
per-division-node lod/tile ranges, and a suggested position, prints them, and
emits a resource-config template — writing nothing. Review it, then add
`--apply` to run the four filter passes and atomically publish:

```text
<dataset>/tiling.<rf>
<dataset>/metanodes.<rf>
```

`--output` and `--store` override those paths. `--resourceConfig <path>` sets
the template destination (`-` means stdout; default is stdout on a dry run and
`<dataset>/resource.<rf>.json` with `--apply`). The template carries no
credits and a `TEMPLATE` comment: fill in attribution and the dataset path,
then copy it into a resource definition directory. A successful unified run
reports progress for the filter passes and ends with `I4 Done.`.

`--gsd <meters>` sets the target floor resolution (highest LOD). A value finer
than the source deepens `lodRange.max`; a coarser value caps it. It applies to
DEM and imagery alike and replaces the deprecated DEM-only `demToOphotoScale`.
Omitting it uses the native GSD (scaled finer for a DEM by `demToOphotoScale`).

Changing `lodRange.max` requires another tiling run for the complete new
range. On a live reload, a replacement that asks for an uncovered LOD fails
to prepare and the previous ready revision remains active. On a fresh start,
the resource remains unavailable until its definition and artifacts match.

#### Suppressing partial tiles (`--skipPartial`)

`--skipPartial` clears the mesh flag on every non-watertight (partial) tile,
so the served surface carries geometry only where a tile fully covers its
extent. Partial tiles cause visible cracks and force renderer framebuffer
switches; a local detail surface is normally used over a global base surface
that can fill the gaps, so suppressing partial meshes is often the right
operator choice. A node may then have no mesh while its children do — that is
legitimate: the subtree stays reachable and mesh requests for the suppressed
tile 404. The metanode store still records every partial tile's true coverage,
so the choice is reversible offline (see [Re-flagging](#re-flagging-an-existing-pair-reflag)).
DEM path only; mutually exclusive with `--forceWatertight`.

#### Spatial prune (`--prune`, default on)

`--prune` drops tiles that would subdivide finer than the source resolves at
their own location. Where a projection inflates area, a same-LOD tile covers
less ground, so a global lod range otherwise keeps descending past the source
resolution; the prune stops that per location. At a node centre the cutoff
equals the depth `mapproxy-calipers` measured, so low-distortion (e.g.
equatorial) coverage is unchanged and only over-generated tiles are removed.
Coarser LODs are untouched. `--prune false` tiles the full lod range
everywhere. The prune threshold is tied to `--gsd`. DEM path only.

#### Re-flagging an existing pair (`--reflag`)

`--reflag` changes the partial-tile or prune policy of an existing pair
in place, without re-running the warps:

```sh
# suppress partial meshes on an already-tiled resource (dry report first)
mapproxy-tiling <dataset> <rf> --reflag --skipPartial true
mapproxy-tiling <dataset> <rf> --reflag --skipPartial true --apply

# restore them
mapproxy-tiling <dataset> <rf> --reflag --skipPartial false --apply

# retro-prune to a target floor GSD
mapproxy-tiling <dataset> <rf> --reflag --gsd 10 --apply
```

Re-flagging reads the store as the witness of each tile's true coverage,
rewrites the flag index (and, for prune, drops store nodes too), and
re-pairs and atomically republishes both artifacts. Without `--apply` it
reports the counts it would change. It never touches heights or watertight
flags. Restoring suppressed partial tiles recomputes their navtile bit from
the dataset, so `--skipPartial true` then `--skipPartial false` reproduces the
original index exactly.

Retro-prune only removes tiles; it cannot add resolution back (the store nodes
are gone). To make a pruned resource deeper again, re-tile with `--apply`.

A running mapproxy notices the new pairing on its next resource prepare and
adopts the republished pair (see [Force resource preparation](#3-force-resource-preparation)).

#### Vertical datum inputs

The tiling command must use the resource definition's `geoidGrid` and
`heightFunction` values:

```sh
mapproxy-tiling <dataset> <rf> ... \
    --geoidGrid egm96_15.gtx \
    --heightFunction <height-function.json>
```

`geoidGrid` is compared byte for byte. Use the exact resource-definition
string. It must also be readable by PROJ because both tiling and serving use
it in a `vgridshift` operation.

For Earth, use the PROJ grid name:

```text
egm96_15.gtx
```

Do not use the VTS registry JPEG path:

```text
/opt/vts/etc/registry/geoidgrid/geographic-wgs84-egm96-geoidgrid.jpg
```

The JPEG represents the same geoid but is not a PROJ vertical-grid format.
Both setup tools validate the grid at startup and reject an unreadable grid.

Omitting `--geoidGrid` selects the reference-frame body's
`defaultGeoidGrid`. For Earth this is `egm96_15.gtx`. Use an explicit empty
value for an ellipsoidal source with no geoid:

```sh
mapproxy-tiling <dataset> <rf> ... --geoidGrid ""
```

#### Metatile packaging

Keep the defaults. Current cartolina-js clients and mapproxy require
effective `metaBinaryOrder = 5` and `metaDepth = 1` for these resources.

### Dataset in a read-only shared location

Create a writable dataset directory and link the vrtwo directory into it:

```sh
mkdir <local-dataset>
cd <local-dataset>
ln -s /shared/path/vrtwo.cubicspline vrtwo.cubicspline
ln -s vrtwo.cubicspline/dataset dem
```

Link the vrtwo directory, not the shared `dem` symlink. GDAL resolves VRT
relative paths against the opened location. The tiling and store artifacts
are then written into `<local-dataset>`.

## Migrate a three-pyramid DEM resource

The migration replaces `tiling.<rf>`. Back it up before running
`mapproxy-tiling`. Keep `dem.min` and `dem.max` in place throughout the
verification period.

### 1. Back up the legacy artifacts

Create a backup outside the dataset directory:

```sh
mkdir -p <backup>/legacy
cp -a <dataset>/tiling.<rf> <backup>/legacy/
```

Also retain the current resource definition through the deployment's normal
configuration versioning or backup procedure. The legacy rollback consists
of that definition, the saved `tiling.<rf>`, and the existing `dem.min` and
`dem.max` pyramids.

Repeat the backup and migration for each reference frame served from the
dataset. Each reference frame has its own `tiling.<rf>` and
`metanodes.<rf>` pair.

### 2. Generate the new pair

Use the original floor resolution, normally recorded in the dataset README or
the resource definition:

```sh
mapproxy-tiling <dataset> <rf> --gsd <meters> --geoidGrid egm96_15.gtx --apply
```

`mapproxy-tiling` measures the ranges itself; pass the same `--gsd` the
resource was built with (omit it to reuse the native resolution). This
replaces `tiling.<rf>` and creates `metanodes.<rf>`.

Inspect the store and compare the old and new flag indexes:

```sh
mapproxy-mnstore info <dataset>/metanodes.<rf>

mapproxy-tidiff \
    <backup>/legacy/tiling.<rf> \
    <dataset>/tiling.<rf> \
    --lodRange <min,max> \
    --tileRange <xmin,ymin:xmax,ymax> \
    --tileRangeLod <lod>
```

Known differences are boundary tiles containing only an edge-shared sample,
watertight classification over the full footprint, and navtile-band changes
at extreme latitudes. RFC 7 records their characterization.

### 3. Force resource preparation

Bump the resource `revision`, then reload mapproxy. Alternatively, clear the
resource's mapproxy-store cache directory before reloading.

A plain reload of an unchanged resource does not prepare it again. The
running generator retains its existing delivery index and store mapping, so
the new artifacts are not examined and no rejection is logged.

Preparation rebuilds the cached delivery index from the new flag index and
records its digest in `delivery.index.src`.

### 4. Verify store service

Confirm this log entry:

```text
Generator for <rf/group/id>: serving metatiles from metanode store
    "...metanodes.<rf>" (N pages, pairing <digest>).
```

Request representative metatiles and check the log for `W3` messages
containing `falling back to warp`. There must be none. Tiled-geodata
freelayer metatiles that use this DEM must pass the same check.

### 5. Establish store-only rollback

After verification, save the known-good pair together:

```sh
mkdir -p <backup>/store-pair
cp -a <dataset>/tiling.<rf> <dataset>/metanodes.<rf> \
    <backup>/store-pair/
```

Test restoration of this pair and forced preparation before removing the
legacy pyramids. The two files are one rollback unit; never restore only one
of them.

### 6. Remove the legacy pyramids

Remove `dem.min` and `dem.max` only after:

- representative surface and tiled-geodata metatiles use the store;
- the log contains no store rejection or warp-fallback warnings;
- the store pair is backed up; and
- restoration and forced preparation of that pair have been tested.

After removal, a missing or invalid store makes resource preparation fail.
The warp fallback is no longer available.

The resulting behavior is:

| Dataset artifacts | Server behavior |
|---|---|
| Three pyramids, no store | Legacy warp path |
| Matched flag index and store | Store path |
| Mismatched store with three pyramids | Warning and warp path |
| Normal-only pyramid without a valid store | Preparation failure |

## Rollback

Before deleting `dem.min` and `dem.max`, restore legacy service as follows:

1. Restore `<backup>/legacy/tiling.<rf>` to the dataset directory.
2. Remove the incompatible `metanodes.<rf>` from the active dataset.
3. Restore the previous resource definition if it changed.
4. Bump the resource revision and reload mapproxy.
5. Confirm that representative metatiles use the legacy warp path.

After deleting `dem.min` and `dem.max`, restore
`<backup>/store-pair/tiling.<rf>` and
`<backup>/store-pair/metanodes.<rf>` together, force preparation, and verify
the store-service log entry.

Apply the deployment's public revision change when a CDN or reverse proxy
caches metatile responses. Re-tiling can change the served metatile bytes.

## Failure reference

- `pairing mismatch with flag tile index`: the files came from different
  runs. Regenerate or restore the pair.
- `delivery index is not derived from...`: the resource cache is stale.
  Bump the revision or clear the cache, then reload.
- `unsupported version`: the store uses an old format. Rerun
  `mapproxy-tiling`.
- `geoid grid mismatch`: the store and resource strings differ. Correct the
  resource input and rerun tiling.
- `height function mismatch`: the store and resource functions differ.
  Rerun tiling with the current function.
- `configured max LOD ... exceeds...`: the artifacts do not cover the
  configured range. Rerun tiling for the complete range.
- `falling back to warp`: the store could not serve the metatile. Resolve
  the cause before removing `dem.min` and `dem.max`.

[rfc-7]: https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md
