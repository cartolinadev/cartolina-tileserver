/**
 * Copyright (c) 2026 Montevallo Consulting, s.r.o.
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

#ifndef mapproxy_tiling_unified_hpp_included_
#define mapproxy_tiling_unified_hpp_included_

#include <algorithm>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "vts-libs/registry.hpp"
#include "vts-libs/vts/tileindex.hpp"
#include "vts-libs/vts/basetypes.hpp"

#include "mapproxy/support/mnstore.hpp"
#include "mapproxy/heightfunction.hpp"

/** RFC 7 unified tiling pass (rfc-metanode-store.md, section 4).
 *
 * Replaces the per-tile per-LOD warp of the legacy tiling with four
 * GDAL filter passes per reference-frame division node (mask min/max,
 * elevation min/max), each warping into a grid of one destination
 * pixel per tile at the analysis maximum LOD while reading the source
 * at its native (base) resolution — overview selection is disabled so
 * averaged overviews cannot bias the min/max reduction. Coarser LODs
 * are built by an in-tool 2x2 min/max mip loop. Emits the flag tile
 * index and the metanode store pages in one run.
 */
namespace tiling {

struct UnifiedConfig {
    /** Effective metatile packaging (store page shape).
     */
    unsigned int metaBinaryOrder = 5;
    unsigned int metaDepth = 1;

    /** SDS vertical datum geoid grid (resource geoidGrid setting).
     *  Stored heights are converted from the geoid-shifted SDS to the
     *  raw SDS vertical, matching the warp serve path.
     */
    boost::optional<std::string> geoidGrid;

    /** Height function baked into stored heights (must be monotone).
     */
    HeightFunction::pointer heightFunction;

    /** Samples per tile of the legacy analysis warp; used to derive
     *  the navtile eligibility (truescale) rule.
     */
    int tileSampling = 128;

    /** Treats all partial tiles as watertight (legacy option).
     */
    bool forceWatertight = false;

    /** Maximum concurrent filter-pass warps across all division
     *  nodes. The work is source-read/decompress bound, so the only
     *  hard ceiling is the storage's sustainable stream count; each
     *  in-flight pass holds one destination grid, and a node
     *  completed but not yet reduced holds four.
     */
    static int defaultWarpConcurrency() {
        const auto cores(std::thread::hardware_concurrency());
        return std::min(12, cores ? int(cores) : 4);
    }

    int warpConcurrency = defaultWarpConcurrency();
};

struct UnifiedResult {
    vtslibs::vts::TileIndex tileIndex;
    mnstore::Page::list pages;
};

/** Runs the unified tiling pass.
 *
 * @param dataset path to the DEM dataset (vrtwo dataset link)
 * @param referenceFrame reference frame to tile in
 * @param lodRange analyzed lod range
 * @param tileRanges analyzed tile ranges (mapproxy-calipers output)
 * @param config pass configuration
 * @return flag tile index and metanode store pages
 */
UnifiedResult
generateUnified(const boost::filesystem::path &dataset
                , const vtslibs::registry::ReferenceFrame &referenceFrame
                , const vtslibs::vts::LodRange &lodRange
                , const vtslibs::vts::LodTileRange::list &tileRanges
                , const UnifiedConfig &config);

/** Atomically publishes the paired flag tile index and metanode store:
 *  writes both to temporary names, computes the pairing digest and the
 *  source hash, fsyncs and renames into place so a serving daemon sees
 *  either the old pair or the new pair.
 *
 * @param result unified pass output
 * @param config pass configuration (header metadata source)
 * @param referenceFrameId reference frame id
 * @param lodRange analyzed lod range (source-hash input)
 * @param tileRanges analyzed tile ranges (source-hash input)
 * @param tileIndexPath final flag tile index path
 * @param storePath final metanode store path
 */
void publishUnified(const UnifiedResult &result
                    , const UnifiedConfig &config
                    , const std::string &referenceFrameId
                    , const vtslibs::vts::LodRange &lodRange
                    , const vtslibs::vts::LodTileRange::list &tileRanges
                    , const boost::filesystem::path &tileIndexPath
                    , const boost::filesystem::path &storePath);

} // namespace tiling

#endif // mapproxy_tiling_unified_hpp_included_
