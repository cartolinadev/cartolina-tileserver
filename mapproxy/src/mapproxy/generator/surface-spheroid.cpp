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

#include <new>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <opencv2/highgui/highgui.hpp>

#include "utility/premain.hpp"
#include "utility/raise.hpp"
#include "utility/path.hpp"

#include "geometry/mesh.hpp"

#include "geo/coordinates.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "vts-libs/storage/fstreams.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/tileset/config.hpp"
#include "vts-libs/vts/metatile.hpp"
#include "vts-libs/vts/csconvertor.hpp"
#include "vts-libs/vts/math.hpp"
#include "vts-libs/vts/mesh.hpp"
#include "vts-libs/vts/opencv/navtile.hpp"
#include "vts-libs/vts/service.hpp"

#include "../error.hpp"
#include "../support/metatile.hpp"
#include "../support/mesh.hpp"
#include "../support/srs.hpp"
#include "../support/grid.hpp"

#include "surface-spheroid.hpp"
#include "factory.hpp"

namespace fs = boost::filesystem;
namespace vr = vtslibs::registry;
namespace vs = vtslibs::storage;
namespace vts = vtslibs::vts;

namespace generator {

namespace {

unsigned int GeneratorRevision(1);

struct Factory : Generator::Factory {
    virtual Generator::pointer create(const Generator::Params &params)
    {
        return std::make_shared<SurfaceSpheroid>(params);
    }

    virtual bool systemInstance() const { return true; }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType<SurfaceSpheroid>(std::make_shared<Factory>());
});

} // namespace

SurfaceSpheroid::SurfaceSpheroid(const Params &params)
    : SurfaceBase(params)
    , definition_(resource().definition<Definition>())
{
    // TODO: do not load existing file if system and something changed
    if (loadFiles(definition_)) {
        makeReady();
    }

}

void SurfaceSpheroid::prepare_impl(Arsenal&)
{
    LOG(info2) << "Preparing <" << id() << ">.";

    const auto &r(resource());

    // build properties
    properties_ = {};
    properties_.id = r.id.fullId();
    properties_.referenceFrame = r.referenceFrame->id;
    properties_.credits = asIntSet(r.credits);
    if (definition_.textureLayerId) {
        properties_.boundLayers.insert(definition_.textureLayerId);
    }
    // keep driverOptions empty -> no driver
    properties_.lodRange = r.lodRange;
    properties_.tileRange = r.tileRange;
    properties_.revision = r.revision;

    // create default position:
    // place to zero in navigation space
    // TODO: make center/barycenter of mask/dataset
    properties_.position.position = { 0.0, 0.0, 0.0 };
    // look down
    properties_.position.orientation = { 0.0, -90.0, 0.0 };
    // take Y size of reference frame's 3D extents extents
    properties_.position.verticalExtent
        = math::size(referenceFrame().division.extents).height;

    // yes, we provide normal maps
    properties_.hasNormalMaps = true;

    // no. we no longer provide a 2d interface
    properties_.has2dInterface = false;

    // no, we do not provide textures
    properties_.hasTextures = false;

    properties_.position.verticalFov = config().defaultFov;

    vts::tileset::Index index(referenceFrame().metaBinaryOrder);

    // grab and reset tile index
    auto &ti(index.tileIndex);
    ti = {};

    // build tile index
    // metatiles are distributed everywhere
    for (auto lod : r.lodRange) {
        // treat whole lod as a huge metatile and process each block
        // independently
        for (const auto &block
                 : metatileBlocks(resource(), vts::TileId(lod), lod, true))
        {
            LOG(info1) << "Generating tile index LOD <" << lod
                       << ">: ancestor: "
                       << block.commonAncestor.nodeId()
                       << "block: " << block.view << ".";

            if (block.commonAncestor.productive() && in(lod, r.lodRange)) {
                // mesh and navtile in valid area (If there are non-existent
                // tiles we'll get empty meshes and navtiles with empty masks.
                // This is lesser evil than to construct gargantuan tileindex
                // that would not fit in any imaginable memory)
                vts::TileIndex::Flag::value_type flags
                    (vts::TileIndex::Flag::mesh
                     | vts::TileIndex::Flag::watertight);

                // generate navtiles only to LOD 10 (arbitrary) but at least for
                // top-level LOD;
                if ((lod == r.lodRange.min) || (lod <= 10)) {
                    flags |= vts::TileIndex::Flag::navtile;
                }

                // set current block to computed value
                ti.set(lod, block.view, flags);
            }
        }
    }

    // save it all
    vts::tileset::saveConfig(filePath(vts::File::config), properties_);
    vts::tileset::saveTileSetIndex(index, filePath(vts::File::tileIndex));

    const auto deliveryIndexPath(root() / "delivery.index");
    // convert it to delivery index (using a temporary file)
    const auto tmpPath(utility::addExtension
                       (deliveryIndexPath, ".tmp"));
    mmapped::TileIndex::write(tmpPath, index.tileIndex);
    fs::rename(tmpPath, deliveryIndexPath);

    index_ = boost::in_place(referenceFrame().metaBinaryOrder
                             , deliveryIndexPath);
}

vts::MapConfig SurfaceSpheroid::mapConfig_impl(ResourceRoot root) const
{
    const auto path(prependRoot(fs::path(), resource(), root));

    auto mc(vts::mapConfig
            (properties_, resource().registry
             , extraProperties(definition_), path));

    // position
    if (!definition_.introspection.position) {
        // no introspection position, generate some

        // look down
        mc.position.orientation = { 0.0, -90.0, 0.0 };

        // take Y size of reference frame's 3D extents extents
        mc.position.verticalExtent
            = math::size(referenceFrame().division.extents).height;

        mc.position.verticalFov = config().defaultFov;
    }

    // add local services
    vts::service::addLocal(mc, path);

    return mc;
}

namespace {

typedef vts::MetaNode::Flag MetaFlag;
typedef vts::TileIndex::Flag TiFlag;

inline MetaFlag::value_type ti2metaFlags(TiFlag::value_type ti)
{
    MetaFlag::value_type meta(MetaFlag::allChildren);
    if (ti & TiFlag::mesh) {
        meta |= MetaFlag::geometryPresent;
    }
    if (ti & TiFlag::navtile) {
        meta |= MetaFlag::navtilePresent;
    }

    return meta;
}

/** NB: Do Not Change!
 *
 * This constant has huge impact on dataset stability. Changing this value break
 * data already served to the outer world.
 */
const int metatileSamplesPerTile(8);

} // namespace

void SurfaceSpheroid::generateMetatile(const vts::TileId &tileId
                                       , Sink &sink
                                       , const SurfaceFileInfo &fi
                                       , Arsenal&
                                       , const MetatileOverrides &overrides)
    const
{
    sink.checkAborted();

    if (!index_->meta(tileId)) {
        sink.error(utility::makeError<NotFound>("Metatile not found."));
        return;
    }

    auto blocks(metatileBlocks(resource(), tileId));

    if (blocks.empty()) {
        sink.error(utility::makeError<NotFound>
                    ("Metatile completely outside of configured range."));
        return;
    }

    const auto &rf(referenceFrame());

    vts::MetaTile metatile(tileId, rf.metaBinaryOrder);

    auto setChildren([&](const MetatileBlock &block, const vts::TileId &nodeId
                         , vts::MetaNode &node) -> void
    {
        if (block.commonAncestor.partial() || special(rf, nodeId)) {
            // partial node, update children flags
            for (const auto &child : vts::children(nodeId)) {
                node.setChildFromId
                    (child, vts::NodeInfo(rf, child).valid());
            }
        }
    });

    auto generateUnproductiveNodes([&](const MetatileBlock &block
                                       , const math::Size2 &bSize) -> void
    {
        const auto &view(block.view);
        for (int j(0), je(bSize.height); j < je; ++j) {
            for (int i(0), ie(bSize.width); i < ie; ++i) {
                // ID of current tile
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                // build metanode
                vts::MetaNode node;
                node.flags(MetaFlag::allChildren);
                setChildren(block, nodeId, node);
                metatile.set(nodeId, node);
            }
        }
    });

    // set internal texture count to 1 if we are generating internal textures
    const std::size_t internalTextureCount
        (overrides.textureMode == vts::SubMesh::internal);
    const auto credits(overrides.mergedCredits(resource().credits));

    for (const auto &block : blocks) {
        const auto &view(block.view);
        const auto &extents(block.extents);
        const auto es(math::size(extents));
        const math::Size2 bSize(vts::tileRangesSize(view));

        if (!block.commonAncestor.productive()) {
            // unproductive node
            generateUnproductiveNodes(block, bSize);
            continue;
        }

        const math::Size2 gridSize
            (bSize.width * metatileSamplesPerTile + 1
             , bSize.height * metatileSamplesPerTile + 1);

        LOG(info1) << "Processing metatile block ["
                   << vts::tileId(tileId.lod, block.view.ll)
                   << ", " << vts::tileId(tileId.lod, block.view.ur)
                   << "], ancestor: " << block.commonAncestor.nodeId()
                   << ", tile offset: " << block.offset;

        // grid (in grid coordinates); fill in with invalid numbers
        Grid<math::Point3> grid
            (gridSize, math::Point3(std::numeric_limits<double>::quiet_NaN()));

        // grid mask
        const ShiftMask mask(block, metatileSamplesPerTile);

        // tile size in grid and in real SDS
        math::Size2f gts
            (es.width / (metatileSamplesPerTile * bSize.width)
             , es.height / (metatileSamplesPerTile * bSize.height));
        math::Size2f ts(es.width / bSize.width
                        , es.height / bSize.height);

        auto conv(sds2phys(block.commonAncestor, definition_.geoidGrid));
        auto navConv(sds2nav(block.commonAncestor, definition_.geoidGrid));
        auto geConv(phys2sds(block.commonAncestor));

        // fill in matrix
        for (int j(0), je(gridSize.height); j < je; ++j) {
            auto y(extents.ur(1) - j * gts.height);
            for (int i(0), ie(gridSize.width); i < ie; ++i) {
                // work only with non-masked pixels
                if (mask(i, j)) {
                    grid(i, j)
                        = conv(math::Point3
                               (extents.ll(0) + i * gts.width, y, 0.0));
                }
            }
        }

        // generate metatile content
        for (int j(0), je(bSize.height); j < je; ++j) {
            for (int i(0), ie(bSize.width); i < ie; ++i) {
                // ID of current tile
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                // build metanode
                vts::MetaNode node;
                node.flags(ti2metaFlags(index_->tileIndex.get(nodeId)));
                bool geometry(node.geometry());
                bool navtile(node.navtile());

                // compute tile extents and height range
                auto heightRange(vs::Range<double>::emptyRange());
                // math::Extents3 te(math::InvalidExtents{});
                double area(0);
                int triangleCount(0);
                double avgHeightSum(0.f);
                int avgHeightCount(0);

                // process all node's vertices in grid
                for (int jj(0); jj <= metatileSamplesPerTile; ++jj) {
                    auto yy(j * metatileSamplesPerTile + jj);
                    for (int ii(0); ii <= metatileSamplesPerTile; ++ii) {
                        auto xx(i * metatileSamplesPerTile + ii);
                        const auto *p(grid(mask, xx, yy));

                        // update tile extents (if point valid)
                        if (p) {
                            // math::update(te, *p);
                            // convert point to proper SDS
                            const auto sdPoint(geConv(*p));
                            // update geom extents
                            vts::update(node.geomExtents, sdPoint);

                            // accumulate average height (surrogate) calculator
                            avgHeightSum += sdPoint(2);
                            ++avgHeightCount;
                        }

                        if (geometry && ii && jj) {
                            // compute area of the quad composed of 1 or 2
                            // triangles
                            auto qa(quadArea(grid(mask, xx - 1, yy - 1)
                                             , p
                                             , grid(mask, xx - 1, yy)
                                             , grid(mask, xx, yy - 1)));
                            area += std::get<0>(qa);
                            triangleCount += std::get<1>(qa);
                        }

                        if (p && navtile) {
                            // sample height in navtile
                            auto z(navConv
                                   (math::Point3
                                    (extents.ll(0) + xx * gts.width
                                     , yy, 0.0))(2));
                            update(heightRange, z);
                        }
                    }
                }

                setChildren(block, nodeId, node);

                if (geometry && !area) {
                    // well, empty tile, no children
                    continue;
                }

                // set extents
                // node.extents = vr::normalizedExtents(rf, te);

                // build height range
                node.heightRange.min = std::floor(heightRange.min);
                node.heightRange.max = std::ceil(heightRange.max);

                // set credits
                node.updateCredits(credits);

                // texturing
                node.internalTextureCount(internalTextureCount);

                // mesh is (almost) flat -> use tile area
                if (geometry) {
                    node.applyTexelSize(true);

                    // calculate texture size using node mask
                    auto textureArea([&]() -> double
                    {
                        // ancestor is full -> we are full as well
                        if (!block.commonAncestor.partial()) {
                            return vr::BoundLayer::tileArea();
                        }

                        // partial node: use triangle count to calculate
                        // percentage of texture
                        math::Size2 size(metatileSamplesPerTile
                                         , metatileSamplesPerTile);

                        // return scaled coverage; NB: triangle covers hald of
                        // pixel so real area is in pixels is half of number of
                        // pixels
                        return ((triangleCount * vr::BoundLayer::tileArea())
                                / (2.0 * math::area(size)));
                    }());

                    // well, empty tile as well
                    if (!textureArea) { continue; }

                    // calculate texel size
                    node.texelSize = std::sqrt(area / textureArea);

                    // surrogate
                    if (avgHeightCount) {
                        node.geomExtents.surrogate
                            = (avgHeightSum / avgHeightCount);
                    }
                }

                // store metata node
                metatile.set(nodeId, node);
            }
        }
    }

    // write metatile to stream
    std::ostringstream os;
    metatile.save(os);
    sink.content(os.str(), fi.sinkFileInfo());
}

AugmentedMesh
SurfaceSpheroid::generateMeshImpl(const vts::NodeInfo &nodeInfo, Sink &sink
                                  , Arsenal&, const OptHeight&) const
{
    // TODO: calculate tile sampling
    const int samplesPerSide(10);
    // const TileFacesCalculator tileFacesCalculator;

    sink.checkAborted();

    // generate mesh
    auto mesh
        (meshFromNode
         (nodeInfo, math::Size2(samplesPerSide, samplesPerSide)));
    mesh.textureLayerId = definition_.textureLayerId;
    mesh.geoidGrid = definition_.geoidGrid;

    // simplify
    // simplifyMesh(lm, nodeInfo, tileFacesCalculator);

    return mesh;
}

cv::Mat SurfaceSpheroid::generateNormalMapImpl(
    const vts::NodeInfo &, Sink &sink, Arsenal &) const {

    sink.checkAborted();

    // warp input dataset as DEM, at tile size + 1 pixel on each side

    // obtain normal map at spatial division coords
    auto normalMap = geo::normalmap::flatSurfaceNormals(
        math::Size2(256, 256), CV_32F);

    // return result
    return normalMap;
}


void SurfaceSpheroid::generateNavtile(const vts::TileId &tileId
                                      , Sink &sink
                                      , const SurfaceFileInfo &fi
                                      , Arsenal&) const
{
    sink.checkAborted();

    const auto &rf(referenceFrame());

    if (!index_->navtile(tileId)) {
        sink.error(utility::makeError<NotFound>("No navtile for this tile."));
        return;
    }

    vts::NodeInfo nodeInfo(rf, tileId);
    if (!nodeInfo.valid()) {
        sink.error(utility::makeError<NotFound>
                    ("TileId outside of valid reference frame tree."));
        return;
    }

    const auto &extents(nodeInfo.extents());
    const auto ts(math::size(extents));

    // sds -> navigation SRS convertor
    auto navConv(sds2nav(nodeInfo, definition_.geoidGrid));

    // first, calculate height range in the same way as is done in metatile
    auto heightRange(vs::Range<double>::emptyRange());
    {
        // create node doverage
        const auto coverage(nodeInfo.coverageMask
                            (vts::NodeInfo::CoverageType::grid
                             , math::Size2(metatileSamplesPerTile + 1
                                           , metatileSamplesPerTile + 1), 1));
        // grid pixel size
        math::Size2f gpx
            (ts.width / (metatileSamplesPerTile + 1)
             , ts.height / (metatileSamplesPerTile + 1));
        for (int j(0); j <= metatileSamplesPerTile; ++j) {
            auto y(extents.ll(1) + j * gpx.height);
            for (int i(0); i <= metatileSamplesPerTile; ++i) {
                if (!coverage.get(i, j)) { continue; }
                auto z(navConv
                   (math::Point3
                    (extents.ll(0) + i * gpx.width, y, 0.0))(2));
                update(heightRange, z);
            }
        }
    }

    // calculate navtile values
    vts::opencv::NavTile nt;
    auto ntd(nt.data());
    // generate coverage mask in grid coordinates
    auto &coverage(nt.coverageMask() = nodeInfo.coverageMask
                   (vts::NodeInfo::CoverageType::grid
                    , math::Size2(ntd.cols, ntd.rows), 1));

    // set height range
    nt.heightRange(vts::NavTile::HeightRange
                   (std::floor(heightRange.min), std::ceil(heightRange.max)));
    math::Size2f npx(ts.width / (ntd.cols - 1)
                     , ts.height / (ntd.rows - 1));
    for (int j(0); j < ntd.rows; ++j) {
        auto y(extents.ll(1) + j * npx.height);
        for (int i(0); i < ntd.cols; ++i) {
            // mask with node's mask
            if (!coverage.get(i, j)) { continue; }
            auto z(navConv
                   (math::Point3
                    (extents.ll(0) + i * npx.width, y, 0.0))(2));
            // write
            ntd.at<vts::opencv::NavTile::DataType>(j, i) = z;
        }
    }

    // done
    std::ostringstream os;
    if (fi.flavor == vts::FileFlavor::raw) {
        // raw navtile -> serialize to on-disk format
        nt.serialize(os);
    } else {
        // just navtile itself
        nt.serializeNavtileProper(os);
    }

    sink.content(os.str(), fi.sinkFileInfo());
}

unsigned int SurfaceSpheroid::generatorRevision() const {
    return GeneratorRevision;
}

} // namespace generator
