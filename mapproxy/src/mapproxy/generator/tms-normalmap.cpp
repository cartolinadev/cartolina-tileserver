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

#include "imgproc/morphology.hpp"
#include "utility/premain.hpp"
#include "utility/raise.hpp"

namespace fs = boost::filesystem;

namespace generator {

namespace {

// upgrade whenever functionality is altered to warrant invalidation
// of all cached generator output in production environment
int generatorRevision_(0);

// register generator via pre-main static initialization

/*

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
}); */

} // namespace



// Generator::Detail::TmsGdaldemMFB

detail::TmsNormalMapMFB::TmsNormalMapMFB(const Generator::Params & params)
    : definition_(params.resource.definition<Definition>()) {
}


TmsNormalMap::TmsNormalMap(const Params &params)
    : detail::TmsNormalMapMFB(params), TmsRasterBase(params) {

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
/*
void TmsNormalMap::prepare_impl(Arsenal &) {

    LOG(info2) << "Preparing <" << id() << ">.";



}


void generateTileImage(const vts::TileId &tileId
    , const Sink::FileInfo &fi
    , RasterFormat format
    , Sink &sink, Arsenal &arsenal
    , const ImageFlags &imageFlags = ImageFlags()) const {

    // TODO
}



void TmsNormalMap::generateTileMask(const vts::TileId &tileId
                          , const TmsFileInfo &fi
                          , Sink &sink, Arsenal &arsenal) const {

    // TODO
}
*/

RasterFormat TmsNormalMap::format() const {
    return RasterFormat::webp;
}

int TmsNormalMap::generatorRevision() const {
    return generatorRevision_;
}

boost::any TmsNormalMap::boundLayerOptions() const {

    return definition_.options;
}


} // namespace generator
