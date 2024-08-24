
function quadKey(x, y, z) {
    var quadKey = [];
    for (i = z - 1; i >= 0; --i) {
        quadKey.push(((((y >> i) & 1) << 1) + ((x >> i) & 1)));
    }
    return quadKey.join('');
}

var BingLayer = L.TileLayer.extend({
    getTileUrl: function (tilePoint) {
        return L.Util.template(this._url, {
            quad: quadKey(tilePoint.x, tilePoint.y, this._getZoomForUrl())
        });
    }
});

var prepareTile = function(layer, coords, generator) {
    var maxCount = Math.pow(2, coords.z);

    // create a <canvas> element for drawing
    var tile = L.DomUtil.create('canvas', 'leaflet-tile');
    // setup tile width and height according to the options
    var size = layer.getTileSize();
    tile.width = size.x;
    tile.height = size.y;

    if ((coords.x < 0) || (coords.x >= maxCount)
        || (coords.y < 0) || (coords.y >= maxCount))
    {
        // out of bounds
        return tile;
    }

    return generator(tile, size, maxCount);
}

function drawFrame(ctx, size, bottom, right) {
    // draw grid
    ctx.beginPath();
    if (bottom) {
        // last tile
        ctx.moveTo(size.x, size.y);
        ctx.lineTo(0, size.y);
    } else {
        ctx.moveTo(0, size.y);
    }

    ctx.lineTo(0, 0);
    ctx.lineTo(size.x, 0);

    if (right) {
        // last tile
        ctx.lineTo(size.x, size.y);
    }
    ctx.stroke();
}

var TileGridLayer = L.GridLayer.extend({
    createTile: function(coords) {
        return prepareTile(this, coords, function(tile, size, maxCount) {
            var ctx = tile.getContext('2d');

            var bottom = (maxCount == (coords.y + 1));
            var right = (maxCount == (coords.x + 1));
            ctx.save();
            ctx.strokeStyle = "white";
            drawFrame(ctx, size, bottom, right);
            ctx.restore();

            ctx.save();
            ctx.strokeStyle = "black";
            ctx.setLineDash([5, 5]);
            drawFrame(ctx, size, bottom, right);
            ctx.restore();

            var scale = 4.0;

            ctx.scale(1.0 / scale, 1.0 / scale);

            var center = L.point(size.x / 2, size.y / 2);
            var tileId = coords.z + "-" + coords.x + "-" + coords.y;
            ctx.font = "80px Sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "middle";
            ctx.lineWidth = 8;
            ctx.strokeStyle = "white";
            ctx.strokeText(tileId, center.x * scale, center.y * scale);
            ctx.fillStyle = "black";
            ctx.fillText(tileId, center.x * scale, center.y * scale);

            return tile;
        });
    }
});

function loadBoundLayer(callback) {
    var x = new XMLHttpRequest();
    x.overrideMimeType("application/json");
    x.open("GET", "boundlayer.json", true);
    x.onreadystatechange = function () {
        if ((x.readyState) == 4 && ((x.status == 200) || (x.status == 0)))
        {
            callback(JSON.parse(x.responseText));
        }
    };
    x.send(null);
}


// Function to extract 'pos' parameter from the URL
// convenience hack, for melown2015 ref frame only
function getPosFromURL() {
    var urlParams = new URLSearchParams(window.location.search);
    var pos = urlParams.get('pos');
    
    // pos=lat,lon,zoom
    if (pos) {
        var coords = pos.split(',');
        if (coords.length === 3) {
            var lat = parseFloat(coords[0]);
            var lon = parseFloat(coords[1]);
            var zoom = parseInt(coords[2], 10);
            if (!isNaN(lat) && !isNaN(lon) && !isNaN(zoom)) {
                return { lat: lat, lon: lon, zoom: zoom };
            }
        }
    }
    
    // x=lon,y=lat,z=zoom (mapy.cz convenience)
    var x = urlParams.get('x'), y = urlParams.get('y'); 
    var z = urlParams.get('z');
    
    if (x && y && z ) {
        var lon = parseFloat(x), lat = parseFloat(y), zoom = parseInt(z, 10);
        if (!isNaN(lat) && !isNaN(lon) && !isNaN(zoom)) {
            return { lat: lat, lon: lon, zoom: zoom };
        }
    }
    
    return null;
}


function processBoundLayer(bl) {
    var crs = L.CRS.Simple;
    var map = new L.Map("map", { crs: crs });

    var startZoom = bl.lodRange[0];

    var credits = bl["credits"];
    var copyyear = "&copy; " + new Date().getFullYear() + " ";

    var maxLod = 0;

    var url = bl.url.replace(/{lod}/g, "{z}")
        .replace(/{loclod}/g, "{z}")
        .replace(/{locx}/g, "{x}")
        .replace(/{locy}/g, "{y}")
        .replace(/{alt\(([^,)]+)[^)]*\)}/g, "$1")
        .replace(/{quad\([^)]*\)}/g, "{quad}");

    var attribution = "";
    var prefix = "";
    for (var creditIndex in credits) {
        var credit = credits[creditIndex];
        attribution += prefix;
        if (credit.copyrighted != false) {
            attribution += copyyear;
        }
        attribution += credit.notice;
        prefix = ", ";
    }

    if (bl.lodRange[1] > maxLod) {
        maxLod = bl.lodRange[1];
    }

    var layerGenerator = L.TileLayer;
    if (url.includes("{quad}")) {
        layerGenerator = BingLayer;
    }

    var tl = new layerGenerator(url, {
        minZoom: bl.lodRange[0]
        , maxZoom: bl.lodRange[1]
        , attribution: attribution

        , continuousWorld: true
        , noWrap: true
        , bounds: L.latLngBounds(crs.pointToLatLng(L.point(0, 0))
                                 , crs.pointToLatLng(L.point(256, 256)))
    });

    var layers = {};
    var overlays = {};
    layers["map"] = tl;

    if (bl.maskUrl) {
        var murl = bl.maskUrl.replace(/{lod}/g, "{z}");

        var mtl = new L.TileLayer(murl, {
            minZoom: bl.lodRange[0]
            , maxZoom: bl.lodRange[1]
            , attribution: attribution
            , opacity: 0.25
        });

        overlays["mask"] = mtl;
    }

    var gridLayer = new TileGridLayer();
    overlays["grid"] = gridLayer;

    L.control.layers(layers, overlays).addTo(map);
    map.addLayer(tl);

    // get coordinates from the URL if present
    // convenience hack, for melown2015 ref frame only
    var urlCoords = getPosFromURL();
    if (urlCoords) {
    
        let ppos = L.CRS.EPSG3857.latLngToPoint(L.latLng(urlCoords.lat, urlCoords.lon), urlCoords.zoom);     
        map.setView(crs.pointToLatLng(ppos, urlCoords.zoom + 1), urlCoords.zoom + 1);
        
        // Update the 'pos' parameter in the URL when the user pans or zooms
        map.on('moveend', function() {
               
            let ll = L.CRS.EPSG3857.pointToLatLng(crs.latLngToPoint(map.getCenter(), map.getZoom()),
                map.getZoom() - 1);
            
            var newPos = `pos=${ll.lat.toFixed(4)},${ll.lng.toFixed(4)},${map.getZoom() - 1}`;
            var newUrl = window.location.origin + window.location.pathname + '?' + newPos;
            window.history.replaceState({ path: newUrl }, '', newUrl);
        });
                
    } else {

        // set view to center of tile range at min lod
        var center = [ (bl.tileRange[0][0] + bl.tileRange[1][0]) / 2.0 + 0.5
                   , (bl.tileRange[0][1] + bl.tileRange[1][1]) / 2.0 + 0.5];

        map.setView(crs.pointToLatLng(L.point(center[0] / Math.pow(2, startZoom - 8)
                                          , center[1] / Math.pow(2, startZoom - 8)))
                , startZoom);
    }
}


function startBrowser() {
    loadBoundLayer(processBoundLayer);
}

