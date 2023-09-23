/**
 * Copyright (c) 2019 Melown Technologies SE
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

#include "mapproxy/resource.hpp"

#include "imgproc/png.hpp"
#include "imgproc/morphology.hpp"

#include "utility/raise.hpp"
#include "utility/format.hpp"
#include "utility/httpquery.hpp"

#include "../support/wmts.hpp"
#include "../support/revision.hpp"
#include "../support/mmapped/qtree-rasterize.hpp"
#include "../support/metatile.hpp"


#include "files.hpp"

#include "tms-raster-base.hpp"
#include "providers.hpp"
#include <boost/optional/detail/optional_swap.hpp>

namespace uq = utility::query;
namespace bgil = boost::gil;

namespace generator {

namespace {
Generator::Properties
wmtsSupport(const Generator::Params &params
            , const boost::optional<RasterFormat> &format)
{
    Generator::Properties props;
    if (!format) { return props; }
    if (!params.resource.referenceFrame->findExtension<vre::Wmts>()) {
        return props;
    }
    return props.support(GeneratorInterface::Interface::wmts);
}

} // namespace

class AtlasProvider
    : public Generator::Provider
    , public VtsAtlasProvider
{
public:
    AtlasProvider(TmsRasterBase &tms)
        : tms_(tms)
    {}

private:
    Generator::Task atlas_impl(const vts::TileId &tileId, Sink&
                               , const Sink::FileInfo &sfi
                               , bool atlas) const override
    {
        TmsRasterBase::ImageFlags imageFlags;
        imageFlags.forceFormat = true;
        imageFlags.atlas = atlas;
        return[=](Sink &sink, Arsenal &arsenal) {
            tms_.generateTileImage(tileId, sfi, RasterFormat::jpg
                                   , sink, arsenal, imageFlags);
        };
    }

    TmsRasterBase &tms_;
};

TmsRasterBase
::TmsRasterBase(const Params &params
    , const boost::optional<RasterFormat> format)
    : Generator(params, wmtsSupport(params, format))
    , wmts_(properties().isSupported(GeneratorInterface::Interface::wmts)
            ? params.resource.referenceFrame->findExtension<vre::Wmts>()
            : nullptr)
{
    setProvider(std::make_unique<AtlasProvider>(*this));
}

Generator::Task TmsRasterBase
::generateFile_impl(const FileInfo &fileInfo, Sink &sink) const
{
    // handle special case
    switch (fileInfo.interface.interface) {
    case GeneratorInterface::Interface::vts:
        return generateVtsFile_impl(fileInfo, sink);

    case GeneratorInterface::Interface::wmts:
        return wmtsInterface(fileInfo, sink);

    default:
        sink.error(utility::makeError<InternalError>
                   ("TMS resource has no <%s> interface."
                    , fileInfo.interface));
        return {};
    }
}

const vre::Wmts& TmsRasterBase::getWmts() const
{
    if (!wmts_) {
        utility::raise<NotFound>
            ("WMTS interface disabled, no <wmts> extension in "
             "reference frame <%s> or not supported by <%s> driver."
             , referenceFrameId(), resource().generator);
    }

    return *wmts_;
}

wmts::WmtsResources TmsRasterBase::wmtsResources(const WmtsFileInfo &fileInfo)
    const
{
    const auto &fi(fileInfo.fileInfo);
    bool introspection
        (!uq::empty(uq::find(uq::splitQuery(fi.query), "is")));

    wmts::WmtsResources resources;

    resources.layers.emplace_back(resource());
    auto &layer(resources.layers.back());

    layer.format = format();

    // build root path
    if (introspection) {
        // used in introspection -> local, can be relative from wmts interface
        layer.rootPath = "./";

        resources.capabilitiesUrl = "./" + fileInfo.capabilitesName;
    } else {
        layer.rootPath = config().externalUrl
            + prependRoot(std::string(), id()
                          , {type(), GeneratorInterface::Interface::wmts}
                          , ResourceRoot::Depth::referenceFrame);
        resources.capabilitiesUrl =
            layer.rootPath + "/" + fileInfo.capabilitesName;
    }

    return resources;
}

std::string TmsRasterBase::wmtsReadme() const
{
    vs::SupportFile::Vars vars;
    vars["externalUrl"] = config().externalUrl;
    vars["url"] = url(GeneratorInterface::Interface::wmts);

    return files::wmtsReadme.expand(&vars, nullptr);
}

Generator::Task TmsRasterBase
::wmtsInterface(const FileInfo &fileInfo, Sink &sink) const
{
    WmtsFileInfo fi(fileInfo);

    switch (fi.type) {
    case WmtsFileInfo::Type::unknown:
        sink.error(utility::makeError<NotFound>("Unrecognized filename."));
        break;

    case WmtsFileInfo::Type::image:
        return [=](Sink &sink, Arsenal &arsenal) {
            ImageFlags imageFlags;
            imageFlags.dontOptimize = true;
            generateTileImage(fi.tileId, fi.sinkFileInfo(), fi.format
                              , sink, arsenal, imageFlags);
        };

    case WmtsFileInfo::Type::capabilities:
        sink.content(wmtsCapabilities(wmtsResources(fi)), fi.sinkFileInfo());
        return {};

    case WmtsFileInfo::Type::support:
        supportFile(*fi.support, sink, fi.sinkFileInfo());
        break;

    case WmtsFileInfo::Type::listing:
        sink.listing(fi.listing, "", markdown(wmtsReadme()));
        break;

    case WmtsFileInfo::Type::readme:
        sink.markdown(utility::format("%s: WMTS Readme", id().fullId())
                      , wmtsReadme());
        break;

    default:
        sink.error(utility::makeError<InternalError>
                    ("Not implemented yet."));
    }

    return {};
}

vts::MapConfig TmsRasterBase::mapConfig_impl(ResourceRoot root)
    const
{
    const auto &res(resource());

    vts::MapConfig mapConfig;
    mapConfig.referenceFrame = *res.referenceFrame;

    // this is Tiled service: we have bound layer only; use remote definition
    mapConfig.boundLayers.add
        (vr::BoundLayer
         (res.id.fullId()
          , prependRoot(std::string("boundlayer.json"), resource(), root)));

    return mapConfig;
}

Generator::Task TmsRasterBase::generateVtsFile_impl(const FileInfo &fileInfo
                                                , Sink &sink) const
{
    TmsFileInfo fi(fileInfo);

    // check for valid tileId
    switch (fi.type) {
    case TmsFileInfo::Type::image:
    case TmsFileInfo::Type::mask:
        if (!checkRanges(resource(), fi.tileId)) {
            sink.error(utility::makeError<NotFound>
                        ("TileId outside of configured range."));
            return {};
        }
        break;

    case TmsFileInfo::Type::metatile:
        if (hasMetatiles()) {
            sink.error(utility::makeError<NotFound>
                        ("This dataset doesn't provide metatiles."));
            return {};
        }
        if (!checkRanges(resource(), fi.tileId, RangeType::lod)) {
            sink.error(utility::makeError<NotFound>
                        ("TileId outside of configured range."));
            return {};
        }
        break;

    default: break;
    }

    // beef
    switch (fi.type) {
    case TmsFileInfo::Type::unknown:
        sink.error(utility::makeError<NotFound>("Unrecognized filename."));
        break;

    case TmsFileInfo::Type::config: {
        std::ostringstream os;
        mapConfig(os, ResourceRoot::none);
        sink.content(os.str(), fi.sinkFileInfo());
        break;
    };

    case TmsFileInfo::Type::definition: {
        std::ostringstream os;
        vr::saveBoundLayer(os, boundLayer(ResourceRoot::none));
        sink.content(os.str(), fi.sinkFileInfo());
        break;
    }

    case TmsFileInfo::Type::support:
        sink.content(fi.support->data, fi.support->size
                      , fi.sinkFileInfo(), false);
        break;

    case TmsFileInfo::Type::image: {
        return[=](Sink &sink, Arsenal &arsenal) {
            generateTileImage(fi.tileId, fi.sinkFileInfo(), fi.format
                              , sink, arsenal, ImageFlags());
        };
    }

    case TmsFileInfo::Type::mask:
        return [=](Sink &sink, Arsenal & arsenal) {
            generateTileMask(fi.tileId, fi, sink, arsenal);
        };

    case TmsFileInfo::Type::metatile:
        return [=](Sink &sink, Arsenal &arsenal) {
            generateMetatile(fi.tileId, fi, sink, arsenal);
        };
    }

    return {};
}

vr::BoundLayer TmsRasterBase::boundLayer(ResourceRoot root) const
{
    const auto &res(resource());

    vr::BoundLayer bl;
    bl.id = res.id.fullId();
    bl.numericId = 0; // no numeric ID
    bl.type = vr::BoundLayer::Type::raster;

    // build url
    bl.url = prependRoot
        (utility::format("{lod}-{x}-{y}.%s?gr=%d%s"
                         , format(), RevisionWrapper(res.revision, "&"))
         , resource(), root);
    if (hasMask()) {
        bl.maskUrl = prependRoot
            (utility::format("{lod}-{x}-{y}.mask?gr=%d%s"
                             , RevisionWrapper(res.revision, "&"))
             , resource(), root);
        if (hasMetatiles()) {
            const auto fname
                (utility::format("{lod}-{x}-{y}.meta?gr=%d%s"
                                 , generatorRevision()
                                 , RevisionWrapper(res.revision, "&")));

            bl.metaUrl = prependRoot(fname, resource(), root);
        }
    }

    bl.lodRange = res.lodRange;
    bl.tileRange = res.tileRange;
    bl.credits = asInlineCredits(res);
    bl.isTransparent = transparent();

    bl.options = boundLayerOptions();

    // done
    return bl;
}

namespace Constants {
    const unsigned int RasterMetatileBinaryOrder(8);
    const math::Size2 RasterMetatileSize(1 << RasterMetatileBinaryOrder
                                         , 1 << RasterMetatileBinaryOrder);
}

namespace MetaFlags {
    constexpr std::uint8_t watertight(0xc0);
    constexpr std::uint8_t available(0x80);
}


namespace {

void meta2d(const mmapped::TileIndex &tileIndex, const vts::TileId &tileId
    , const TmsFileInfo &fi, Sink &sink)
{
    bgil::gray8_image_t out(Constants::RasterMetatileSize.width
                            , Constants::RasterMetatileSize.height
                            , bgil::gray8_pixel_t(0x00), 0);
    auto outView(view(out));

    if (const auto *tree = tileIndex.tree(tileId.lod)) {
        const auto parentId
            (vts::parent(tileId, Constants::RasterMetatileBinaryOrder));

        rasterize(*tree, parentId.lod, parentId.x, parentId.y
                  , outView, [&](vts::QTree::value_type flags) -> std::uint8_t
        {
            std::uint8_t out(0);

            if (flags & vts::TileIndex::Flag::mesh) {
                out |= MetaFlags::available;

                if (flags & vts::TileIndex::Flag::watertight) {
                    out |= MetaFlags::watertight;
                }
            }

            return out;
        });
    }

    sink.content(imgproc::png::serialize(out, 9), fi.sinkFileInfo());
}

} // namespace

void TmsRasterBase::generateMetatile(const vts::TileId &tileId
    , const TmsFileInfo &fi , Sink &sink, Arsenal &) const {

    sink.checkAborted();

    auto blocks(metatileBlocks
        (resource(), tileId, Constants::RasterMetatileBinaryOrder));

    if (blocks.empty()) {
        sink.error(utility::makeError<NotFound>
                    ("Metatile completely outside of configured range."));
        return;
    }

    if (!tileIndex()) {
        LOGTHROW(err4, std::runtime_error)
            << "Subclass needs to return valid tile index for default " <<
            "implementation of generate metatile - fix your subclass.";
    }

    // render tileindex
    meta2d(*tileIndex(), tileId, fi, sink);
    return;
}



} // namespace generator
