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

/** RFC 7 metanode store serve path: builds a v6 metatile from
 *  precomputed node payload — no DEM warp.
 *
 * Stored fields (mesh/watertight via the paired flag index, minZ/maxZ
 * from the store) are combined with fields derived at delivery:
 * full-cell horizontal extents, midpoint surrogate, navtile height
 * range (raw-SDS to navigation SRS conversion at the cell center) and
 * the calibrated relief-corrected analytic texelSize.
 */

#include "utility/raise.hpp"

#include "vts-libs/vts/csconvertor.hpp"

#include "../support/metatile.hpp"
#include "../support/srs.hpp"
#include "../support/mesh.hpp"

#include "metatile.hpp"

namespace {

typedef vts::MetaNode::Flag MetaFlag;
typedef vts::TileIndex::Flag TiFlag;

/** Relief correction constant of the texelSize heuristic
 *  (rfc-metanode-store.md section 5): fitted in the phase-1
 *  calibration spike (0.3-0.7 across datasets and reference frames,
 *  with sub-percent impact); 0.5 errs toward deeper descent.
 */
const double texelReliefCoefficient(0.5);

inline MetaFlag::value_type ti2metaFlags(TiFlag::value_type ti)
{
    MetaFlag::value_type meta(MetaFlag::none);
    if (ti & TiFlag::mesh) { meta |= MetaFlag::geometryPresent; }
    if (ti & TiFlag::navtile) { meta |= MetaFlag::navtilePresent; }
    if (ti & TiFlag::watertight) { meta |= MetaFlag::watertight; }
    return meta;
}

/** Per-tile planar-texel sampling density: full warp-equivalent 8x8
 *  quads at coarse lods where projection curvature matters, tile
 *  corners only at deep lods where the cell is locally flat.
 */
int planarSamples(vts::Lod lod)
{
    if (lod >= 10) { return 1; }
    if (lod >= 7) { return 2; }
    return 8;
}

/** Returns a thread-cached convertor; CsConvertor construction
 *  builds a PROJ pipeline (proj.db lookups), which dominates the
 *  no-warp serve cost when done per request.
 */
const vts::CsConvertor& cachedConvertor(const std::string &from
                                        , const std::string &to)
{
    thread_local std::map<std::pair<std::string, std::string>
                          , vts::CsConvertor> cache;
    auto key(std::make_pair(from, to));
    auto icache(cache.find(key));
    if (icache == cache.end()) {
        icache = cache.emplace(key, vts::CsConvertor(from, to)).first;
    }
    return icache->second;
}

} // namespace

boost::optional<vts::MetaTile>
metatileFromStore(const vts::TileId &tileId
                  , const mnstore::Store &store
                  , const Resource &resource
                  , const mmapped::TileIndex &tileIndex
                  , const MetatileOverrides &overrides)
{
    auto blocks(metatileBlocks(resource, tileId));

    if (blocks.empty()) {
        utility::raise<NotFound>
            ("Metatile completely outside of configured range.");
    }

    const auto &rf(*resource.referenceFrame);

    vts::MetaTile metatile(tileId, rf.metaBinaryOrder);

    const std::size_t internalTextureCount
        (overrides.textureMode == vts::SubMesh::internal);

    const auto credits(overrides.mergedCredits(resource.credits));

    // the store page covering this metatile (single page: page shape
    // equals the delivery unit)
    mnstore::Page page;
    const bool havePage(store.read(tileId, page));

    auto setChildren([&](const MetatileBlock &block
                         , const vts::TileId &nodeId, vts::MetaNode &node)
                     -> void
    {
        if (!block.commonAncestor.partial()) {
            // fully covered RF subtree: copy tileindex subtree validity
            for (const auto &child : vts::children(nodeId)) {
                node.setChildFromId(child, tileIndex.validSubtree(child));
            }
            return;
        }

        // not fully valid: generate this node's validity info
        vts::NodeInfo ni(rf, nodeId);
        if (!ni.valid()) { return; }

        for (const auto &child : vts::children(nodeId)) {
            bool valid(tileIndex.validSubtree(child)
                       && ni.child(child).valid());
            node.setChildFromId(child, valid);
        }
    });

    for (const auto &block : blocks) {
        const auto &view(block.view);
        const auto extents(block.extents);
        const auto es(math::size(extents));
        const math::Size2 bSize(vts::tileRangesSize(view));

        const math::Size2f ts(es.width / bSize.width
                              , es.height / bSize.height);

        if (!block.commonAncestor.productive()) {
            // unproductive node: flags and children only
            for (int j(0); j < bSize.height; ++j) {
                for (int i(0); i < bSize.width; ++i) {
                    const vts::TileId nodeId
                        (tileId.lod, view.ll(0) + i, view.ll(1) + j);
                    vts::MetaNode node;
                    node.flags(ti2metaFlags(tileIndex.get(nodeId)));
                    setChildren(block, nodeId, node);
                    metatile.set(nodeId, node);
                }
            }
            continue;
        }

        // raw-SDS converters: physical for the planar texel, navigation
        // SRS for the navtile height range
        const auto &conv
            (cachedConvertor(block.srs, rf.model.physicalSrs));
        const auto &navConv
            (cachedConvertor(block.srs, rf.model.navigationSrs));

        // physical-space corner grid (planarSamples x planarSamples
        // quads per tile, corners shared between neighbours)
        const int samples(planarSamples(tileId.lod));
        const math::Size2 gridSize(bSize.width * samples + 1
                                   , bSize.height * samples + 1);
        std::vector<math::Point3> grid;
        grid.reserve(gridSize.width * gridSize.height);
        {
            const math::Size2f gts(es.width / (samples * bSize.width)
                                   , es.height / (samples * bSize.height));
            for (int j(0); j < gridSize.height; ++j) {
                const double y(extents.ur(1) - j * gts.height);
                for (int i(0); i < gridSize.width; ++i) {
                    const double x(extents.ll(0) + i * gts.width);
                    grid.push_back(conv(math::Point3(x, y, 0.0)));
                }
            }
        }

        const auto gridPoint([&](int col, int row) -> const math::Point3*
        {
            return &grid[row * gridSize.width + col];
        });

        // planar physical area of one tile cell
        const auto cellArea([&](int i, int j) -> double
        {
            double area(0.0);
            for (int row(j * samples + 1); row <= (j + 1) * samples;
                 ++row)
            {
                for (int col(i * samples + 1); col <= (i + 1) * samples;
                     ++col)
                {
                    const auto qa(quadArea
                                  (gridPoint(col - 1, row - 1)
                                   , gridPoint(col, row)
                                   , gridPoint(col - 1, row)
                                   , gridPoint(col, row - 1)));
                    area += std::get<0>(qa);
                }
            }
            return area;
        });

        for (int j(0); j < bSize.height; ++j) {
            for (int i(0); i < bSize.width; ++i) {
                const vts::TileId nodeId
                    (tileId.lod, view.ll(0) + i, view.ll(1) + j);

                vts::MetaNode node;
                const auto tiFlags(tileIndex.get(nodeId));
                node.flags(ti2metaFlags(tiFlags));
                const bool geometry(node.geometry());
                const bool navtile(node.navtile());

                setChildren(block, nodeId, node);

                if (geometry || navtile) {
                    // real tile: needs stored payload
                    if (!havePage) {
                        LOG(info2)
                            << "Metanode store has no page for metatile "
                            << tileId << " with real tiles; falling "
                            "back to warp.";
                        return boost::none;
                    }
                    const auto &data(page.node(nodeId));
                    if (!data) {
                        LOG(warn2)
                            << "Metanode store page " << page.root()
                            << " has no payload for real tile "
                            << nodeId << "; falling back to warp.";
                        return boost::none;
                    }

                    const double minZ(data.min());
                    const double maxZ(data.max());

                    // full-cell horizontal extents (conservative),
                    // stored height range, midpoint surrogate
                    const math::Extents2 cell
                        (extents.ll(0) + i * ts.width
                         , extents.ur(1) - (j + 1) * ts.height
                         , extents.ll(0) + (i + 1) * ts.width
                         , extents.ur(1) - j * ts.height);

                    node.geomExtents.extents
                        = vts::GeomExtents::Extents
                        (cell.ll(0), cell.ll(1), cell.ur(0), cell.ur(1));
                    node.geomExtents.z
                        = vts::GeomExtents::ZRange(minZ, maxZ);
                    node.geomExtents.makeAverageSurrogate();

                    if (navtile) {
                        const auto center(math::center(cell));
                        const auto navMin
                            (navConv(math::Point3
                                     (center(0), center(1), minZ))(2));
                        const auto navMax
                            (navConv(math::Point3
                                     (center(0), center(1), maxZ))(2));
                        node.heightRange.min = std::floor(navMin);
                        node.heightRange.max = std::ceil(navMax);
                    }

                    if (geometry) {
                        node.updateCredits(credits);
                        node.internalTextureCount(internalTextureCount);
                        node.applyTexelSize(true);

                        // calibrated relief-corrected analytic texel
                        // size (RFC 7 section 5)
                        const auto area(cellArea(i, j));
                        const auto edge(std::sqrt(area));
                        const auto relief(maxZ - minZ);
                        /* Real terrain relief stays well below the
                         * tile edge; larger ratios only arise from
                         * data defects (nodata sentinels leaking into
                         * the source as valid values). Clamp so a
                         * poisoned range cannot blow up LOD selection.
                         */
                        const auto reliefRatio
                            (std::min(edge > 0.0 ? relief / edge : 0.0
                                      , 2.0));
                        node.texelSize
                            = std::sqrt(area / vr::BoundLayer::tileArea())
                            * std::sqrt(1.0 + texelReliefCoefficient
                                        * reliefRatio * reliefRatio);
                    }
                }

                metatile.set(nodeId, node);
            }
        }
    }

    return metatile;
}
