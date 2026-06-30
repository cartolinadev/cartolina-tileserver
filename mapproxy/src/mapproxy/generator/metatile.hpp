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

#ifndef mapproxy_metatile_hpp_included_
#define mapproxy_metatile_hpp_included_

#include <memory>

#include <boost/filesystem.hpp>

#include "vts-libs/vts/tileindex.hpp"
#include "vts-libs/vts/metatile.hpp"

#include "../support/coverage.hpp"
#include "../support/mmapped/tileindex.hpp"
#include "../support/mnstore.hpp"

#include "../generator.hpp"

#include "../heightfunction.hpp"

struct MetatileOverrides {
    enum class CreditsMode { replace, add };

    vts::SubMesh::TextureMode textureMode = vts::SubMesh::external;
    DualId::set credits;
    CreditsMode creditsMode = CreditsMode::add;

    MetatileOverrides() = default;
    MetatileOverrides(vts::SubMesh::TextureMode textureMode)
        : textureMode(textureMode)
    {}

    void addCredits(const DualId::set &addition);

    DualId::set mergedCredits(const DualId::set &original) const;

    vr::IdSet mergedCredits(const vr::IdSet &original) const;
};

/** Metanode-store validation parameters shared by DEM-surface and
 *  tiled-geodata metatile generators.
 */
struct MetanodeStoreConfig {
    std::string id;
    boost::filesystem::path datasetDir;
    boost::filesystem::path root;
    std::string referenceFrame;
    unsigned int metaBinaryOrder = 5;
    unsigned int metaDepth = 1;
    boost::optional<std::string> geoidGrid;
    std::string heightFunction;
    bool hasMask = false;
    vts::LodRange lodRange = vts::LodRange::emptyRange();
};

/** Opens and validates a metanode store. Returns null when the store is
 *  absent or rejected; every fallback-worthy condition is logged at W3.
 *
 *  A store that is structurally valid but paired with a newer flag tile
 *  index than the cached delivery index is *not* a warp-fallback case: a
 *  re-prepare rebuilds the delivery index and adopts the store. When this
 *  is detected and @p needsReprepare is non-null, it is set to true (and
 *  null is returned without the scary reject) so the generator can force a
 *  re-prepare instead of silently degrading to the warp path.
 *
 * @param config validation context
 * @param needsReprepare optional out-flag set when a re-prepare would adopt
 *        the store; left untouched otherwise
 * @return opened store, or null when the warp path must be used
 */
std::unique_ptr<mnstore::Store>
openMetanodeStore(const MetanodeStoreConfig &config
                  , bool *needsReprepare = nullptr);

/** Checks whether the legacy warp metatile path has its min/max inputs.
 *  Probes quietly: a missing pyramid (the norm for store-backed datasets)
 *  returns false without emitting GeoDataset open errors.
 *
 * @param demDataset path to the normal DEM dataset link
 * @return true when both dem.min and dem.max can be opened
 */
bool legacyDemMetatileInputsAvailable(const std::string &demDataset);

vts::MetaTile metatileFromDem(const vts::TileId &tileId, Sink &sink
                              , Arsenal &arsenal
                              , const Resource &resource
                              , const vts::TileIndex &tileIndex
                              , const std::string &demDataset
                              , const boost::optional<std::string> &geoidGrid
                              , const MaskTree &maskTree = MaskTree()
                              , const boost::optional<int> &displaySize
                              = boost::none
                              , const HeightFunction::pointer &heightFunction
                              = HeightFunction::pointer()
                              , const MetatileOverrides &overrides = {});

vts::MetaTile metatileFromDem(const vts::TileId &tileId, Sink &sink
                              , Arsenal &arsenal
                              , const Resource &resource
                              , const mmapped::TileIndex &tileIndex
                              , const std::string &demDataset
                              , const boost::optional<std::string> &geoidGrid
                              , const MaskTree &maskTree = MaskTree()
                              , const boost::optional<int> &displaySize
                              = boost::none
                              , const HeightFunction::pointer &heightFunction
                              = HeightFunction::pointer()
                              , const MetatileOverrides &overrides = {});

/** Builds a metatile from the metanode store (RFC 7) without a DEM
 *  warp. Returns boost::none when the store cannot serve this
 *  metatile (page or node payload missing for tiles the flag index
 *  claims) so the caller can fall back to the warp path.
 *
 * @param tileId metatile id
 * @param store opened metanode store
 * @param resource resource being served
 * @param tileIndex paired delivery flag index
 * @param geoidGrid SDS vertical datum geoid grid (resource setting)
 * @param displaySize optional geodata display-size override
 * @param overrides credits/texture overrides
 * @return assembled metatile or boost::none
 */
boost::optional<vts::MetaTile>
metatileFromStore(const vts::TileId &tileId
                  , const mnstore::Store &store
                  , const Resource &resource
                  , const mmapped::TileIndex &tileIndex
                  , const boost::optional<std::string> &geoidGrid
                  , const boost::optional<int> &displaySize = boost::none
                  , const MetatileOverrides &overrides = {});

// inines

inline DualId::set
MetatileOverrides::mergedCredits(const DualId::set &original) const
{
    // just send configured credits
    if (creditsMode == CreditsMode::replace) { return credits; }

    // merge original with configured credits
    auto c(original);
    c.insert(credits.begin(), credits.end());
    return c;
}

inline vr::IdSet
MetatileOverrides::mergedCredits(const vr::IdSet &original) const
{
    const auto thisCredits(asIntSet(credits));
    // just send configured credits
    if (creditsMode == CreditsMode::replace) { return thisCredits; }

    // merge original with configured credits
    auto c(original);
    c.insert(thisCredits.begin(), thisCredits.end());
    return c;
}

inline void MetatileOverrides::addCredits(const DualId::set &addition)
{
    credits.insert(addition.begin(), addition.end());
}

#endif // mapproxy_metatile_hpp_included_
