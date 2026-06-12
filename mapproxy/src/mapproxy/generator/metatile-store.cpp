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
 * from the store, in the geoid-shifted orthometric SDS vertical) are
 * combined with fields derived at delivery: the raw-SDS height range
 * (geoid undulation evaluated at the block corners and bilinearly
 * interpolated — sub-half-ulp at metatile scales), full-cell
 * horizontal extents, midpoint surrogate, navtile height range
 * (geoid-shifted SDS to navigation SRS at the cell center, the warp
 * path's own conversion) and the calibrated relief-corrected analytic
 * texelSize.
 */

#include <cstdlib>
#include <mutex>

#include "utility/raise.hpp"

#include "geo/srsdef.hpp"
#include "geo/geodataset.hpp"

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

/** Per-tile planar-texel sampling density: tile corners at deep lods
 *  where the cell is locally flat, 2x2 quads below to track
 *  projection curvature. The phase-1 spike measured even
 *  corner-sampling within a few percent of the warp value at coarse
 *  lods, and coarse-lod texelSize only steers instantly-transited
 *  levels — denser sampling buys nothing but CsConvertor calls,
 *  which dominate coarse-metatile serve time.
 */
int planarSamples(vts::Lod lod)
{
    return (lod >= 10) ? 1 : 2;
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

/** Thread-cached convertor from the geoid-shifted SDS (the store's
 *  vertical datum) to given target SRS.
 */
const vts::CsConvertor&
cachedGeoidConvertor(const std::string &sds, const std::string &geoidGrid
                     , const std::string &to)
{
    thread_local std::map<std::string, vts::CsConvertor> cache;
    auto key(sds + "\0" + geoidGrid + "\0" + to);
    auto icache(cache.find(key));
    if (icache == cache.end()) {
        icache = cache.emplace
            (key, vts::CsConvertor
             (geo::setGeoid(vr::system.srs(sds).srsDef, geoidGrid), to))
            .first;
    }
    return icache->second;
}

/** Geoid grid sample spacing, in meters. Read once from the grid
 *  file header: the grid's own pitch is the scale below which
 *  bilinear interpolation of the undulation is as faithful as PROJ's
 *  own grid read. Falls back to 0.25 degree (EGM96-class) with a
 *  warning when the grid file cannot be located.
 */
double geoidGridSpacing(const std::string &geoidGrid)
{
    static std::mutex mutex;
    static std::map<std::string, double> cache;
    std::lock_guard<std::mutex> lock(mutex);

    auto icache(cache.find(geoidGrid));
    if (icache != cache.end()) { return icache->second; }

    double spacing(0.25); // degrees

    std::vector<boost::filesystem::path> dirs;
    if (const char *projLib = std::getenv("PROJ_LIB")) {
        dirs.push_back(projLib);
    }
    dirs.push_back("/usr/share/proj");
    dirs.push_back("/usr/local/share/proj");

    bool found(false);
    for (const auto &dir : dirs) {
        const auto path(dir / geoidGrid);
        if (!boost::filesystem::exists(path)) { continue; }
        try {
            const auto grid(geo::GeoDataset::open(path));
            const auto gt(grid.geoTransform());
            spacing = std::min(std::abs(gt[1]), std::abs(gt[5]));
            found = true;
            LOG(info2)
                << "Geoid grid " << path << ": sample spacing "
                << spacing << " deg.";
        } catch (const std::exception &e) {
            LOG(warn2)
                << "Cannot read geoid grid " << path << ": "
                << e.what() << ".";
        }
        break;
    }

    if (!found) {
        LOG(warn2)
            << "Geoid grid \"" << geoidGrid << "\" not found in PROJ "
            "search paths; assuming 0.25 degree sample spacing for "
            "undulation interpolation.";
    }

    cache[geoidGrid] = spacing;
    return spacing;
}

/** Geoid undulation sampled on a lattice over the block. The lattice
 *  density follows the block's footprint in the geoid grid itself:
 *  the block corners are projected into the grid's CRS and the
 *  bounding-box span is divided by the grid pitch, so lattice nodes
 *  land at (or below) grid-sample spacing and interpolating between
 *  them is as faithful as evaluating the (itself grid-interpolated)
 *  undulation per node. The density is capped; the cap is reached
 *  only on the few coarsest metatiles, where the residual
 *  sub-lattice error is meters against kilometer-scale ranges.
 */
class UndulationGrid {
public:
    /** Identity grid (no geoid configured). */
    UndulationGrid()
        : extents_(), cols_(1), rows_(1), values_(4, 0.0) {}

    UndulationGrid(const vts::CsConvertor &shift
                   , const vts::CsConvertor &toGridCrs
                   , const math::Extents2 &extents
                   , double spacing
                   , const math::Size2 &blockCells)
        : extents_(extents)
    {
        // block footprint in the geoid grid CRS (corner bounding box)
        math::Extents2 footprint(math::InvalidExtents{});
        for (const auto &corner : {
                math::Point2(extents.ll(0), extents.ll(1))
                , math::Point2(extents.ur(0), extents.ll(1))
                , math::Point2(extents.ll(0), extents.ur(1))
                , math::Point2(extents.ur(0), extents.ur(1)) })
        {
            const auto projected
                (toGridCrs(math::Point3(corner(0), corner(1), 0.0)));
            math::update(footprint
                         , math::Point2(projected(0), projected(1)));
        }
        const auto span(math::size(footprint));

        /* Lattice density: enough nodes to stay at the geoid grid's
         * own pitch, but never denser than two lattice cells per
         * metatile cell — beyond that the per-cell undulation spread
         * is already resolved, and the extra rows only add
         * conversion cost on coarse blocks whose height ranges dwarf
         * the residual (meters) anyway.
         */
        const auto cells([&](double span, int blockCells) -> int
        {
            if (!(span > 0.0) || !(spacing > 0.0)) { return 1; }
            const int byPitch(std::ceil(span / spacing));
            return std::max(1, std::min(std::min(64, 2 * blockCells)
                                        , byPitch));
        });
        cols_ = cells(span.width, blockCells.width);
        rows_ = cells(span.height, blockCells.height);

        const auto size(math::size(extents));
        values_.reserve((cols_ + 1) * (rows_ + 1));
        for (int row(0); row <= rows_; ++row) {
            const double y(extents.ll(1)
                           + size.height * row / rows_);
            for (int col(0); col <= cols_; ++col) {
                const double x(extents.ll(0)
                               + size.width * col / cols_);
                values_.push_back
                    (shift(math::Point3(x, y, 0.0))(2));
            }
        }
    }

    /** Undulation range over a cell: bilinear values at the cell
     *  corners plus all lattice nodes inside the cell (interior
     *  extremes of cells coarser than the lattice).
     */
    std::pair<double, double>
    range(const math::Extents2 &cell) const {
        auto low(bilinear(cell.ll(0), cell.ll(1)));
        auto high(low);
        const auto update([&](double value)
        {
            low = std::min(low, value);
            high = std::max(high, value);
        });
        update(bilinear(cell.ur(0), cell.ll(1)));
        update(bilinear(cell.ll(0), cell.ur(1)));
        update(bilinear(cell.ur(0), cell.ur(1)));

        // lattice nodes inside the cell
        const auto size(math::size(extents_));
        if ((size.width > 0.0) && (size.height > 0.0)) {
            const double stepX(size.width / cols_);
            const double stepY(size.height / rows_);
            const int col0(std::max
                           (0, int(std::ceil((cell.ll(0)
                                              - extents_.ll(0))
                                             / stepX))));
            const int col1(std::min
                           (cols_, int(std::floor((cell.ur(0)
                                                   - extents_.ll(0))
                                                  / stepX))));
            const int row0(std::max
                           (0, int(std::ceil((cell.ll(1)
                                              - extents_.ll(1))
                                             / stepY))));
            const int row1(std::min
                           (rows_, int(std::floor((cell.ur(1)
                                                   - extents_.ll(1))
                                                  / stepY))));
            for (int row(row0); row <= row1; ++row) {
                for (int col(col0); col <= col1; ++col) {
                    update(values_[row * (cols_ + 1) + col]);
                }
            }
        }

        return { low, high };
    }

private:
    double bilinear(double x, double y) const {
        const auto size(math::size(extents_));
        if (!(size.width > 0.0) || !(size.height > 0.0)) {
            return values_[0];
        }
        double fx((x - extents_.ll(0)) / size.width * cols_);
        double fy((y - extents_.ll(1)) / size.height * rows_);
        fx = std::min(std::max(fx, 0.0), double(cols_));
        fy = std::min(std::max(fy, 0.0), double(rows_));
        const int col(std::min(int(fx), cols_ - 1));
        const int row(std::min(int(fy), rows_ - 1));
        const double dx(fx - col);
        const double dy(fy - row);

        const auto at([&](int c, int r)
        {
            return values_[r * (cols_ + 1) + c];
        });
        const double bottom(at(col, row) * (1.0 - dx)
                            + at(col + 1, row) * dx);
        const double top(at(col, row + 1) * (1.0 - dx)
                         + at(col + 1, row + 1) * dx);
        return bottom * (1.0 - dy) + top * dy;
    }

    math::Extents2 extents_;
    int cols_;
    int rows_;
    std::vector<double> values_;
};

} // namespace

boost::optional<vts::MetaTile>
metatileFromStore(const vts::TileId &tileId
                  , const mnstore::Store &store
                  , const Resource &resource
                  , const mmapped::TileIndex &tileIndex
                  , const boost::optional<std::string> &geoidGrid
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

        // not fully valid: generate this node's validity info;
        // derive from the block ancestor (shares the constraint
        // sampler -- a fresh NodeInfo builds a PROJ pipeline)
        const auto ni(deriveNodeInfo(block.commonAncestor, nodeId));
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

        // derived-field converters and grids can fail on blocks that
        // reach outside a projection's domain (polar caps); fail the
        // whole metatile over to the warp path, which has its own
        // handling
        try {

        // physical converter for the planar texel; navigation-SRS
        // converter from the store's (geoid-shifted) datum for the
        // navtile height range — the warp path's own conversion
        const auto &conv
            (cachedConvertor(block.srs, rf.model.physicalSrs));
        const auto &navConv
            (geoidGrid
             ? cachedGeoidConvertor(block.srs, *geoidGrid
                                    , rf.model.navigationSrs)
             : cachedConvertor(block.srs, rf.model.navigationSrs));

        // orthometric-to-raw-SDS shift for the serialized height range
        const auto undulation
            (geoidGrid
             ? UndulationGrid
             (cachedGeoidConvertor(block.srs, *geoidGrid, block.srs)
              , cachedConvertor(block.srs, rf.model.navigationSrs)
              , extents, geoidGridSpacing(*geoidGrid), bSize)
             : UndulationGrid());

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
                    const auto center(math::center(cell));

                    /* Serialized z range is raw SDS: shift the stored
                     * orthometric range by the local undulation,
                     * widened by its within-cell spread so the range
                     * still covers the mesh, which samples the
                     * undulation per vertex.
                     */
                    const auto shift(undulation.range(cell));
                    node.geomExtents.extents
                        = vts::GeomExtents::Extents
                        (cell.ll(0), cell.ll(1), cell.ur(0), cell.ur(1));
                    node.geomExtents.z
                        = vts::GeomExtents::ZRange(minZ + shift.first
                                                   , maxZ + shift.second);
                    node.geomExtents.makeAverageSurrogate();

                    if (navtile) {
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

        } catch (const std::exception &e) {
            LOG(warn2)
                << "Metanode store: cannot derive block fields for "
                << tileId << ": <" << e.what()
                << ">; falling back to warp.";
            return boost::none;
        }
    }

    return metatile;
}
