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


//#include "mapproxy/resource.hpp"
#include "utility/premain.hpp"
#include "utility/raise.hpp"
#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "tms.hpp"
#include "factory.hpp"

namespace ut = utility;

namespace resource {

constexpr char TmsSpecularMap::driverName[];

MAPPROXY_DEFINITION_REGISTER(TmsSpecularMap)

namespace {

void parseDefinition(TmsSpecularMap &def, const Json::Value &value) {

    Json::get(def.classdef, value, "classdef");

    typedef geo::GeoDataset::Resampling Resampling;

    if (value.isMember("shininessBits")) {

        def.shininessBits = Json::as<uchar>(value["shininessBits"]);
    }

    Json::getOpt(def.shininessBits, value, "shininessBits");

    // sanity
    if (def.format != RasterFormat::webp
        && def.format != RasterFormat::png) {

        ut::raise<Json::Error>(
            "Format %1% not supported in tms-specularmap, use png or webp",
            def.format);

    }

    if (def.resampling != Resampling::mode
        && def.resampling != Resampling::nearest) {

        ut::raise<Json::Error>(
            "Resampling %1% not supported in tms-specularmap, use mode or nearest",
            def.format);
    }
}

void buildDefinition(Json::Value &value, const TmsSpecularMap &def) {

    value["classdef"] = def.classdef;
    value["shininessBits"] = def.shininessBits;
}

} // namespace

void TmsSpecularMap::from_impl(const Json::Value &value) {

    TmsRaster::from_impl(value);
    parseDefinition(*this, value);
}

void TmsSpecularMap::to_impl(Json::Value &value) const {

    TmsRaster::to_impl(value);
    buildDefinition(value, *this);
}

Changed TmsSpecularMap::changed_impl(const DefinitionBase &o) const {

    const auto &other(o.as<TmsSpecularMap>());

    // non-safe changes - none

    // revision bump - none
    if (classdef != other.classdef) { return Changed::yes; }
    if (shininessBits != other.shininessBits) { return Changed::yes; }

    // safe changes - none

    // common definition
    return TmsRaster::changed_impl(o);
}


} // namespace resource
