/**
 * Copyright (c) 2024 Ondrej Prochazka
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

#include "tms-normalmap.hpp"

#include "factory.hpp"
#include "../support/atlas.cpp"
#include "../support/mesh.cpp"

#include "imgproc/morphology.hpp"
#include "utility/premain.hpp"
#include "utility/raise.hpp"


namespace generator {

namespace {

// upgrade whenever functionality is altered to warrant invalidation
// of all cached generator output in production environment
int generatorRevision_(0);

// register generator via pre-main static initialization

struct Factory : Generator::Factory {
    virtual Generator::pointer create(const Generator::Params &params)
    {
        return std::make_shared<TmsNormalMap>(params);
    }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType<TmsNormalMap>(std::make_shared<Factory>());
});

} // namespace



TmsNormalMap::TmsNormalMap(const Params &params) : TmsRaster(params) {

    auto definition = params.resource.definition<resource::TmsNormalMap>();

    if (definition.landcover) {
        landcover_.emplace(
            LandcoverDataset(
                absoluteDataset(definition.landcover->dataset + "/ophoto"),
                absoluteDataset(definition.landcover->classdef)));
    }

    bool success(true);

    try {

        if (landcover_) loadLandcoverClassdef();

    } catch (const std::exception& e) {
        // not ready
        success = false;
    }

    if (success) { makeReady(); return; }

    // default
    LOG(info1) << "Generator for <" << id() << "> not ready.";
}

void TmsNormalMap::extraPrep() {

    if (landcover_) loadLandcoverClassdef();
}

void TmsNormalMap::generateTileImage(const vts::TileId &tileId
    , const Sink::FileInfo &fi
    , RasterFormat format
    , Sink &sink, Arsenal &arsenal
    , const ImageFlags &imageFlags) const {

    sink.checkAborted();

    // serialization func
    const auto &serialize([&](const cv::Mat &tile, const DatasetDesc &ds)
                          -> void
    {
        sendImage(tile, Sink::FileInfo(fi).setMaxAge(ds.maxAge)
                  , format, imageFlags.atlas, sink);
    });


    // validity checks and corner cases
    if (!imageFlags.checkFormat(format, this->format())) {
        return sink.error
            (utility::makeError<NotFound>
             ("Format <%s> is not supported by this resource (%s)."
              , format, this->format()));
    }

    vts::NodeInfo nodeInfo(referenceFrame(), tileId);
    if (!nodeInfo.valid()) {
        return sink.error
            (utility::makeError<NotFound>
             ("TileId outside of valid reference frame tree."));
    }

    if (!nodeInfo.productive()
        || (index_ && !vts::TileIndex::Flag::isReal(index_->get(tileId))))
    {
        // return full blown black image
        return serialize(cv::Mat_<cv::Vec3b>(vr::BoundLayer::tileHeight
                                             , vr::BoundLayer::tileWidth
                                             , cv::Vec3b(0, 0, 0))
                         , dataset());
    }

    // grab dataset to use
    const auto ds(dataset());

    // choose resampling (configured or default)
    const auto resampling(definition_.resampling ? *definition_.resampling
                          : geo::GeoDataset::Resampling::cubic);

    // warp
    auto tile(arsenal.warper.warp
              (GdalWarper::RasterRequest
               (GdalWarper::RasterRequest::Operation::image
                , absoluteDataset(ds.path)
                , nodeInfo.srsDef()
                , nodeInfo.extents()
                , math::Size2(256, 256)
                , resampling
                , absoluteDataset(maskDataset_))
               , sink));
    sink.checkAborted();


    // convert to grayscale if needed
    if  (tile->channels() != 1) {

        auto tmp = std::make_shared<cv::Mat>();

        if (tile->channels() == 3) {
            cv::cvtColor(*tile, *tmp, cv::COLOR_BGR2GRAY);
        }

        if (tile->channels() == 4) {
            cv::cvtColor(*tile, *tmp, cv::COLOR_BGRA2GRAY);
        }

        tile = tmp;
    }

    // obtain flat mask if landcover ds is provided, create empty inversion mask
    imgproc::RasterMask flatMask(tile->cols, tile->rows,
                            imgproc::RasterMask::EMPTY);
    imgproc::quadtree::RasterMask inversionMask(tile->cols, tile->rows,
                            imgproc::quadtree::RasterMask::EMPTY);

    if (landcover_) {

        auto lc(arsenal.warper.warp(
            GdalWarper::RasterRequest(
                GdalWarper::RasterRequest::Operation::imageNoExpand,
                landcover_->dataset,
                nodeInfo.srsDef(),
                nodeInfo.extents(),
                math::Size2(256, 256),
                geo::GeoDataset::Resampling::nearest), sink));

        //cv::imwrite("lc.png", *lc);

        sink.checkAborted();

        flatMask = geo::landcover::flatMask(*lc, lcClassdef_);
        // flatMask.invert(); // diagnostics
    }

    // obtain normal map
    math::Size2f pixelSize(
        (nodeInfo.extents().ur[0] - nodeInfo.extents().ll[0]) / 256,
        (nodeInfo.extents().ur[1] - nodeInfo.extents().ll[1]) / 256);

    geo::normalmap::Parameters params;

    params.algorithm = geo::normalmap::Algorithm::zevenbergenThorne;
    params.viewspaceRf = true;
    params.invertRelief = true; // darker is higher, as the convention goes
    params.zFactor = 0.427; // empirical value chosen to mimick gimp plugin

    auto normalMap = geo::normalmap::demNormals<uchar>(
        *tile, pixelSize, params, flatMask, inversionMask);

    // convert normals to refframe's physical srs

    const auto conv(sds2phys(nodeInfo, boost::none));
    if (!conv) { utility::raise<InternalError>("Conversion failed."); }

    bool optimize = false;

    if (tileId.lod > 3) {
        // we optimize normals for lods starting with 4, when tiles no
        // longer span greater parts of hemispheres. A more abstract
        // approach based on the reference frames specification would
        // be more rigorous.
        optimize = true;
    }

    geo::normalmap::convertNormals(
        normalMap, nodeInfo.extents(), conv.conv(), optimize);

    // obtain the final image
    cv::Mat img = geo::normalmap::exportToBGR(normalMap);

    // send output
    serialize(img, ds);
}


RasterFormat TmsNormalMap::format() const {
    return RasterFormat::webp;
}

int TmsNormalMap::generatorRevision() const {
    return generatorRevision_;
}


void TmsNormalMap::loadLandcoverClassdef() {

    Json::Value jclasses;

    try {
            std::ifstream file(landcover_->classdef);
            file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

            file >> jclasses;
            file.close();

    } catch (const std::exception& e) {
            utility::raise<IOError>("Error reading \"%1% (%2%)\".",
                landcover_->classdef, e.what());
    }

    lcClassdef_ = geo::landcover::fromJson(jclasses);
}

} // namespace generator
