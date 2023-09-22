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

#include "mapproxy/support/mmapped/tileindex.hpp"

#include "utility/premain.hpp"
#include "utility/path.hpp"


namespace fs = boost::filesystem;

namespace generator {

namespace {

// upgrade whenever functionality is altered to warant invalidation
// of the cached generator output
// int generatorRevision(0);

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


} // namespace


// Generator::TmsGdaldem implementation follows


TmsGdaldem::TmsGdaldem(const Params &params
    , const boost::optional<RasterFormat> &format)
    : detail::TmsGdaldemMFB(params)
    , TmsRasterBase(params, format ? *format : definition_.format) {

    const auto deliveryIndexPath(root() / "delivery.index");

    if (changeEnforced()) {
        LOG(info1) << "Generator for <" << id() << "> not ready.";
        return;
    }

    if (fs::exists(deliveryIndexPath)) {
        index_ = std::make_unique<mmapped::TileIndex>(deliveryIndexPath);
        makeReady();
        return;
    }

    LOG(info1) << "Generator for <" << id() << "> not ready.";
    return;
}


void TmsGdaldem::prepare_impl(Arsenal &) {

    LOG(info2) << "Preparing <" << id() << ">.";

    // try to open
    geo::GeoDataset::open(absoluteDataset(definition_.dataset + "/dem"));

    // build delivery inddex
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


} // namespace generator
