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
#include <boost/utility/in_place_factory.hpp>
#include <boost/lexical_cast.hpp>

#include <opencv2/highgui/highgui.hpp>

#include "utility/raise.hpp"
#include "utility/path.hpp"
#include "utility/gzipper.hpp"
#include "utility/cppversion.hpp"

#include "imgproc/rastermask/cvmat.hpp"
#include "imgproc/png.hpp"

#include "vts-libs/registry/io.hpp"
#include "vts-libs/storage/fstreams.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/tileset/config.hpp"
#include "vts-libs/vts/metatile.hpp"
#include "vts-libs/vts/csconvertor.hpp"
#include "vts-libs/vts/math.hpp"
#include "vts-libs/vts/mesh.hpp"
#include "vts-libs/vts/opencv/navtile.hpp"
#include "vts-libs/vts/types2d.hpp"
#include "vts-libs/vts/qtree-rasterize.hpp"
#include "vts-libs/vts/2d.hpp"
#include "vts-libs/vts/debug.hpp"
#include "vts-libs/vts/mapconfig.hpp"
#include "vts-libs/vts/service.hpp"

#include "../error.hpp"
#include "../support/metatile.hpp"
#include "../support/mesh.hpp"
#include "../support/srs.hpp"
#include "../support/grid.hpp"
#include "../support/mmapped/qtree-rasterize.hpp"
#include "../support/tilejson.hpp"
#include "../support/cesiumconf.hpp"
#include "../support/revision.hpp"
#include "../support/tms.hpp"
#include "../support/introspection.hpp"
#include "../support/atlas.hpp"

#include "files.hpp"
#include "surface.hpp"
#include "providers.hpp"

namespace fs = boost::filesystem;
//namespace bio = boost::iostreams;
namespace vr = vtslibs::registry;
namespace vs = vtslibs::storage;
namespace vts = vtslibs::vts;

namespace generator {

namespace {

Generator::Properties
terrainSupport(const Generator::Params &params)
{
    Generator::Properties props;
    if (!params.resource.referenceFrame->findExtension<vre::Tms>()) {
        return props;
    }
    return props.support(GeneratorInterface::Interface::terrain);
}

} // namespace

class SurfaceProvider
    : public Generator::Provider
    , public VtsTilesetProvider
{
public:
    SurfaceProvider(SurfaceBase &surface)
        : surface_(surface)
    {}

private:
    Generator::Task mesh_impl(const vts::TileId &tileId, Sink&
                              , const SurfaceFileInfo &fileInfo
                              , vts::SubMesh::TextureMode textureMode)
        const override
    {
        return [=](Sink &sink, Arsenal &arsenal) {
            surface_.generateMesh
                (tileId, sink, fileInfo, arsenal, textureMode);
        };
    }

    Generator::Task metatile_impl(const vts::TileId &tileId, Sink&
                                  , const SurfaceFileInfo &fileInfo
                                  , const MetatileOverrides &overrides)
        const override
    {
        return [=](Sink &sink, Arsenal &arsenal) {
            if (fileInfo.flavor == vts::FileFlavor::debug) {
                // debug metanode
                surface_.generateDebugNode
                    (tileId, sink, fileInfo, arsenal, overrides.textureMode);
            } else {
                // real metatile
                surface_.generateMetatile
                    (tileId, sink, fileInfo, arsenal, overrides);
            }
        };
    }

    Generator::Task file_impl(const FileInfo &fileInfo, Sink sink)
        const override
    {
        return surface_.generateFile(fileInfo, sink);
    }

    /** Returns path to VTS file if supported.
     */
    boost::optional<fs::path> path_impl(vts::File file) const override
    {
        switch (file) {
        case vts::File::config:
        case vts::File::tileIndex:
            break;

        default:
            return boost::none;
        }

        return surface_.filePath(file);
    }

    vts::FullTileSetProperties properties_impl() const override {
        return surface_.properties_;
    }

    SurfaceBase &surface_;
};

fs::path SurfaceBase::filePath(vts::File fileType) const
{
    switch (fileType) {
    case vts::File::config:
        return root() / "tileset.conf";
    case vts::File::tileIndex:
        return root() / "tileset.index";
    default: break;
    }

    throw utility::makeError<InternalError>("Unsupported file");
}

SurfaceBase::SurfaceBase(const Params &params)
    : Generator(params, terrainSupport(params))
    , definition_(resource().definition<Definition>())
    , tms_(params.resource.referenceFrame->findExtension<vre::Tms>())
{
    setProvider(std::make_unique<SurfaceProvider>(*this));
}

bool SurfaceBase::loadFiles(const Definition &definition)
{
    if (changeEnforced()) {
        LOG(info1) << "Generator for <" << id() << "> not ready.";
        return false;
    }

    try {
        auto indexPath(filePath(vts::File::tileIndex));
        auto deliveryIndexPath(root() / "delivery.index");
        auto propertiesPath(filePath(vts::File::config));
        if (fs::exists(indexPath) && fs::exists(propertiesPath)) {
            // both paths exist -> ok
            properties_ = vts::tileset::loadConfig(propertiesPath);
            if (updateProperties(definition)) {
                // something changed in properties, update
                vts::tileset::saveConfig(filePath(vts::File::config)
                                         , properties_);
            }

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
            // it is now up to child class to handle this
            // makeReady();
            return true;
        }
    } catch (const std::exception &e) {
        // not ready
    }

    LOG(info1) << "Generator for <" << id() << "> not ready.";
    return false;
}

bool SurfaceBase::updateProperties(const Definition &def)
{
    bool changed(false);

    if (properties_.nominalTexelSize != def.nominalTexelSize) {
        properties_.nominalTexelSize = def.nominalTexelSize;
        changed = true;
    }

    if (def.mergeBottomLod) {
        if (properties_.mergeBottomLod != *def.mergeBottomLod) {
            properties_.mergeBottomLod = *def.mergeBottomLod;
            changed = true;
        }
    } else if (properties_.mergeBottomLod) {
        properties_.mergeBottomLod = 0;
        changed = true;
    }

    // update revision if changed
    if (resource().revision > properties_.revision) {
        properties_.revision = resource().revision;
        changed = true;
    }

    return changed;
}

Generator::Task SurfaceBase
::generateFile_impl(const FileInfo &fileInfo, Sink &sink) const
{
    // handle special case
    switch (fileInfo.interface.interface) {
    case GeneratorInterface::Interface::vts: break;
    case GeneratorInterface::Interface::terrain:
        return terrainInterface(fileInfo, sink);
    default:
        sink.error(utility::makeError<InternalError>
                   ("Surface resource has no <%s> interface."
                    , fileInfo.interface));
        return {};
    }

    SurfaceFileInfo fi(fileInfo);

    switch (fi.type) {
    case SurfaceFileInfo::Type::unknown:
        sink.error(utility::makeError<NotFound>("Unrecognized filename."));
        break;

    case SurfaceFileInfo::Type::definition: {
        auto fl(vts::freeLayer
                (vts::meshTilesConfig
                 (properties_, vts::ExtraTileSetProperties()
                  , prependRoot(fs::path(), resource(), ResourceRoot::none))));

        std::ostringstream os;
        vr::saveFreeLayer(os, fl);
        sink.content(os.str(), fi.sinkFileInfo());
        break;
    }

    case SurfaceFileInfo::Type::file: {
        switch (fi.fileType) {
        case vts::File::config: {
            switch (fi.flavor) {
            case vts::FileFlavor::regular: {
                std::ostringstream os;
                mapConfig(os, ResourceRoot::none);
                sink.content(os.str(), fi.sinkFileInfo());
                break;
            }

            case vts::FileFlavor::raw:
                sink.content(vs::fileIStream
                             (fi.fileType, filePath(vts::File::config))
                             , FileClass::unknown);
                break;

            case vts::FileFlavor::debug: {
                std::ostringstream os;
                const auto debug
                    (vts::debugConfig
                     (vts::meshTilesConfig
                      (properties_, vts::ExtraTileSetProperties()
                       , prependRoot(fs::path(), resource()
                                     , ResourceRoot::none))));
                vts::saveDebug(os, debug);
                sink.content(os.str(), fi.sinkFileInfo());
                break;
            }

            default:
                sink.error(utility::makeError<NotFound>
                           ("Unsupported file flavor %s.", fi.flavor));
                break;
            }
            break;
        }

        case vts::File::tileIndex:
            sink.content(vs::fileIStream
                          (fi.fileType, filePath(vts::File::tileIndex))
                         , FileClass::unknown);
            break;

        case vts::File::registry: {
            std::ostringstream os;
            save(os, resource().registry);
            sink.content(os.str(), fi.sinkFileInfo());
            break; }

        default:
            sink.error(utility::makeError<NotFound>("Not found"));
            break;
        }
        break;
    }

    case SurfaceFileInfo::Type::tile: {
        switch (fi.tileType) {
        case vts::TileFile::meta:
            return [=](Sink &sink, Arsenal &arsenal) {
                if (fi.flavor == vts::FileFlavor::debug) {
                    // debug metanode
                    generateDebugNode(fi.tileId, sink, fi, arsenal);
                } else {
                    // regular metatile
                    generateMetatile(fi.tileId, sink, fi, arsenal);
                }
            };

        case vts::TileFile::mesh:
            return [=](Sink &sink, Arsenal &arsenal) {
                generateMesh(fi.tileId, sink, fi, arsenal);
            };

        case vts::TileFile::normals:
            return [=](Sink &sink, Arsenal &arsenal) {
                generateNormalMap(fi.tileId, sink, fi, arsenal);
            };

        case vts::TileFile::atlas:
            sink.error(utility::makeError<NotFound>
                        ("No internal texture present."));
            break;

        case vts::TileFile::navtile:
            return [=](Sink &sink, Arsenal &arsenal) {
                generateNavtile(fi.tileId, sink, fi, arsenal);
            };
            break;

        case vts::TileFile::meta2d:
            return [=](Sink &sink, Arsenal &arsenal) {
                generate2dMetatile(fi.tileId, sink, fi, arsenal);
            };
            break;

        case vts::TileFile::mask:
            return [=](Sink &sink, Arsenal &arsenal) {
                generate2dMask(fi.tileId, sink, fi, arsenal);
            };
            break;

        case vts::TileFile::ortho:
            sink.error(utility::makeError<NotFound>
                        ("No orthophoto present."));
            break;

        case vts::TileFile::credits:
            return [=](Sink &sink, Arsenal &arsenal) {
                generateCredits(fi.tileId, sink, fi, arsenal);
            };
            break;
        }
        break;
    }

    case SurfaceFileInfo::Type::support:
        supportFile(*fi.support, sink, fi.sinkFileInfo());
        break;

    case SurfaceFileInfo::Type::registry:
        sink.content(vs::fileIStream
                      (fi.registry->contentType, fi.registry->path)
                     , FileClass::registry);
        break;

    case SurfaceFileInfo::Type::service:
        sink.content(vts::service::generate
                     (fi.serviceFile, fi.fileInfo.filename, fi.fileInfo.query)
                     , FileClass::data);
        break;

    default:
        sink.error(utility::makeError<InternalError>
                    ("Not implemented yet."));
    }

    return {};
}

void SurfaceBase::generateMesh(const vts::TileId &tileId
                               , Sink &sink
                               , const SurfaceFileInfo &fi
                               , Arsenal &arsenal
                               , vts::SubMesh::TextureMode textureMode) const
{
    auto flags(index_->tileIndex.get(tileId));
    if (!vts::TileIndex::Flag::isReal(flags)) {
        utility::raise<NotFound>("No mesh for this tile.");
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.productive()) {
        utility::raise<NotFound>
            ("TileId outside of valid reference frame tree.");
    }

    // generate the actual mesh
    auto lm(generateMeshImpl(nodeInfo, sink, arsenal));

    // and add skirt
    addSkirt(lm.mesh, nodeInfo);

    const auto raw(fi.flavor == vts::FileFlavor::raw);

    // generate VTS mesh
    vts::Mesh mesh(false);
    if (!lm.mesh.vertices.empty()) {
        // local mesh is valid -> add as a submesh into output mesh
        auto &sm(addSubMesh(mesh, lm.mesh, nodeInfo, lm.geoidGrid
                            , textureMode));
        if (lm.textureLayerId) {
            sm.textureLayer = lm.textureLayerId;
        }

        if (raw) {
            // we are returning full mesh file -> generate coverage mask
            meshCoverageMask
                (mesh.coverageMask, lm.mesh, nodeInfo, lm.fullyCovered);
        }
    }

    // write mesh to stream
    std::stringstream os;
    auto sfi(fi.sinkFileInfo());
    if (raw) {
        vts::saveMesh(os, mesh);
    } else {
        vts::saveMeshProper(os, mesh);
        if (vs::gzipped(os)) {
            // gzip -> mesh
            sfi.addHeader("Content-Encoding", "gzip");
        }
    }

    sink.content(os.str(), sfi);
}


void SurfaceBase::generateNormalMap(const vts::TileId &tileId
                                    , Sink &sink
                                    , const SurfaceFileInfo &fi
                                    , Arsenal &arsenal) const {
    // check availability
    auto flags(index_->tileIndex.get(tileId));
    if (!vts::TileIndex::Flag::isReal(flags)) {
        utility::raise<NotFound>("No mesh (or  normal map) for this tile.");
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.productive()) {
        utility::raise<NotFound>
            ("TileId outside of valid reference frame tree.");
    }

    // generate normal map
    cv::Mat normalMap = generateNormalMapImpl(nodeInfo, sink, arsenal);

    // convert normal map to physical srs
    const auto conv(sds2phys(nodeInfo, definition_.getGeoidGrid()));
    if (!conv) { utility::raise<InternalError>("Conversion failed."); }

    bool optimize = false;

    if (tileId.lod > 3) {
        // FIXME: we optimize normals for lods starting with 4, when tiles no
        // longer span greater parts of hemispheres. A more abstract
        // approach based on the reference frames specification would
        // be more rigorous.
        optimize = true;
    }

    geo::normalmap::convertNormals(
        normalMap, nodeInfo.extents(), conv.conv(), optimize);

    // obtain the final image, write to stream
    auto sfi(fi.sinkFileInfo());

    cv::Mat img = geo::normalmap::exportToBGR(normalMap);
    sendImage(img, sfi, RasterNormalMapFormat, false, sink);
}

cv::Mat SurfaceBase::generateNormalMapImpl(
    const vts::NodeInfo &, Sink &, Arsenal &) const {

    utility::raise<NotFound>("Normal maps not provided by this generator.");
    return cv::Mat();
}


void SurfaceBase::generate2dMask(const vts::TileId &tileId
                                 , Sink &sink
                                 , const SurfaceFileInfo &fi
                                 , Arsenal &arsenal) const
{
    const auto debug(fi.flavor == vts::FileFlavor::debug);

    auto flags(index_->tileIndex.get(tileId));
    if (!vts::TileIndex::Flag::isReal(flags)) {
        if (debug) {
            return sink.error(utility::makeError<EmptyDebugMask>
                              ("No mesh for this tile."));
        }
        return sink.error(utility::makeError<NotFound>
                          ("No mesh for this tile."));
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.productive()) {
        if (debug) {
            return sink.error(utility::makeError<EmptyDebugMask>
                              ("No mesh for this tile."));
        }
        return sink.error(utility::makeError<NotFound>
                          ("TileId outside of valid reference frame tree."));
    }

    // by default full watertight mesh
    vts::MeshMask mask;
    mask.createCoverage(true);

    if (!vts::TileIndex::Flag::isWatertight(flags)) {
        auto lm(generateMeshImpl(nodeInfo, sink, arsenal));
        meshCoverageMask
            (mask.coverageMask, lm.mesh, nodeInfo, lm.fullyCovered);
    }

    if (debug) {
        sink.content(imgproc::png::serialize
                     (vts::debugMask(mask.coverageMask, { 1 }), 9)
                     , fi.sinkFileInfo());
    } else {
        sink.content(imgproc::png::serialize
                     (vts::mask2d(mask.coverageMask, { 1 }), 9)
                     , fi.sinkFileInfo());
    }
}

void SurfaceBase::generate2dMetatile(const vts::TileId &tileId
                                     , Sink &sink
                                     , const SurfaceFileInfo &fi
                                     , Arsenal&) const

{
    sink.content(imgproc::png::serialize
                 (vts::meta2d(index_->tileIndex, tileId), 9)
                 , fi.sinkFileInfo());
}

void SurfaceBase::generateCredits(const vts::TileId&
                                  , Sink &sink
                                  , const SurfaceFileInfo &fi
                                  , Arsenal&) const
{
    vts::CreditTile creditTile;
    creditTile.credits = asInlineCredits(resource());

    std::ostringstream os;
    saveCreditTile(os, creditTile, true);
    sink.content(os.str(), fi.sinkFileInfo());
}

void SurfaceBase::generateDebugNode(const vts::TileId &tileId
                                    , Sink &sink
                                    , const SurfaceFileInfo &fi
                                    , Arsenal &
                                    , vts::SubMesh::TextureMode textureMode)
    const
{
    // generate debug metanode
    const auto debugNode([&]()
    {
        if (textureMode == vts::SubMesh::external) {
            return vts::getNodeDebugInfo(index_->tileIndex, tileId);
        }

        // internal texture -> add atlas flag
        return vts::getNodeDebugInfo
            (index_->tileIndex, tileId
             , [](vts::TileIndex::Flag::value_type f) {
                return (vts::TileIndex::Flag::isReal(f)
                        ? (f | vts::TileIndex::Flag::atlas) : f);
            });
    }());

    std::ostringstream os;
    vts::saveDebug(os, debugNode);
    sink.content(os.str(), fi.sinkFileInfo());
}

vts::ExtraTileSetProperties
SurfaceBase::extraProperties(const Definition &def) const
{
    vts::ExtraTileSetProperties extra;

    const auto &findResource([this](Resource::Generator::Type type
                                    , const Resource::Id &id)
                             -> const Resource*
    {
        if (auto other = otherGenerator
            (type, addReferenceFrame(id, referenceFrameId())))
        {
            return &other->resource();
        }
        return nullptr;
    });

    const auto &r(resource());

    if (def.introspection.tms.empty()) {
        introspection::add
            (extra, Resource::Generator::Type::tms, introspection::LocalLayer
             ({}, systemGroup(), "tms-raster-patchwork")
             , r, findResource);
    } else {
        introspection::add(extra, Resource::Generator::Type::tms
                           , def.introspection.tms, r, findResource);
    }

    introspection::add(extra, Resource::Generator::Type::geodata
                       , def.introspection.geodata, r, findResource);

    if (def.introspection.position) {
        extra.position = *def.introspection.position;
    }

    // browser options (must be Json::Value!)
    extra.browserOptions = def.introspection.browserOptions;

    return extra;
}

const vre::Tms& SurfaceBase::getTms() const
{
    if (!tms_) {
        utility::raise<NotFound>
            ("Terrain provider interface disabled, no <tms> extension in "
             "reference frame <%s>.", referenceFrameId());
    }

    return *tms_;
}

Generator::Task SurfaceBase
::terrainInterface(const FileInfo &fileInfo, Sink &sink) const
{
    const auto &tms(getTms());

    TerrainFileInfo fi(fileInfo);

    switch (fi.type) {
    case TerrainFileInfo::Type::unknown:
        sink.error(utility::makeError<NotFound>("Unrecognized filename."));
        break;

    case TerrainFileInfo::Type::tile:
        return [=](Sink &sink, Arsenal &arsenal) {
            generateTerrain(fi.tileId, sink, fi, arsenal, tms);
        };

    case TerrainFileInfo::Type::definition:
        layerJson(sink, fi, tms);
        break;

    case TerrainFileInfo::Type::support:
        supportFile(*fi.support, sink, fi.sinkFileInfo());
        break;

    case TerrainFileInfo::Type::cesiumConf:
        cesiumConf(sink, fi, tms);
        break;

    case TerrainFileInfo::Type::listing:
        sink.listing(fi.listing, "", markdown(cesiumReadme()));
        break;

    case TerrainFileInfo::Type::readme:
        sink.markdown(utility::format("%s: Terrain Readme", id().fullId())
                      , cesiumReadme());
        break;

    default:
        sink.error(utility::makeError<InternalError>
                    ("Not implemented yet."));
    }

    return {};
}

void SurfaceBase::generateTerrain(const vts::TileId &tmsTileId
                                  , Sink &sink
                                  , const TerrainFileInfo &fi
                                  , Arsenal &arsenal
                                  , const vre::Tms &tms) const
{
    // remap id from TMS to VTS
    const auto tileId(tms2vts(tms.rootId, tms.flipY, tmsTileId));

    const auto &zerotile([&]() -> bool
    {
        // level 0 is always generated
        if (tmsTileId.lod > 0) {
            // other non-defined levels are base on tile's presence above
            // existing tiles (computed in VTS system!)
            const auto &r(resource());
            // shift resource's tile range to this lod
            const auto range
                (vts::shiftRange(r.lodRange.min, r.tileRange, tileId.lod));
            // and check for tile's incidence
            if (!math::inside(range, tileId.x, tileId.y)) { return false; }
        }

        vts::NodeInfo nodeInfo(referenceFrame(), tileId);
        if (!nodeInfo.productive()) {
            utility::raise<NotFound>
                ("TileId outside of valid reference frame tree.");
        }

        // some toplevel tile -> zero tile
        auto lm(meshFromNode(nodeInfo, math::Size2(10, 10)));

        // write mesh to stream (gzipped)
        std::ostringstream os;
        qmf::save(qmfMesh(lm.mesh, nodeInfo
                          , (tms.physicalSrs ? *tms.physicalSrs
                             : referenceFrame().model.physicalSrs)
                          , definition_.getGeoidGrid())
                  , utility::Gzipper(os), fi.fileInfo.filename);

        auto sfi(fi.sinkFileInfo());
        sfi.addHeader("Content-Encoding", "gzip");
        sink.content(os.str(), sfi);

        return true;
    });

    auto flags(index_->tileIndex.get(tileId));
    if (!vts::TileIndex::Flag::isReal(flags)) {
        if (zerotile()) { return; }
        utility::raise<NotFound>("No terrain for this tile.");
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.productive()) {
        utility::raise<NotFound>
            ("TileId outside of valid reference frame tree.");
    }

    // generate the actual mesh; replace all no-data values with zero
    auto lm(generateMeshImpl(nodeInfo, sink, arsenal, 0.0));

    // write mesh to stream (gzipped)
    std::ostringstream os;
    qmf::save(qmfMesh(lm.mesh, nodeInfo
                      , (tms.physicalSrs ? *tms.physicalSrs
                         : referenceFrame().model.physicalSrs)
                      , lm.geoidGrid)
              , utility::Gzipper(os), fi.fileInfo.filename);

    auto sfi(fi.sinkFileInfo());
    sfi.addHeader("Content-Encoding", "gzip");
    sink.content(os.str(), sfi);
}

namespace {

struct TerrainBound {
    LayerJson::Available available;
    math::Extents2 bounds;

    TerrainBound() : bounds(math::InvalidExtents{}) {}
};

TerrainBound terrainBounds(const Resource &r, const vre::Tms &tms)
{
    TerrainBound tb;

    // ensure we have top-level tiles "available"
    if (tms.rootId.lod < r.lodRange.min) {
        tb.available.emplace_back();
        auto &current(tb.available.back());

        const auto lod(tms.rootId.lod);
        for (const auto &block : metatileBlocks
                 (*r.referenceFrame, vts::TileId(lod, 0, 0), lod, false))
        {
            const auto tmsRange(vts2tms(tms.rootId, tms.flipY
                                        , vts::LodTileRange(lod, block.view)));
            current.push_back(tmsRange.range);
        }
    }

    // all other lods between top-level and first data lod are empty
    for (auto lod(tms.rootId.lod + 1); lod < r.lodRange.min; ++lod) {
        tb.available.emplace_back();
    }

    const auto physicalSrs(tms.physicalSrs ? *tms.physicalSrs
                           : r.referenceFrame->model.physicalSrs);
    for (const auto &range : vts::Ranges(r.lodRange, r.tileRange).ranges()) {
        tb.available.emplace_back();
        auto &current(tb.available.back());
        const auto tmsRange(vts2tms(tms.rootId, tms.flipY, range));
        current.push_back(tmsRange.range);

        // treat current LOD as one gigantic metatile
        for (const auto &block : metatileBlocks(r, vts::TileId(range.lod, 0, 0)
                                                , range.lod, false))
        {
            const vts::CsConvertor conv(block.srs, physicalSrs);
            math::update(tb.bounds, conv(block.extents));
        }
    }

    return tb;
}

} // namespace

void SurfaceBase::layerJson(Sink &sink, const TerrainFileInfo &fi
                            , const vre::Tms &tms) const
{
    LayerJson layer;
    const auto &r(resource());

    layer.name = id().fullId();
    layer.description = r.comment;

    // use revision as major version (plus 1)
    layer.version.maj = r.revision + 1;
    layer.format = "quantized-mesh-1.0";
    layer.scheme = LayerJson::Scheme::tms;
    layer.tiles.push_back
        (utility::format("{z}-{x}-{y}.terrain%s"
                         , RevisionWrapper(r.revision, "?")));
    layer.projection = tms.projection;

    // fixed LOD range
    layer.zoom.min = 0; // r.lodRange.min - tms.rootId.lod;
    layer.zoom.max = r.lodRange.max - tms.rootId.lod;

    auto tb(terrainBounds(r, tms));
    layer.available = std::move(tb.available);
    layer.bounds = std::move(tb.bounds);

    if (!r.credits.empty()) {
        layer.attribution
            = boost::lexical_cast<std::string>
            (utility::join(html(asInlineCredits(r)), "<br/>"));
    }

    std::ostringstream os;
    save(layer, os);
    sink.content(os.str(), fi.sinkFileInfo());
}

void SurfaceBase::cesiumConf(Sink &sink, const TerrainFileInfo &fi
                             , const vre::Tms &tms) const
{
    const auto &findResource([this](Resource::Generator::Type type
                                    , const Resource::Id &id)
                             -> const Resource*
    {
        if (auto other = otherGenerator
            (type, addReferenceFrame(id, referenceFrameId())))
        {
            return &other->resource();
        }
        return nullptr;
    });

    CesiumConf conf;
    conf.tms = tms;

    if (definition_.introspection.tms.empty()) {
        if (const auto intro = introspection::remote
            (Resource::Generator::Type::tms
             , Resource::Id({}, systemGroup(), "tms-raster-patchwork")
             , resource(), findResource))
        {
            conf.boundLayer = intro->url;
        }
    } else if (const auto intro = introspection::remote
               (Resource::Generator::Type::tms
                , definition_.introspection.tms.front()
                , resource(), findResource))
    {
        conf.boundLayer = intro->url;
    }

    const auto tb(terrainBounds(resource(), tms));
    conf.defaultView = tb.bounds;

    std::ostringstream os;
    save(conf, os);
    sink.content(os.str(), fi.sinkFileInfo());
}

std::string SurfaceBase::cesiumReadme() const
{
    vs::SupportFile::Vars vars;
    vars["externalUrl"] = config().externalUrl;
    vars["url"] = url(GeneratorInterface::Interface::terrain);

    return files::cesiumReadme.expand(&vars, nullptr);
}

} // namespace generator
