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

#include "json/value.h"
#include <boost/lexical_cast.hpp>
#include <boost/utility/in_place_factory.hpp>

#include "mapproxy/resource.hpp"
#include "utility/premain.hpp"
#include "utility/raise.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/as.hpp"

#include "tms.hpp"
#include "factory.hpp"
#include <iostream>

namespace resource {

constexpr char TmsGdaldem::driverName[];


namespace { utility::PreMain mapproxy_regdef_TmsGdaldem ([]() {

    std::cout << "Here.\n";

    registerDefinition<TmsGdaldem>(); });

}

//MAPPROXY_DEFINITION_REGISTER(TmsGdaldem)


namespace {

void parseDefinition(TmsGdaldem &def, const Json::Value &value)
{
    Json::get(def.dataset, value, "dataset");
    Json::get(def.processing, value, "processing");

    if (value.isMember("processingOptions")) {
        const auto poptions(value["processingOptions"]);

        for (const auto &option : poptions)
            def.processingOptions.emplace_back(option.asString());
    }

    if (value.isMember("poProgressions")) {

        const auto poprogressions(value["poProgressions"]);

        for (auto it = poprogressions.begin(); it != poprogressions.end();
            ++it ) {


            def.poProgressions.emplace_back(
                it.key().asString(), (*it)[0].asUInt(), (*it)[1].asFloat());
            }
    }

    if (value.isMember("colorFile")) {
        def.colorFile = boost::in_place(value["colorFile"].asString());
        LOG(warn3) << "Color file handling not (yet) implemented.";
    }

    if (value.isMember("format")) {

        Json::get(def.format, value, "format");

    }

    if (value.isMember("resampling")) {
        Json::get(def.resampling, value, "resampling");
    }

    if (value.isMember("erodeMask")) {
        Json::get(def.erodeMask, value,"erodeMask");
    }

    // tmsCommon
    def.TmsCommon::parse(value);
}

void buildDefinition(Json::Value &value, const TmsGdaldem &def)
{

    value["dataset"] = def.dataset;
    value["processing"] = boost::lexical_cast<std::string>(def.processing);

    if (!def.processingOptions.empty()) {

        auto & poptions(value["processingOptions"] = Json::arrayValue);

        for (const auto & option : def.processingOptions)
            poptions.append(option);
    }


    if (!def.poProgressions.empty()) {

        auto & poProgressions(value["poProgresssions"] = Json::objectValue);

        for (const auto & progression : def.poProgressions) {

            auto & op(poProgressions[progression.option] = Json::arrayValue);
            op.append(progression.baseLod); op.append(progression.factor);
        }
    }

    if (def.colorFile) {
        value["colorFile"] = def.colorFile.get();
    }

    value["format"] = boost::lexical_cast<std::string>(def.format);
    value["resampling"] = boost::lexical_cast<std::string>(def.resampling);
    value["erodeMask"] = def.erodeMask;

    def.TmsCommon::build(value);
}

} // namespace

void TmsGdaldem::from_impl(const Json::Value &value) {

    parseDefinition(*this, value);
}

void TmsGdaldem::to_impl(Json::Value &value) const {
    buildDefinition(value, *this);
}

Changed TmsGdaldem::changed_impl(const DefinitionBase &o) const {

    const auto &other(o.as<TmsGdaldem>());

    // non-safe changes
    if (dataset != other.dataset) { return Changed::yes; }

    // revision bump
    if (processing != other.processing) { return Changed::withRevisionBump; }
    if (processingOptions != other.processingOptions) {
        return Changed::withRevisionBump; }
    if (colorFile != other.colorFile) { return Changed::withRevisionBump; }
    if (resampling != other.resampling) { return Changed::withRevisionBump; }
    if (erodeMask != other.erodeMask) { return Changed::withRevisionBump; }
    if (poProgressions != other.poProgressions) {
        return Changed::withRevisionBump; }

    // safe changes
    if (format != other.format) { return Changed::safely; }

    return TmsCommon::changed_impl(o);
}


bool TmsGdaldem::transparent() const {

    auto & poptions(processingOptions);

    // the only known case when geo::demProcessing output is transparent
    if (processing == geo::GeoDataset::DemProcessing::color_relief
        && std::find(poptions.begin(), poptions.end(), "-alpha") != poptions.begin()) {
        return true;
    }

    return false;

}

} // namespace resource
