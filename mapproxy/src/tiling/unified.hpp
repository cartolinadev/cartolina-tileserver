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

    /** Suppresses the flag tile index entry of every non-watertight
     *  (partial) tile: the served metanode carries no mesh (and no
     *  navtile) there, so a partial tile produces no cracks and forces
     *  no framebuffer switch; a global surface fills the hole. The
     *  metanode store still records the tile's true coverage, so the
     *  suppression is a reversible index-side policy. Mutually
     *  exclusive with forceWatertight; DEM (store) path only.
     */
    bool skipPartial = false;

    /** Spatially-varying bottom LOD: when set, a tile is dropped once
     *  its subdivision passes the source resolution at its own location
     *  — the target floor GSD (meters) the finest useful tile resolves.
     *  Unset means no prune (tile down to the full analyzed lod range
     *  everywhere). DEM (store) path only.
     */
    boost::optional<double> pruneGsd;

    /** Extra local LODs added to the per-tile prune floor depth (a
     *  resolution margin, and the room an operator --bottomLod override
     *  needs below the measured floor). 0 = prune exactly at
     *  source-native resolution.
     */
    int pruneExtraLods = 0;

    /** Coverage (mask-only) mode for non-DEM datasets (imagery): runs
     *  only the two mask filter passes to derive existence and
     *  watertightness, skips the elevation passes and the metanode
     *  store entirely. The pass emits the flag tile index alone, which
     *  is published with publishUnifiedIndex.
     */
    bool coverage = false;

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

/** Atomically publishes the coverage-mode flag tile index alone (no
 *  metanode store): writes to a temporary name, fsyncs and renames into
 *  place. Used for imagery, where heights are not stored.
 *
 * @param result unified pass output (only the tile index is used)
 * @param tileIndexPath final flag tile index path
 */
void publishUnifiedIndex(const UnifiedResult &result
                         , const boost::filesystem::path &tileIndexPath);

/** Re-flagging request over an existing paired flag index + metanode
 *  store. The store is the read-only witness of every tile's true
 *  coverage; the flip rewrites the flag index (and, for prune, drops
 *  store nodes) and re-pairs the pair — no warps.
 */
struct ReflagConfig {
    /** Set = target skipPartial state: true suppresses the mesh of
     *  partial tiles, false restores it (navtile recomputed).
     */
    boost::optional<bool> skipPartial;

    /** Set = retro-prune to this target floor GSD (meters per pixel).
     *  Only removes tiles finer than the source resolves; it cannot add
     *  resolution back (re-tile for that).
     */
    boost::optional<double> pruneGsd;

    int pruneExtraLods = 0;

    /** Samples per tile edge for the navtile-eligibility measure used
     *  when restoring a suppressed partial tile.
     */
    int tileSampling = 128;
};

struct ReflagStats {
    std::size_t total = 0;      // real store nodes examined
    std::size_t suppressed = 0; // partial index entries cleared
    std::size_t restored = 0;   // partial index entries set
    std::size_t pruned = 0;     // nodes dropped from store and index
};

/** Re-flags an existing paired flag index + metanode store in place.
 *
 * @param dataset DEM dataset (needed only to restore navtile bits)
 * @param referenceFrame reference frame (store header names it)
 * @param tileIndexPath paired flag tile index path
 * @param storePath paired metanode store path
 * @param config the flip to apply
 * @param apply write the re-paired artifacts (false = report only)
 * @return counts of the planned/applied changes
 */
ReflagStats reflag(const boost::filesystem::path &dataset
                   , const vtslibs::registry::ReferenceFrame &referenceFrame
                   , const boost::filesystem::path &tileIndexPath
                   , const boost::filesystem::path &storePath
                   , const ReflagConfig &config
                   , bool apply);

} // namespace tiling

#endif // mapproxy_tiling_unified_hpp_included_
