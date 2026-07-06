# Configuring resources

A resource definition tells mapproxy what data to serve and how to serve it. This page
explains the resource configuration from an operator's point of view.

The purpose of all resources is to provide building blocks for cartolina-js
[map styles][map-styles].

There are three kinds of resource:

- `surface`: 3D terrain generated from a digital elevation model (DEM) or a
  reference ellipsoid; and
- `tms`: a raster layer, such as aerial imagery, hillshade, or a normal map;
- `geodata`: vector features or a 3D model.

In cartolina-js [map styles][map-styles], surfaces define terrain, and `tms` and 
`geodata` are used as layers.

The `driver` selects how mapproxy produces a resource. For example,
`tms-raster` serves imagery while `tms-gdaldem` derives hillshade or slope
from a DEM.

Resource organised data in  a way prescribed by a a [reference frame][reference-frames],
which is cartolina's terminology for a complete manifesto decribing the spatial oranisation
of all resources used within within a single map style and a geometric reference for their 
interpretation.

All resources used within a single map style have to share the same reference frame, which 
must be defined in the registry used by the server. A single resource configuration, on the 
other hand, may make the resource available for use in multiple reference frames.

A common choice of reference frame for Earth-depicting maps is 'melown2015'.

[reference-frames]:
  https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/reference-frames.md
[map-styles]:
  https://github.com/cartolinadev/cartolina-js/blob/main/README.md#minimal-map-style
[lod-selection]:
  https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/lod-selection.md
[openmaptiles]:
  https://openmaptiles.org/schema/
[tileserver-tools]: 
tileserver-tools.md

## Loading resource files

The first resource file is named in the tileserver configuration file (/etc/vts/mapproxy/mapproxy.conf):

```ini
[resource-backend]
type = conffile
path = /etc/vts/mapproxy/resources.json
root = /var/vts/mapproxy/datasets
```

Relative dataset paths in resource definitions are resolved below `root`.

The resource file contains one JSON resource object or an array of objects.
It can also include other files:

```json
[
    { "include": "imagery.d/*.json" },
    { "include": "terrain.d/*.json" }
]
```

An include path is relative to the file that contains it. Wildcards are
allowed. It is not an error when a wildcard matches no files.

## A complete resource

It is normally not necessary to write resource configuration files by hand. The 
[tileserver tools][tileserver-tools] provide ways to produce them in the process of 
resource preparation. The tools either write the resource configuration directly
into the tileserver's resource configuration tree (mapproxy-setup-resource does this)
or they produce resource definition templates which the operator edits and copies
into the target location (this is the case with mapproxy-tiling).

This example serves a prepared imagery dataset as an image layer:

```json
{
    "comment": "Example aerial imagery",
    "group": "example",
    "id": "aerial",
    "type": "tms",
    "driver": "tms-raster",
    "referenceFrames": {
        "melown2015": {
            "lodRange": [0, 10],
            "tileRange": [[0, 0], [1, 1]]
        }
    },
    "credits": [],
    "definition": {
        "dataset": "imagery/example/example.tif",
        "format": "jpg"
    }
}
```

Here the top-level fields mean:

- `definition`: the path to the actual geoereferenced dataset and the format of individual
  output files.
- `type`: the resource type, tms`, `surface`, or `geodata`.
- `driver`: the specific production method described later on this page.
- `group` and `id`: the resource's URL identifier. Together they must be
  unique within each reference frame and resource type.
- `referenceFrames`: the [reference frames][reference-frame] where the resource is available 
   and which tile ranges it may cover (see below)
- `credits`: credit identifiers (data attributions) shown to clients. The field is required; use
  an empty array when no credit applies.

There may be other top level fields:

- `comment`: optional operator note. It does not affect output.
- `revision`: optional integer added to generated URLs. Change it when clients
  must stop using cached output.
- `maxAge`: optional cache lifetime overrides, in seconds. Its members are
  `config`, `support`, `registry`, and `data`.

### Tile coverage

Raster layers, terrain, and tiled vector layers need a level-of-detail (LOD)
and tile range for each [reference frame][reference-frames]:

```json
"referenceFrames": {
    "melown2015": {
        "lodRange": [4, 16],
        "tileRange": [[2, 1], [5, 3]]
    }
}
```

`lodRange` includes both endpoints. `[4, 16]` therefore serves LODs 4 through
16. `tileRange` is the inclusive range of tiles at the first LOD in
`lodRange`: `[[minimumX, minimumY], [maximumX, maximumY]]`.

Monolithic resources (resources served as one file) does not need tile ranges. List
only the reference-frame IDs:

```json
"referenceFrames": ["melown2015", "earth-qsc"]
```

Tileserver creates one addressable resource for every listed reference frame.

### Credits

`credits` may contain string or numeric IDs from the system registry or the
resource's local `registry`. String and numeric IDs must be unique across the
installation.

Credit notices may contain `{Y}` for the current year and `{copy}` for the
copyright symbol.

For example, this resource defines and uses its own credit:

```json
"registry": {
    "credits": {
        "example-provider": {
            "id": 200,
            "notice": "{copy} {Y} Example Provider"
        }
    }
},
"credits": ["example-provider"]
```

Choose a numeric ID from 200 through 65535 that no other credit in the
installation uses.

## Values shared by several drivers

### Raster format

For TMS drivers that provide a `format` field, `definition.format` selects the
encoding and filename extension of generated raster tiles. The possible
values are `jpg`, `png`, and `webp`. Each driver section gives its default and
any restrictions.

Use JPEG for opaque photographic imagery. Use PNG or WebP when the output
needs transparency or lossless storage.

### Resampling

`definition.resampling` controls how input pixels are combined when mapproxy
warps or resizes a raster. It accepts:

- `nearest`, `bilinear`, `cubic`, `cubicspline`, `lanczos`, `average`,
  `mode`, `minimum`, `maximum`, `median`, `q1`, or `q3`;
- `texture`, which chooses `average` for heavy downsampling and `cubic` at
  other scales; and
- `dem`, which chooses `average` for heavy downsampling and `cubicspline` at
  other scales.

Use `texture` for ordinary imagery, `dem` for continuous elevation, and
`mode` or `nearest` for category numbers such as land-cover classes.

### URL templates

Remote raster services and tiled vector services use templates to insert the
requested tile coordinates into a URL or dataset path. Some reference frames
divide their tile tree into regions that use different coordinate systems.
Each such region is a spatial division node.

- `{lod}`, `{x}`, `{y}`: tile coordinates in the complete reference frame.
- `{loclod}`, `{locx}`, `{locy}`: coordinates within the spatial division node
  that contains the tile. Use these when the input tiling matches one region
  of a reference frame, such as its Web Mercator region.
- `{srs}`: the coordinate system used by that spatial division node.
- `{rf}`: reference-frame ID.
- `{sub}`: sub-part number, used by resources that produce several files for
  one tile.
- `{alt(a,b,c)}`: choose one listed string consistently for each tile. This
  can distribute requests across equivalent server hostnames.
- `{switch(variable,value:replacement,...,*:fallback)}`: replace the value of
  one of the variables listed above. For example,
  `{switch(rf,melown2015:earth,earth-qsc:qsc,*:other)}` produces `earth` for
  `melown2015`, `qsc` for `earth-qsc`, and `other` for any other reference
  frame. Use `*:*` as the fallback to keep an unmatched value unchanged. With
  no fallback, an unmatched value produces an empty string.

### Raster input

#### Raw imagery

The main inputs of `tms-raster`, `tms-normalmap`, and `tms-specularmap` may be
raster files such as GeoTIFF, VRT, JPEG, or PNG. Mapproxy opens and warps the
raster when a tile is requested. A separate `mask` can limit its coverage.

#### Prepared imagery

The same drivers also accept a prepared imagery directory. It contains the
image as `ophoto` and a `tiling.<reference-frame>` file that records its tile
coverage. This is optional for the main image input; it is not a prerequisite
for serving imagery.

Preparation lets mapproxy tell the client which tiles contain data and
whether their imagery is opaque, transparent, or mixed. The client can avoid
requests outside the data coverage and make better rendering decisions. This
reduces unnecessary data transfer and rendering work.

The optional `landcover.dataset` used by `tms-normalmap` and `surface-dem` is
an exception: it must be a prepared imagery directory because those drivers
read its `ophoto` raster.

#### Prepared DEM

`tms-gdaldem` and `surface-dem` require a prepared DEM directory. It contains
the elevation raster as `dem`, tile-coverage information, and the terrain
metadata prepared for each reference frame.

These directories are created with [Tileserver tools](tileserver-tools.md).
`mapproxy-setup-resource` runs the end-to-end workflow; its stages use tools
such as `generatevrtwo` and `mapproxy-tiling`. For DEM preparation, follow the
[metanode-store operator guide](metanode-store-operations.md).

## Raster layers (`tms`)

A TMS resource is a raster map layer. It provides a `cartolina-tms` data
source for cartolina-js [map styles][map-styles]. Its driver defines how input
data is transformed into the raster tiles served to clients:

- `tms-raster` warps a raster image into ordinary image tiles.
- `tms-gdaldem` turns elevation values into hillshade, slope, aspect, or
  another relief image.
- `tms-normalmap` turns brightness changes in an image into normal-map tiles
  for bump mapping.
- `tms-specularmap` turns land-cover class IDs into reflectivity and shininess
  values for terrain lighting.
- `tms-raster-remote` does not transform pixels; it directs clients to tiles
  served by another service.

And, as a diagnostic device:

- `tms-raster-patchwork` has no input data; it generates a checkered test
  pattern.


### `tms-raster`: serve imagery

Use `tms-raster` to serve ordinary imagery — a raster file or a prepared
imagery dataset — as a `diffuse-map` layer in a cartolina-js
[map style][map-styles].

```json
"definition": {
    "dataset": "imagery/aerial/dataset",
    "format": "jpg",
    "resampling": "texture"
}
```

- `dataset` (required): raster file or prepared imagery directory.
- `mask` (optional): reference-frame mask or another raster used as a mask.
- `format` (default `jpg`): `jpg`, `png`, or `webp`.
- `transparent` (default `false`): when `true`, serve PNG with transparency.
- `erodeMask` (default `false`): shrink the valid area of the mask slightly
  to avoid coloured edges around missing data.
- `resampling` (optional): pixel resampling method. When omitted, mapproxy
  uses its texture resampling profile.

### `tms-gdaldem`: derive relief from a DEM

Use `tms-gdaldem` to generate hillshade, slope, aspect, terrain-ruggedness,
topographic-position, or roughness tiles from a DEM, served as a `diffuse-map`
layer in a cartolina-js [map style][map-styles].

```json
"definition": {
    "dataset": "terrain/dem/dataset",
    "processing": "hillshade",
    "processingOptions": ["-multidirectional"],
    "format": "jpg",
    "resampling": "dem"
}
```

- `dataset` (required): prepared DEM dataset directory. It must contain the
  `dem` raster and a `tiling.<reference-frame>` tile index.
- `processing` (required): `hillshade`, `slope`, `aspect`, `color-relief`,
  `TRI`, `TPI`, or `roughness`.
- `processingOptions` (default `[]`): additional options for the selected DEM
  operation, written as an array of strings.
- `format` (default `jpg`): generated tile format.
- `resampling` (default `dem`): resampling applied to the DEM.
- `erodeMask` (default `false`): shrink the valid data mask slightly.
- `poProgressions` (optional): vary a numeric processing option by LOD, as
  explained below.
- `colorFile` (optional): reserved for `color-relief`, but not currently
  supported. Setting it produces a warning and has no effect.

Use `poProgressions` only when a numeric processing option should change with
LOD.
Each member names an option from `processingOptions` and has the form
`[baseLod, factor]`:

```json
"processingOptions": ["-z", "1.0"],
"poProgressions": {
    "-z": [12, 1.2]
}
```

At a requested LOD, the option value is multiplied by
`factor^(baseLod - requestedLod)`.

In this example `-z` remains `1.0` at LOD 12, becomes `1.2` at LOD 11, and
becomes `1.44` at lod 10. Using `poProgressions` for  '-z' is a typical choice:
it allows for making vertical exaggeration vary exponentially with LOD.

### `tms-normalmap`: derive bump detail from an image

Use `tms-normalmap` to provide `bump-map` layers to cartolina-js
[map styles][map-styles]. Bump maps turn brightness changes in satellite or
aerial imagery into three-dimensional texture on coarser terrain.

```json
"definition": {
    "dataset": "imagery/aerial/dataset",
    "format": "webp",
    "resampling": "cubic",
    "zFactor": 0.427,
    "invertRelief": true
}
```
The driver accepts the `tms-raster` fields, with these rules:

- `dataset` is the source image and is required. It accepts the same raster
  file or prepared imagery directory as `tms-raster`.
- `format` is always `webp`; other values are rejected.
- `resampling` defaults to `cubic`.
- `zFactor` (default `0.427`) controls the strength of the apparent relief.
- `invertRelief` (default `true`) treats darker pixels as raised detail. Set
  it to `false` when brighter pixels should be raised instead.
- `landcover` (optional) supplies a land-cover raster and the class metadata
  needed to interpret its pixel values:

```json
"landcover": {
    "dataset": "landcover/dataset",
    "classdef": "landcover/classes.json"
}
```

`landcover.dataset` must be a prepared imagery directory whose `ophoto`
raster contains 8-bit class IDs. For each pixel, mapproxy finds the entry with
the same `id` in `landcover.classdef`. To remove image-derived bump detail
from water, for example, give the water value an entry with `"isFlat": true`.
The complete class-file format is described after `tms-specularmap`.

### `tms-specularmap`: derive material properties from land cover

Use `tms-specularmap` to provide `specular-map` layers to cartolina-js
[map styles][map-styles]. Specular maps turn land-cover category numbers into
material properties used in terrain lighting.

```json
"definition": {
    "dataset": "landcover/dataset",
    "classdef": "landcover/classes.json",
    "format": "png",
    "resampling": "mode"
}
```

- `dataset` (required): one-band, one-byte raster file or prepared imagery
  directory containing land-cover class IDs from 0 through 255.
- `classdef` (required): class file in the format described below.
- `format` (default `png`): `png` or `webp`.
- `resampling` (default `mode`): `mode` or `nearest`. Other methods would
  invent category numbers and are rejected.
- The other `tms-raster` fields are also accepted.

### Land-cover classes for normal and specular maps

The normal-map and specular-map drivers share a JSON class file. It is an
array with one object for each numeric value in the land-cover raster:

```json
[
    {
        "id": 0,
        "name": "water",
        "isFlat": true,
        "specular-reflectivity": 12,
        "shininess-exp": 10
    },
    {
        "id": 1,
        "name": "marsh",
        "isFlat": false,
        "specular-reflectivity": 3,
        "shininess-exp": 2
    }
]
```

- `id` and `name` are required. `id` is the matching land-cover raster value,
  from 0 through 255; `name` records what that value represents, such as
  `water` or `woodland`.
- `isFlat` defaults to `false`. When it is `true`, `tms-normalmap` writes a
  flat normal for pixels with that class ID, so brightness variations in the
  source image do not create bump detail there.
- `specular-reflectivity` and `shininess-exp` default to `0`. Both values are 
   integers between 0 (lowest) and 15 (highest).

### `tms-raster-patchwork`: generate a test pattern

This is a diagnostic driver driver generating checkered tiles. It needs no 
source dataset.

- `mask` (optional): limits where tiles are generated.
- `format` (default `jpg`): output format.

### `tms-raster-remote`: point to remote tiles

This driver publishes a raster layer whose images come from another service.

- `remoteUrl` (required): image URL template.
- `mask` (optional): limits where the remote layer is available.

### Advanced client options

Each TMS resource publishes `boundlayer.json`, which tells clients how to
request its tiles. Most operators can leave `definition.options` unset. When
present, its JSON value is copied to the `options` member of
`boundlayer.json`.

## Terrain (`surface`)

A surface resource produces terrain meshes, terrain metadata, height-query
tiles, normal maps, and a tile index. It also serves `freelayer.json`, which
allows the terrain mesh to be used as a free layer.

### Settings shared by terrain drivers

- `heightFunction` (optional): transform DEM heights before generating output.

### Changing terrain height

The supported height function is `superelevation`:

```json
"heightFunction": {
    "function": "superelevation",
    "heightRange": [0.0, 3000.0],
    "scaleRange": [1.0, 1.5]
}
```

The first height in `heightRange` receives the first scale in `scaleRange`;
the second height receives the second scale. Values between them are scaled
smoothly. Values outside the height range use the nearest endpoint scale.

### `surface-dem`: terrain from elevation data

```json
"definition": {
    "dataset": "terrain/dem/dataset"
}
```

- `dataset` (required): prepared DEM dataset directory. It contains the `dem`
  raster and the tiling information used for this resource.
- `mask` (optional): limits terrain generation to the valid data area.
- `textureLayerId` (default `0`): texture layer number written to the mesh.
- `geoidGrid` (optional): grid file for DEM heights measured from a geoid.
- `landcover` (optional): `dataset` and `classdef` paths used when generating
  terrain normal maps. `dataset` is a prepared imagery directory whose
  `ophoto` raster contains one-byte class IDs.
- `heightcodingAlias` (optional): another name by which vector resources can
  select this DEM for height coding.
- The shared terrain settings are accepted.

Without `geoidGrid`, mapproxy treats DEM values as heights above the
ellipsoid. Configure a grid when the DEM values are heights above a geoid.
The DEM and grid must describe compatible horizontal and vertical coordinate
systems.

### `surface-meta`: combine terrain and imagery

This driver places an existing raster resource directly into an existing
terrain resource:

```json
"definition": {
    "surface": { "group": "terrain", "id": "dem" },
    "tms": { "group": "imagery", "id": "aerial" }
}
```

Both resources must exist in the same reference frame. Their credits are
combined. `surface-meta` does not accept the shared terrain settings.

Restart mapproxy after changing either referenced resource. Those changes are
not picked up reliably while the server is running.

### `surface-spheroid`: terrain without a DEM

This diagnostic driver creates terrain at zero height on the reference-frame 
ellipsoid. A geoid grid can displace that surface.

- `textureLayerId` (default `0`): texture layer number written to the mesh.
- `geoidGrid` (optional): geoid-grid file name.
- The shared terrain settings are accepted.

`heightFunction` has no visible effect on this driver because its source
height is always zero.

Independently of any operator resource, mapproxy auto-registers one
`surface-spheroid` instance in the `.system` group for every reference frame
in the server registry (id `<referenceFrame>/.system/surface-spheroid`),
covering the whole reference frame from its root LOD. These require no
configuration and act as baseline ellipsoid terrain for the built-in
introspection browser. `tms-raster-patchwork` is registered the same way.


## Vector and model layers (`geodata`)

Geodata resources serve vector features or a 3D model through
`freelayer.json`. They provide `cartolina-freelayer` data sources for
cartolina-js [map styles][map-styles].

These fields are shared by the drivers below:

- `format` (default `geodataJson`): output format. `geodataJson` is the
  supported value.
- `formatConfig.resolution` (default `4096`): the coordinate quantization
  grid, a 3D analogue of the MVT `extent`. All three feature coordinates are
  snapped to a `resolution`-step grid across the geodata bounding box. A larger
  value keeps more precision but may increase output size.
- `styleUrl` (optional): default style URL. A value beginning with `file:`
  loads a local file and serves it as `style.json`. A relative local path is
  resolved below `[resource-backend] root`.
- `displaySize` (required): nominal display size in pixels. The client uses
  it to estimate on-screen tile size and drive tree traversal (LOD selection).
  See [LOD selection][lod-selection] for the detailed treatment.
- `options` (optional): value copied into `freelayer.json`.

### `geodata-vector-tiled`: tiled vector input

Use `geodata-vector-tiled` to provide `labels` and `lines` layers to a
cartolina-js [map style][map-styles].

The data comse from a tiled vector source: an MVT service, an MBTiles archive, 
or another source whose tile grid matches a spatial division node of the target 
reference frame. A typical pairing is a service following the 
[OpenMapTiles schema][openmaptiles] served in the `melown2015` reference frame.

Because the source is tiled, every reference-frame entry needs a `lodRange`
and `tileRange` — the object form shown under [Tile coverage](#tile-coverage),
not the bare-ID list that single-file resources use.

For a web service, set `dataset` to a URL template. For MBTiles, append a tile
template to the archive path:

```json
"dataset": "vectors.mbtiles/{loclod}-{locx}-{locy}"
```

- `dataset` (required): source URL template or MBTiles tile template.
- `demDataset` (required): DEM used to obtain heights.
- `geoidGrid` (optional): geoid grid for that DEM.
- `layers` (optional): array of source layer names to include.
- `mode` (default `auto`): `never` preserves every source Z coordinate;
  `always` replaces every source Z coordinate with a DEM height; and `auto`
  uses a DEM height for 2D coordinates while preserving 3D coordinates.
- `maxSourceLod` (optional): when the requested LOD is finer than the source,
  reuse tiles from this source LOD.
- `schema` (default `maptiler`): input tile-coordinate convention.
- `clipLayers` (optional): source layers to clip at tile boundaries.
- `enhance` (optional): join attributes from a SQLite database, as described
  below.
- The shared geodata fields are accepted.

`enhance` joins source features to rows of a SQLite table. Each member is named
for a source layer and provides:

- `key`: source feature attribute holding the lookup value;
- `db`: SQLite file, relative to `[resource-backend] root`; and
- `table`: SQLite table name.

The table must have an `id` column. When a feature's `key` value matches a
row's `id`, that row's remaining columns are added to the feature. For example,
to attach a population figure to each `place` feature, keyed on the feature id:

```json
"enhance": {
    "place": {
        "db": "places.sqlite",
        "table": "place_population",
        "key": "#fid"
    }
}
```

### `geodata-vector`: one vector file

This driver converts a single vector dataset, such as GeoJSON or a shapefile,
into one geodata file, assigning DEM heights to 2D coordinates. Like
`geodata-vector-tiled`, it backs `labels` and `lines` layers in a cartolina-js
[map style][map-styles]. It accepts the same vector fields — including
`enhance` — except the tiled-only `maxSourceLod`, `schema`, and `clipLayers`,
and is served as one file, so its `referenceFrames` are listed by ID only (no
`lodRange`/`tileRange`). `dataset` is a single vector file.

### `geodata-mesh`: an OBJ model

This driver converts a triangular OBJ model into one geodata file. Texture
coordinates and vertex normals are ignored.

- `dataset` (required): OBJ path.
- `srs` (required): coordinate system of the OBJ vertices.
- `center` (required): `[x, y, z]` origin in the source coordinate system.
  Use `[0, 0, 0]` when no offset is needed.
- The shared geodata fields are accepted.

Faces that share a material become one output feature named after that
material. A model without materials becomes one feature named `mesh`.

### Cesium terrain

When a reference frame has a TMS extension, surface resources also serve
Cesium quantized-mesh tiles as `{lod}-{x}-{y}.terrain` and metadata as
`layer.json`.

The tile-coordinate transform is configured on the reference-frame TMS
extension, not on the resource. This interface targets `tms-global-geodetic`,
an equirectangular reference frame not suitable for general Cartolina terrain;
a deployment that relies on it warrants its own documentation.


## Resource browser and test viewers

The browser is an optional operator interface. It adds HTML directory pages
and simple viewers for inspecting configured resources. It does not enable or
disable tile delivery.

### Enabling the browser

The browser is disabled by default. Enable it in the main mapproxy
configuration:

```ini
[http]
enableBrowser = true
```

Restart mapproxy after changing this setting. The mapproxy base URL then
provides directory pages for reference frames, resource types, groups, and
individual resources.

When the setting is absent or `false`, these HTML pages and their support
files return 404. Tile data and machine-readable files such as
`boundlayer.json`, `freelayer.json`, and `mapConfig.json` remain available.

The available resource viewers are:

- TMS resources: a 2D raster viewer at the resource URL and `index.html`.
- Surface resources: a 3D terrain viewer at `index.html`, with its generated
  Cartolina style at `style.json`.
- Geodata resources: a 3D vector or model viewer at the resource URL and
  `index.html`.
- `surface-meta` resources do not provide their own viewer.

### Configuring a surface viewer

The surface definition's optional `introspection` object configures the
initial contents of its 3D test viewer. It does not enable the browser.

The object may contain:

- `position`: initial VTS camera position;
- `browserOptions`: initial Cartolina-js runtime options; and
- `tms`: one raster layer or an array of raster layers to place on the
  terrain.

A `tms` entry can name another local resource:

```json
"introspection": {
    "tms": { "group": "imagery", "id": "aerial" }
}
```

It can also name a remote `boundlayer.json` URL:

```json
"introspection": {
    "tms": {
        "id": "external-aerial",
        "url": "https://example.test/boundlayer.json"
    }
}
```

The older `introspection.geodata` field is ignored by the current viewer and
produces a warning. Do not use it in new resources.

### Configuring a geodata viewer

The geodata definition's optional `introspection` object may contain:

- `surface`: local terrain resource on which to display the geodata;
- `position`: initial VTS camera position; and
- `browserOptions`: initial Cartolina-js runtime options.

For example:

```json
"introspection": {
    "surface": { "group": "terrain", "id": "dem" }
}
```

The geodata definition's `styleUrl` selects the vector style used by the
viewer and published in `freelayer.json`.
