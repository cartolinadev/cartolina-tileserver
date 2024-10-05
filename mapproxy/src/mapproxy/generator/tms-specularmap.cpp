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

#include "tms-specularmap.hpp"

#include "factory.hpp"
#include "../support/atlas.hpp"

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
        return std::make_shared<TmsSpecularMap>(params);
    }

private:
    static utility::PreMain register_;
};

utility::PreMain Factory::register_([]()
{
    Generator::registerType<TmsSpecularMap>(std::make_shared<Factory>());
});

} // namespace



TmsSpecularMap::TmsSpecularMap(const Params &params)
    : TmsRaster(params, boost::none, true) {

    auto definition = params.resource.definition<resource::TmsSpecularMap>();

    params_.classdef = absoluteDataset(definition.classdef);
    params_.shininessBits = definition.shininessBits;

    /*bool success(true);

    try {

        loadLandcoverClassdef();

    } catch (const std::exception& e) {
        // not ready
        success = false;
    }

    if (success) { makeReady(); return; }*/

    // default
    //LOG(info1) << "Generator for <" << id() << "> not ready.";
}

void TmsSpecularMap::extraPrep() {

    loadLandcoverClassdef();
}

void TmsSpecularMap::generateTileImage(const vts::TileId &tileId
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

    LOG(debug) << ds.path;

    // choose resampling (configured or default)
    const auto resampling(definition_.resampling ? *definition_.resampling
                          : geo::GeoDataset::Resampling::cubic);

    // warp
    auto tile(arsenal.warper.warp
              (GdalWarper::RasterRequest
               (GdalWarper::RasterRequest::Operation::imageNoExpand
                , absoluteDataset(ds.path)
                , nodeInfo.srsDef()
                , nodeInfo.extents()
                , math::Size2(256, 256)
                , resampling
                , absoluteDataset(maskDataset_))
               , sink));
    sink.checkAborted();

    // obtain specular map
    auto img = geo::landcover::specularMap(*tile, lcClassdef_,
                                           params_.shininessBits);

    // send output
    serialize(img, ds);
}


int TmsSpecularMap::generatorRevision() const {
    return generatorRevision_;
}

void TmsSpecularMap::loadLandcoverClassdef() {

    Json::Value jclasses;

    try {
            std::ifstream file(params_.classdef);
            file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

            file >> jclasses;
            file.close();

    } catch (const std::exception& e) {
            utility::raise<IOError>("Error reading \"%1% (%2%)\".",
                params_.classdef, e.what());
    }

    lcClassdef_ = geo::landcover::fromJson(jclasses);
}

} // namespace generator
