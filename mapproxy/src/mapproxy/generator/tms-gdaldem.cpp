/**
 * Copyright (c) 2023 Ondrej Prochazka
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

#include "tms-gdaldem.hpp"

#include "factory.hpp"

#include "../support/tileindex.hpp"
#include "../support/mmapped/tileindex.hpp"

#include "imgproc/morphology.hpp"
#include "utility/premain.hpp"
#include "utility/path.hpp"
#include "utility/raise.hpp"

#include <opencv2/highgui/highgui.hpp>


namespace fs = boost::filesystem;

typedef geo::GeoDataset::DemProcessing Processing;


namespace generator {

namespace {

// upgrade whenever functionality is altered to warrant invalidation
// of all cached generator output in production environment
int generatorRevision_(0);

// register generator via pre-main static initialization

struct Factory : Generator::Factory {
    virtual Generator::pointer create(const Generator::Params &params)
    {
        return std::make_shared<TmsGdaldem>(params);
    }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType<TmsGdaldem>(std::make_shared<Factory>());
});

std::string datasetPath_(const std::string & datasetPath) {
    return datasetPath + "/dem";
}

} // namespace


// Generator::Detail::TmsGdaldemMFB

detail::TmsGdaldemMFB::TmsGdaldemMFB(const Generator::Params & params)
    : definition_(params.resource.definition<Definition>()) {
}

// Generator::TmsGdaldem

TmsGdaldem::TmsGdaldem(const Params &params
    , const boost::optional<RasterFormat> &format)
    : detail::TmsGdaldemMFB(params)
    , TmsRasterBase(params, format ? *format : definition_.format) {

    const auto deliveryIndexPath(root() / "delivery.index");

    // compulsory check for every driver
    if (changeEnforced()) {
        LOG(info1) << "Generator for <" << id() << "> not ready.";
        return;
    }

    // delivery index is all we need
    if (fs::exists(deliveryIndexPath)) {
        index_ = std::make_unique<mmapped::TileIndex>(deliveryIndexPath);
        makeReady();
        return;
    }

    // default
    LOG(info1) << "Generator for <" << id() << "> not ready.";
    return;
}

void TmsGdaldem::prepare_impl(Arsenal &) {

    LOG(info2) << "Preparing <" << id() << ">.";

    // probe
    geo::GeoDataset::open(absoluteDataset(
        datasetPath_(definition_.dataset)));

    // build delivery index
    const auto &r(resource());

    vts::TileIndex index;
    prepareTileIndex(index, (absoluteDataset(definition_.dataset)
        + "/tiling." + r.id.referenceFrame), r, false, {});

    // store and open
    const auto deliveryIndexPath(root() / "delivery.index");
    const auto tmpPath(utility::addExtension(deliveryIndexPath, ".tmp"));
    mmapped::TileIndex::write(tmpPath, index);
    fs::rename(tmpPath, deliveryIndexPath);
    index_ = std::make_unique<mmapped::TileIndex>(deliveryIndexPath);

    // done
    return;
}

void TmsGdaldem::generateTileMask(const vts::TileId &tileId
    , const TmsFileInfo &fi , Sink &sink, Arsenal &arsenal) const {


    // checks
    sink.checkAborted();

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.valid()) {
        sink.error(utility::makeError<NotFound>
                    ("TileId outside of valid reference frame tree."));
        return;
    }

    if (!nodeInfo.productive()
        || (index_ && !vts::TileIndex::Flag::isReal(index_->get(tileId))))
    {
        sink.error(utility::makeError<EmptyImage>("No valid data."));
        return;
    }

    // get dataset
    auto mask(arsenal.warper.warp
              (GdalWarper::RasterRequest
               (GdalWarper::RasterRequest::Operation::mask
                , absoluteDataset(datasetPath_(definition_.dataset))
                , nodeInfo.srsDef()
                , nodeInfo.extents()
                , math::Size2(256, 256)
                , geo::GeoDataset::Resampling::cubic)
               , sink));

    sink.checkAborted();

    // optional mask erosion
    if (definition_.erodeMask) {
        // TODO: mask should be warped with 1px margin
        // for correct handling of edge pixels
        imgproc::erode<uchar>(*mask);
    }

    // serialize
    std::vector<unsigned char> buf;
    // write as png file
    cv::imencode(".png", *mask, buf
                 , { cv::IMWRITE_PNG_COMPRESSION, 9 });

    // done
    sink.content(buf, fi.sinkFileInfo());
}

bool TmsGdaldem::transparent() const {

    auto & poptions(definition_.processingOptions);

    // the only known case when geo::demProcessing output is transparent
    if (definition_.processing == Processing::color_relief
        && std::find(poptions.begin(), poptions.end(), "-alpha") != poptions.begin()) {
        return true;
    }

    return false;
}

RasterFormat TmsGdaldem::format() const {
    return transparent() ? RasterFormat::png : definition_.format;
}


boost::any TmsGdaldem::boundLayerOptions() const {
    return definition_.options;
}


int TmsGdaldem::generatorRevision() const {
    return generatorRevision_;
}

} // namespace generator
