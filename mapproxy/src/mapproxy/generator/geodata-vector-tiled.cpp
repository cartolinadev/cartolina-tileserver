/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <fstream>
#include <stdexcept>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "utility/premain.hpp"
#include "utility/raise.hpp"
#include "utility/format.hpp"
#include "utility/path.hpp"

#include "geo/heightcoding.hpp"
#include "geo/srsfactors.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "vts-libs/vts/opencv/navtile.hpp"

#include "../support/tileindex.hpp"
#include "../support/srs.hpp"
#include "../support/revision.hpp"

#include "geodata-vector-tiled.hpp"
#include "factory.hpp"
#include "metatile.hpp"

namespace ba = boost::algorithm;

namespace vr = vtslibs::registry;
namespace fs = boost::filesystem;

namespace generator {

namespace {

struct Factory : Generator::Factory {
    virtual Generator::pointer create(const Generator::Params &params)
    {
        return std::make_shared<GeodataVectorTiled>(params);
    }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType<GeodataVectorTiled>(std::make_shared<Factory>());
});

/** NOTICE: increment each time some data-related bug is fixed.
 */
int GeneratorRevision(1);

/** Remote tiles must be downloaded by the MVT driver itself (mvt: prefix).
 *  A bare http(s) URL is claimed by GDAL's generic HTTP driver, which stages
 *  the download in a temporary file named solely after the URL basename
 *  (typically just "{locy}.pbf"); concurrent tile requests in separate warper
 *  processes overwrite each other's staging file and a tile can be silently
 *  built from another tile's data.
 */
std::string mvtDataset(const std::string &dataset)
{
    if (ba::istarts_with(dataset, "http:")
        || ba::istarts_with(dataset, "https:")
        || ba::istarts_with(dataset, "ftp:"))
    {
        return "mvt:" + dataset;
    }
    return dataset;
}

} // namespace

GeodataVectorTiled::GeodataVectorTiled(const Params &params)
    : GeodataVectorBase(params, true)
    , definition_(this->resource().definition<Definition>())
    , dem_(absoluteDataset(definition_.dem.dataset + "/dem")
           , definition_.dem.geoidGrid)
    , effectiveGsdArea_(), effectiveGsdAreaComputed_(false)
    , tileFile_(mvtDataset(definition_.dataset))
    , physicalSrs_
      (vr::system.srs(resource().referenceFrame->model.physicalSrs))
    , warpFallbackAvailable_(false)
{
    {
        // open dataset and get descriptor + metadata
        auto ds(geo::GeoDataset::open(dem_.dataset));
        demDescriptor_ = ds.descriptor();

        auto md(ds.getMetadata("vts"));

        if (auto effectiveGSD = md.get<double>("effectiveGSD")) {
            effectiveGsdArea_ = (*effectiveGSD * *effectiveGSD);

            LOG(info2)
                << "<" << id() << ">: using configured effective GSD area of "
                << definition_.dem.dataset << ": "
                << effectiveGsdArea_ << " m2.";
        } else {
            auto esize(math::size(demDescriptor_.extents));
            auto srs(demDescriptor_.srs.reference());
            if (srs.IsGeographic()) {
                // geographic coordinates system -> convert degrees to meters
                double a(srs.GetSemiMajor());
                // double b(srs.GetSemiMinor());

                esize.width *= (a * M_PI / 180.0);

                // TODO: compute length of arc between extents.ll(1) nad
                // extents.ur(1) on the ellipsoid
                // for now, use same calculation as on the equator
                esize.height *= (a * M_PI / 180.0);
            }

            math::Size2f px(esize.width / demDescriptor_.size.width
                            , esize.height / demDescriptor_.size.height);

            effectiveGsdArea_ = math::area(px);
            effectiveGsdAreaComputed_ = true;

            LOG(info2)
                << id() << ": using computed effective GSD area "
                << definition_.dem.dataset << ": "
                << effectiveGsdArea_ << " m2.";
        }
    }

    if (changeEnforced()) {
        LOG(info1) << "Generator for <" << id() << "> not ready.";
        return;
    }

    try {
        auto indexPath(root() / "tileset.index");
        auto deliveryIndexPath(root() / "delivery.index");

        if (fs::exists(indexPath)) {
            if (!fs::exists(deliveryIndexPath)) {
                // no delivery index -> create
                vts::tileset::Index index(referenceFrame().metaBinaryOrder);
                vts::tileset::loadTileSetIndex(index, indexPath);

                // convert it to delivery index (using a temporary file)
                const auto tmpPath(utility::addExtension
                                   (deliveryIndexPath, ".tmp"));
                mmapped::TileIndex::write(tmpPath, index.tileIndex);
                fs::rename(tmpPath, deliveryIndexPath);
            }

            // load delivery index
            index_ = boost::in_place(referenceFrame().metaBinaryOrder
                                     , deliveryIndexPath);
            openMetanodeStore();
            if (!store_ && !warpFallbackAvailable_) {
                return;
            }
            makeReady();
            return;
        }
    } catch (const std::exception &e) {
        // not ready
    }

    LOG(info1) << "Generator for <" << id() << "> not ready.";
}

void GeodataVectorTiled::prepare_impl(Arsenal&)
{
    LOG(info2) << "Preparing <" << id() << ">.";

    const auto &r(resource());

    // try to open datasets
    geo::GeoDataset::open(dem_.dataset);

    // prepare tile index
    {
        const auto tilingPath
            (absoluteDataset(definition_.dem.dataset)
             + "/tiling." + r.id.referenceFrame);
        const auto storePath
            (fs::path(absoluteDataset(definition_.dem.dataset))
             / ("metanodes." + r.id.referenceFrame));

        vts::tileset::Index index(referenceFrame().metaBinaryOrder);
        prepareTileIndex(index, tilingPath, r, false, MaskTree()
                         , fs::exists(storePath)
                         ? TileIndexExpansion::reject
                         : TileIndexExpansion::allow);

        // save it all
        vts::tileset::saveTileSetIndex(index, root() / "tileset.index");

        const auto deliveryIndexPath(root() / "delivery.index");
        // convert it to delivery index (using a temporary file)
        const auto tmpPath(utility::addExtension
                           (deliveryIndexPath, ".tmp"));
        mmapped::TileIndex::write(tmpPath, index.tileIndex);
        fs::rename(tmpPath, deliveryIndexPath);

        {
            std::ofstream source
                ((root() / "delivery.index.src").string()
                 , std::ostream::out | std::ostream::trunc);
            source << mnstore::fileDigest(tilingPath) << "\n";
        }

        index_ = boost::in_place(referenceFrame().metaBinaryOrder
                                 , deliveryIndexPath);
    }

    openMetanodeStore();
    checkMetatileSource();
}

vr::FreeLayer GeodataVectorTiled::freeLayer_impl(ResourceRoot root) const
{
    const auto &res(resource());

    vr::FreeLayer fl;
    fl.id = res.id.fullId();
    fl.type = vr::FreeLayer::Type::geodataTiles;

    auto &def(fl.createDefinition<vr::FreeLayer::GeodataTiles>());
    def.metaUrl = prependRoot
        (utility::format("{lod}-{x}-{y}.meta?gr=%d-%d%s", GeneratorRevision
                         , vts::MetaTile::currentVersion()
                         , RevisionWrapper(res.revision, "&"))
         , resource(), root);
    def.geodataUrl = prependRoot
        (utility::format("{lod}-{x}-{y}.geo?gr=%d%s&viewspec={viewspec}"
                         , GeneratorRevision
                         , RevisionWrapper(res.revision, "&"))
         , resource(), root);
    def.style = styleUrl();

    def.displaySize = definition_.displaySize;
    def.lodRange = res.lodRange;
    def.tileRange = res.tileRange;
    fl.credits = asInlineCredits(res);
    def.options = definition_.options;

    // done
    return fl;
}

vts::MapConfig GeodataVectorTiled::mapConfig_impl(ResourceRoot root)
    const
{
    const auto &res(resource());

    vts::MapConfig mapConfig;
    mapConfig.referenceFrame = *res.referenceFrame;
    mapConfig.srs = vr::listSrs(*res.referenceFrame);

    // add free layer into list of free layers
    mapConfig.freeLayers.add
        (vr::FreeLayer
         (res.id.fullId()
          , prependRoot(std::string("freelayer.json"), resource(), root)));

    // add free layer into view
    mapConfig.view.freeLayers[res.id.fullId()];

    if (definition_.introspection.surface) {
        LOG(info1) << "trying to find surface";
        if (auto other = otherGenerator
            (Resource::Generator::Type::surface
             , addReferenceFrame(*definition_.introspection.surface
                                 , referenceFrameId())))
        {
            mapConfig.merge(other->mapConfig
                            (resolveRoot(resource(), other->resource())));
        }
    }

    if (definition_.introspection.position) {
        mapConfig.position = *definition_.introspection.position;
    }

    // browser options (must be Json::Value!); overrides browser options from
    // surface's introspection
    if (!definition_.introspection.browserOptions.empty()) {
        mapConfig.browserOptions = definition_.introspection.browserOptions;
    }

    // done
    return mapConfig;
}

void GeodataVectorTiled::generateMetatile(Sink &sink
                                          , const GeodataFileInfo &fi
                                          , Arsenal &arsenal) const
{
    sink.checkAborted();

    if (!index_->meta(fi.tileId)) {
        sink.error(utility::makeError<NotFound>("Metatile not found."));
        return;
    }

    if (store_) {
        if (auto metatile = metatileFromStore
            (fi.tileId, *store_, resource(), index_->tileIndex
             , dem_.geoidGrid, definition_.displaySize))
        {
            std::ostringstream os;
            metatile->save(os);
            sink.content(os.str(), fi.sinkFileInfo());
            return;
        }

        LOG(warn3)
            << "Generator for <" << id()
            << ">: metanode store could not serve geodata metatile "
            << fi.tileId << "; falling back to warp.";
    }

    checkMetatileSource();

    auto metatile(metatileFromDem
                  (fi.tileId, sink, arsenal, resource()
                   , index_->tileIndex, dem_.dataset
                   , dem_.geoidGrid
                   , MaskTree(), definition_.displaySize));

    // write metatile to stream
    std::ostringstream os;
    metatile.save(os);
    sink.content(os.str(), fi.sinkFileInfo());
}

void GeodataVectorTiled::openMetanodeStore()
{
    store_.reset();
    warpFallbackAvailable_ = legacyDemMetatileInputsAvailable(dem_.dataset);

    MetanodeStoreConfig storeConfig;
    storeConfig.id = id().fullId();
    storeConfig.datasetDir = absoluteDataset(definition_.dem.dataset);
    storeConfig.root = root();
    storeConfig.referenceFrame = resource().id.referenceFrame;
    storeConfig.metaBinaryOrder = referenceFrame().metaBinaryOrder;
    storeConfig.metaDepth = 1;
    storeConfig.geoidGrid = dem_.geoidGrid;
    storeConfig.heightFunction = std::string();
    storeConfig.hasMask = false;
    storeConfig.lodRange = resource().lodRange;

    store_ = ::openMetanodeStore(storeConfig);
}

void GeodataVectorTiled::checkMetatileSource() const
{
    if (store_ || warpFallbackAvailable_) { return; }

    LOGTHROW(err2, std::runtime_error)
        << "Generator for <" << id()
        << ">: no valid metanode store and legacy dem.min/dem.max "
        "pyramids are not available; cannot serve geodata metatiles.";
}

void GeodataVectorTiled::generateGeodata(Sink &sink
                                         , const GeodataFileInfo &fi
                                         , Arsenal &arsenal) const
{
    const auto &tileId(fi.tileId);
    auto flags(index_->tileIndex.get(tileId));
    if (!vts::TileIndex::Flag::isReal(flags)) {
        sink.error(utility::makeError<NotFound>("No geodata for this tile."));
        return;
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.productive()) {
        sink.error(utility::makeError<NotFound>
                   ("TileId outside of valid reference frame tree."));
        return;
    }

    auto sourceTileId(tileId);
    auto sourceLocalId(vts::local(nodeInfo.rootLod(), sourceTileId));
    auto tileExtents(nodeInfo.extents());

    bool cutting(false);

    if (definition_.maxSourceLod && (sourceLocalId.lod
                                     > *definition_.maxSourceLod))
    {
        // shift source to proper layer
        auto lodDiff(sourceLocalId.lod - *definition_.maxSourceLod);
        sourceTileId = vts::parent(sourceTileId, lodDiff);
        sourceLocalId = vts::parent(sourceLocalId, lodDiff);
        tileExtents = vts::NodeInfo(referenceFrame(), sourceTileId).extents();
        cutting = true;
    }

    const auto tileFile
        (absoluteDataset
         (tileFile_(vts::UrlTemplate::Vars(sourceTileId, sourceLocalId))));

    LOG(info1) << "Using geo file: <" << tileFile << ">.";

    // combine all dem datasets and default/fallback dem dataset
    auto datasets(viewspec2datasets(fi.fileInfo.query, dem_));

    geo::heightcoding::Config config;
    config.workingSrs = sds(nodeInfo, dem_.geoidGrid);
    config.outputSrs = boost::in_place
        (physicalSrs_.srsDef, physicalSrs_.adjustVertical());
    config.layers = definition_.layers;
    config.format = definition_.format;
    config.formatConfig = definition_.formatConfig;
    config.mode = definition_.mode;
    config.schema = definition_.schema;
    
    LOG(debug) << "Schema: " << config.schema;

    if (cutting) {
        // clip whole tile to node extents
        config.clipWorkingExtents = nodeInfo.extents();
    } else if (definition_.clipLayers) {
        // clip only given layers
        config.clipWorkingExtents = nodeInfo.extents();
        config.clipLayers = definition_.clipLayers;
    }

    // build open options for MVT driver
    GdalWarper::OpenOptions openOptions;
    {
        std::ostringstream os;
        os << "@MVT_EXTENTS=" << std::fixed << tileExtents;
        openOptions.push_back(os.str());

        os.str("");
        os << "@MVT_SRS=" << *config.workingSrs;
        openOptions.push_back(os.str());
    }

    // heightcode data using warper's machinery
    LOG(info1) << "Heightcoding.";
    auto hc(arsenal.warper.heightcode
            (tileFile, datasets.first, config, dem_.geoidGrid
             , openOptions, layerEnhancers(), sink));

    // force 1 hour max age if not all views from viewspec have been found
    boost::optional<long> maxAge;
    if (!datasets.second) { maxAge = 3600; }

    sink.content(hc->data, hc->size
                 , fi.sinkFileInfo().setMaxAge(maxAge), true);
}

} // namespace generator
