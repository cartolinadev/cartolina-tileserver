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


#include "mapproxy/resource.hpp"
#include "utility/premain.hpp"
#include "utility/raise.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "tms.hpp"
#include "factory.hpp"

namespace ut = utility;

namespace resource {

constexpr char TmsNormalMap::driverName[];

MAPPROXY_DEFINITION_REGISTER(TmsNormalMap)

namespace {

void parseDefinition(TmsNormalMap &def, const Json::Value &value) {

    Json::getOpt(def.zFactor, value, "zFactor");
    Json::getOpt(def.invertRelief, value, "invertRelief");

    if (value.isMember("landcover")) {

        auto lc = value["landcover"];

        std::string ds, classdef;
        Json::get(ds, lc, "dataset");
        Json::get(classdef, lc, "classdef");

        def.landcover = LandcoverDataset(ds, classdef);
    }

    // sanity check
    if (def.format != RasterFormat::webp) {

        ut::raise<Json::Error>(
            "Format %1% not supported in tms-normalmap, use webp", def.format);
    }

}

void buildDefinition(Json::Value &value, const TmsNormalMap &def) {

    value["zFactor"] = def.zFactor;
    value["invertRelief"] = def.invertRelief;

    if (def.landcover) {
        auto& lc(value["landcover"] = Json::objectValue);

        lc["dataset"] = def.landcover->dataset;
        lc["classdef"] = def.landcover->classdef;
    }
}

} // namespace

void TmsNormalMap::from_impl(const Json::Value &value) {

    TmsRaster::from_impl(value);
    parseDefinition(*this, value);
}

void TmsNormalMap::to_impl(Json::Value &value) const {

    TmsRaster::to_impl(value);
    buildDefinition(value, *this);
}

Changed TmsNormalMap::changed_impl(const DefinitionBase &o) const {

    const auto &other(o.as<TmsNormalMap>());

    // non-safe changes - none
    // revision bump
    if (landcover != other.landcover
        || ! math::almostEqual(zFactor, other.zFactor)) {
        return Changed::withRevisionBump; }

    // common definition
    return TmsRaster::changed_impl(o);
}


} // namespace resource
