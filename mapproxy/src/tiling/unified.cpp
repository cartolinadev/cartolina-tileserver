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

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <future>
#include <map>
#include <sstream>

#include <opencv2/core/core.hpp>

#include <boost/format.hpp>

#include "dbglog/dbglog.hpp"

#include "utility/streams.hpp"
#include "utility/md5.hpp"
#include "utility/path.hpp"

#include "math/geometry_core.hpp"

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>

#include "geo/geodataset.hpp"
#include "geo/csconvertor.hpp"
#include "geo/srsdef.hpp"

#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/csconvertor.hpp"

#include "unified.hpp"

namespace fs = boost::filesystem;
namespace vr = vtslibs::registry;
namespace vts = vtslibs::vts;

namespace tiling {

namespace {

typedef vts::TileIndex::Flag TiFlag;

/** Sentinel marking elevation cells with no valid source data; far
 *  outside any real elevation.
 */
const double ElevationNodata(-1.0e6);
const double ElevationValidLimit(-0.5e6);

/** Temporary VRT exposing the source's GDAL mask band as a warpable
 *  byte band; removed on destruction.
 */
class MaskVrt {
public:
    MaskVrt(const fs::path &source) {
        const auto ds(geo::GeoDataset::open(source));
        const auto size(ds.size());
        const auto gt(ds.geoTransform());

        path_ = fs::temp_directory_path()
            / fs::unique_path("tiling-mask-%%%%%%.vrt");

        std::ofstream file;
        file.exceptions(std::ostream::failbit | std::ostream::badbit);
        file.open(path_.string(), std::ostream::out | std::ostream::trunc);
        file.precision(18);
        file << "<VRTDataset rasterXSize=\"" << size.width
             << "\" rasterYSize=\"" << size.height << "\">\n"
             << "  <SRS>" << ds.srsWkt() << "</SRS>\n"
             << "  <GeoTransform>"
             << gt[0] << ", " << gt[1] << ", " << gt[2] << ", "
             << gt[3] << ", " << gt[4] << ", " << gt[5]
             << "</GeoTransform>\n"
             << "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
             << "    <SimpleSource>\n"
             << "      <SourceFilename relativeToVRT=\"0\">"
             << fs::absolute(source).string() << "</SourceFilename>\n"
             << "      <SourceBand>mask,1</SourceBand>\n"
             << "    </SimpleSource>\n"
             << "  </VRTRasterBand>\n"
             << "</VRTDataset>\n";
        file.close();
    }

    ~MaskVrt() {
        boost::system::error_code ec;
        fs::remove(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/** World membership test against the requested tile ranges.
 */
class World {
public:
    World(const vts::LodTileRange::list &tileRanges)
        : tileRanges_(tileRanges)
    {}

    bool operator()(vts::Lod lod, unsigned int x, unsigned int y) const {
        for (const auto &tr : tileRanges_) {
            const auto range(vts::shiftRange(tr, lod));
            if ((x >= range.ll(0)) && (x <= range.ur(0))
                && (y >= range.ll(1)) && (y <= range.ur(1)))
            {
                return true;
            }
        }
        return false;
    }

    /** Intersects the union's bounding box with given range.
     */
    boost::optional<vts::TileRange>
    intersect(vts::Lod lod, const vts::TileRange &range) const {
        boost::optional<vts::TileRange> out;
        for (const auto &tr : tileRanges_) {
            auto r(vts::shiftRange(tr, lod));
            r.ll(0) = std::max(r.ll(0), range.ll(0));
            r.ll(1) = std::max(r.ll(1), range.ll(1));
            r.ur(0) = std::min(r.ur(0), range.ur(0));
            r.ur(1) = std::min(r.ur(1), range.ur(1));
            if ((r.ll(0) > r.ur(0)) || (r.ll(1) > r.ur(1))) { continue; }
            if (!out) {
                out = r;
            } else {
                out->ll(0) = std::min(out->ll(0), r.ll(0));
                out->ll(1) = std::min(out->ll(1), r.ll(1));
                out->ur(0) = std::max(out->ur(0), r.ur(0));
                out->ur(1) = std::max(out->ur(1), r.ur(1));
            }
        }
        return out;
    }

private:
    const vts::LodTileRange::list &tileRanges_;
};

/** Per-lod working grids of one division node. All grids are indexed
 *  relative to range.ll.
 */
struct LodGrid {
    vts::TileRange range;
    cv::Mat exists;     // CV_8U, 0/1
    cv::Mat watertight; // CV_8U, 0/1
    cv::Mat minZ;       // CV_32F, raw SDS
    cv::Mat maxZ;       // CV_32F, raw SDS

    LodGrid() {}

    LodGrid(const vts::TileRange &range)
        : range(range)
        , exists(height(), width(), CV_8U, cv::Scalar(0))
        , watertight(height(), width(), CV_8U, cv::Scalar(0))
        , minZ(height(), width(), CV_32F, cv::Scalar(0))
        , maxZ(height(), width(), CV_32F, cv::Scalar(0))
    {}

    int width() const { return range.ur(0) - range.ll(0) + 1; }
    int height() const { return range.ur(1) - range.ll(1) + 1; }
};

/** SDS extents of a tile range inside a division node.
 */
math::Extents2 rangeExtents(const vr::ReferenceFrame::Division::Node &node
                            , vts::Lod lod, const vts::TileRange &range)
{
    const auto depth(lod - node.id.lod);
    const auto count(vts::TileRange::value_type(1) << depth);
    const auto nodeSize(math::size(node.extents));
    const math::Size2f ts(nodeSize.width / count, nodeSize.height / count);

    const auto originX(vts::TileRange::value_type(node.id.x) << depth);
    const auto originY(vts::TileRange::value_type(node.id.y) << depth);

    math::Extents2 extents;
    extents.ll(0) = node.extents.ll(0)
        + (double(range.ll(0)) - originX) * ts.width;
    extents.ur(0) = node.extents.ll(0)
        + (double(range.ur(0)) + 1.0 - originX) * ts.width;
    extents.ur(1) = node.extents.ur(1)
        - (double(range.ll(1)) - originY) * ts.height;
    extents.ll(1) = node.extents.ur(1)
        - (double(range.ur(1)) + 1.0 - originY) * ts.height;
    return extents;
}

/** Replicates the legacy warp scale measure (libgeo getScale): the
 *  size of one source pixel step expressed in destination warp-grid
 *  pixels; >= 1 means the destination samples at or beyond source
 *  resolution (upscaling).
 */
class TrueScale {
public:
    TrueScale(const geo::GeoDataset &src
              , const geo::SrsDefinition &nodeSrs)
        : fwd_(nodeSrs, src.srs()), bwd_(src.srs(), nodeSrs)
        , gt_(src.geoTransform())
    {}

    double operator()(const math::Point2 &center
                      , const math::Size2f &samplePx) const
    {
        try {
            return scaleImpl(center, samplePx);
        } catch (const std::exception&) {
            /* Out-of-domain conversion (e.g. a source pixel just past
             * the pole under a polar division node): the scale cannot
             * be determined; report 1.0 — libgeo's own fallback —
             * which keeps the navtile flag off.
             */
            return 1.0;
        }
    }

private:
    double scaleImpl(const math::Point2 &center
                     , const math::Size2f &samplePx) const
    {
        const auto srcGeo(fwd_(math::Point3(center(0), center(1), 0.0)));
        double row(0.0), col(0.0);
        gt_.geo2rowcol(srcGeo, row, col);

        const auto base(bwd_(gt_.rowcol2geo(row, col, 0.0)));
        const auto stepX(bwd_(gt_.rowcol2geo(row, col + 1.0, 0.0)));
        const auto stepY(bwd_(gt_.rowcol2geo(row + 1.0, col, 0.0)));

        const math::Point2 a((stepX(0) - base(0)) / samplePx.width
                             , (stepX(1) - base(1)) / samplePx.height);
        const math::Point2 b((stepY(0) - base(0)) / samplePx.width
                             , (stepY(1) - base(1)) / samplePx.height);

        return std::sqrt(std::abs(math::crossProduct(a, b)));
    }

    geo::CsConvertor fwd_;
    geo::CsConvertor bwd_;
    geo::GeoTransform gt_;
};

/** Value transform of stored heights: the optional (monotone) height
 *  function, applied post-aggregation per tile (RFC 7 section 4.2).
 *  No datum conversion happens here: the store keeps the
 *  geoid-shifted (orthometric) SDS vertical of the source values and
 *  the v6 serializer shifts to the raw SDS vertical at delivery.
 */
class ValueTransform {
public:
    ValueTransform(const UnifiedConfig &config)
        : heightFunction_(config.heightFunction)
    {}

    std::pair<double, double> operator()(double min, double max) const {
        if (heightFunction_) {
            min = (*heightFunction_)(min);
            max = (*heightFunction_)(max);
            if (min > max) { std::swap(min, max); }
        }
        return { min, max };
    }

private:
    HeightFunction::pointer heightFunction_;
};

/** RAII GDAL dataset handle.
 */
class GdalHandle {
public:
    GdalHandle(const fs::path &path)
        : handle_(::GDALOpen(path.string().c_str(), GA_ReadOnly))
    {
        if (!handle_) {
            LOGTHROW(err2, std::runtime_error)
                << "Cannot open GDAL dataset " << path << ".";
        }
    }

    ~GdalHandle() { if (handle_) { ::GDALClose(handle_); } }

    GdalHandle(const GdalHandle&) = delete;
    GdalHandle& operator=(const GdalHandle&) = delete;

    ::GDALDatasetH get() const { return handle_; }

private:
    ::GDALDatasetH handle_;
};

/** Runs one one-pixel-per-tile GDAL filter pass via the GDALWarp
 *  utility API (the gdalwarp code path): destination grid of one
 *  pixel per tile, source read at base resolution (-ovr NONE), GDAL
 *  min/max kernel doing the whole reduction.
 *
 *  NB: not the libgeo warpInto path — that one degenerates at the
 *  extreme downsample ratio of this pass and its safe-chunks logic
 *  may silently swap in an (averaging) overview, which would bias the
 *  reduced range.
 */
cv::Mat filterPass(const fs::path &srcPath
                   , const geo::SrsDefinition &dstSrs
                   , const math::Size2i &size
                   , const math::Extents2 &extents
                   , GDALDataType dataType
                   , const boost::optional<double> &dstNodata
                   , const char *resampling
                   , const std::string &what)
{
    // pass-private dataset handle: the four passes run concurrently
    // and GDAL dataset handles are not thread-safe
    const GdalHandle source(srcPath);
    auto src(source.get());

    char **argv(nullptr);
    const auto add([&](const std::string &arg)
    {
        argv = ::CSLAddString(argv, arg.c_str());
    });

    const auto number([](double value)
    {
        std::ostringstream os;
        os.precision(17);
        os << value;
        return os.str();
    });

    add("-of"); add("MEM");
    add("-r"); add(resampling);
    add("-ts");
    add(std::to_string(size.width)); add(std::to_string(size.height));
    add("-te");
    add(number(extents.ll(0))); add(number(extents.ll(1)));
    add(number(extents.ur(0))); add(number(extents.ur(1)));
    add("-t_srs");
    add(dstSrs.as(geo::SrsDefinition::Type::wkt).srs);
    add("-ovr"); add("NONE");
    add("-wm"); add("500");
    add("-multi");
    add("-wo"); add("NUM_THREADS=ALL_CPUS");
    add("-ot"); add(::GDALGetDataTypeName(dataType));
    if (dstNodata) {
        add("-dstnodata"); add(number(*dstNodata));
    } else {
        /* No destination nodata: GDAL would nudge valid values that
         * collide with it (a computed mask 0 would become 1). Cells
         * never written by the warp must still read as "no data": 0.
         */
        add("-wo"); add("INIT_DEST=0");
    }

    auto *options(::GDALWarpAppOptionsNew(argv, nullptr));
    ::CSLDestroy(argv);
    if (!options) {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot build GDALWarp options.";
    }

    // per-decile progress so long planetary passes are not silent
    struct Progress {
        std::string what;
        int lastDecile = 0;
    } progress{ what, 0 };
    ::GDALWarpAppOptionsSetProgress
        (options, [](double complete, const char*, void *data) -> int
    {
        auto &progress(*static_cast<Progress*>(data));
        const int decile(complete * 10.0);
        if (decile > progress.lastDecile) {
            progress.lastDecile = decile;
            LOG(info3)
                << "Filter pass " << progress.what << ": "
                << (10 * decile) << "%.";
        }
        return TRUE;
    }, &progress);

    int usageError(0);
    auto out(::GDALWarp("filter-pass", nullptr, 1, &src, options
                        , &usageError));
    ::GDALWarpAppOptionsFree(options);
    if (!out || usageError) {
        LOGTHROW(err2, std::runtime_error)
            << "GDALWarp filter pass failed: <"
            << ::CPLGetLastErrorMsg() << ">.";
    }

    cv::Mat grid(size.height, size.width
                 , (dataType == GDT_Byte) ? CV_8U : CV_32F);
    const auto err
        (::GDALDataset::FromHandle(out)->GetRasterBand(1)->RasterIO
         (GF_Read, 0, 0, size.width, size.height, grid.data
          , size.width, size.height, dataType, 0, 0));
    ::GDALClose(out);
    if (err != CE_None) {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot read filter pass result.";
    }

    LOG(info3) << "Filter pass " << what << ": done.";

    return grid;
}

class UnifiedPass {
public:
    UnifiedPass(const fs::path &dataset
                , const vr::ReferenceFrame &referenceFrame
                , const vts::LodRange &lodRange
                , const vts::LodTileRange::list &tileRanges
                , const UnifiedConfig &config)
        : referenceFrame_(referenceFrame), lodRange_(lodRange)
        , world_(tileRanges), config_(config)
        , dataset_(dataset)
        , dem_(geo::GeoDataset::open(dataset))
        , maskVrt_(dataset)
    {
        storeHeader_.metaBinaryOrder = config.metaBinaryOrder;
        storeHeader_.metaDepth = config.metaDepth;

        for (const auto &item : referenceFrame.division.nodes) {
            const auto &node(item.second);
            if (node.partitioning.mode
                != vr::PartitioningMode::bisection)
            {
                continue;
            }
            processNode(node);
        }
    }

    UnifiedResult result() {
        UnifiedResult out;
        out.tileIndex = std::move(tileIndex_);
        out.pages.reserve(pages_.size());
        for (auto &item : pages_) {
            out.pages.push_back(std::move(item.second));
        }
        return out;
    }

private:
    void processNode(const vr::ReferenceFrame::Division::Node &node);

    void emit(const vr::ReferenceFrame::Division::Node &node
              , const geo::SrsDefinition &srsDef
              , vts::Lod lod, const LodGrid &grid);

    const vr::ReferenceFrame &referenceFrame_;
    const vts::LodRange lodRange_;
    const World world_;
    const UnifiedConfig &config_;

    const fs::path dataset_;
    geo::GeoDataset dem_;
    MaskVrt maskVrt_;

    mnstore::Header storeHeader_;
    vts::TileIndex tileIndex_;
    std::map<vts::TileId, mnstore::Page> pages_;
};

void UnifiedPass::processNode
    (const vr::ReferenceFrame::Division::Node &node)
{
    const auto leafLod(lodRange_.max);
    if (node.id.lod > leafLod) { return; }

    // node subtree range at leaf lod
    const auto depth(leafLod - node.id.lod);
    const vts::TileRange subtree
        (vts::TileRange::value_type(node.id.x) << depth
         , vts::TileRange::value_type(node.id.y) << depth
         , ((vts::TileRange::value_type(node.id.x) + 1) << depth) - 1
         , ((vts::TileRange::value_type(node.id.y) + 1) << depth) - 1);

    const auto leafRange(world_.intersect(leafLod, subtree));
    if (!leafRange) { return; }

    LOG(info3)
        << "Unified pass: processing division node " << node.id
        << " (srs: " << node.srs << "), leaf range "
        << leafLod << "/" << *leafRange << ".";

    const auto srsDef(vr::system.srs(node.srs).srsDef);
    const auto extents(rangeExtents(node, leafLod, *leafRange));
    const math::Size2i gridSize
        (leafRange->ur(0) - leafRange->ll(0) + 1
         , leafRange->ur(1) - leafRange->ll(1) + 1);

    // the four one-pixel-per-tile filter passes (RFC 7 section 4.2);
    // nodata rule per section 4.3: mask passes carry no source nodata
    // (the VRT mask band has none) with destinations initialized to 0,
    // elevation passes inherit source nodata so invalid pixels cannot
    // poison the range
    LOG(info3)
        << "Unified pass: running the four filter passes ("
        << gridSize << " px each).";
    auto maskMinFuture
        (std::async(std::launch::async, [&]() {
            return filterPass(maskVrt_.path(), srsDef, gridSize
                              , extents, GDT_Byte, boost::none, "min"
                              , "mask min");
        }));
    auto maskMaxFuture
        (std::async(std::launch::async, [&]() {
            return filterPass(maskVrt_.path(), srsDef, gridSize
                              , extents, GDT_Byte, boost::none, "max"
                              , "mask max");
        }));
    auto elevMinFuture
        (std::async(std::launch::async, [&]() {
            return filterPass(dataset_, srsDef, gridSize, extents
                              , GDT_Float32, ElevationNodata, "min"
                              , "elevation min");
        }));
    const auto elevMax
        (filterPass(dataset_, srsDef, gridSize, extents, GDT_Float32
                    , ElevationNodata, "max", "elevation max"));
    const auto maskMin(maskMinFuture.get());
    const auto maskMax(maskMaxFuture.get());
    const auto elevMin(elevMinFuture.get());

    const ValueTransform transform(config_);

    // assemble leaf grid
    LodGrid leaf(*leafRange);
    {
        std::size_t cells(0), holes(0);
        for (int j(0); j < leaf.height(); ++j) {
            const auto y(leafRange->ll(1) + j);
            for (int i(0); i < leaf.width(); ++i) {
                const auto x(leafRange->ll(0) + i);
                if (!world_(leafLod, x, y)) { continue; }

                if (!maskMax.at<std::uint8_t>(j, i)) { continue; }

                const double vMin(elevMin.at<float>(j, i));
                const double vMax(elevMax.at<float>(j, i));
                if ((vMin < ElevationValidLimit)
                    || (vMax < ElevationValidLimit))
                {
                    // mask claims data but elevation reduced nothing;
                    // treat as nonexistent
                    ++holes;
                    continue;
                }

                leaf.exists.at<std::uint8_t>(j, i) = 1;
                const bool watertight
                    (config_.forceWatertight
                     || (maskMin.at<std::uint8_t>(j, i) == 255));
                leaf.watertight.at<std::uint8_t>(j, i) = watertight;

                const auto range(transform(vMin, vMax));
                leaf.minZ.at<float>(j, i) = range.first;
                leaf.maxZ.at<float>(j, i) = range.second;
                ++cells;
            }
        }
        LOG(info3)
            << "Unified pass: leaf grid done, " << cells
            << " tiles exist" << (holes ? str
                                  (boost::format(", %d mask/elevation"
                                                 " mismatches") % holes)
                                  : std::string()) << ".";
    }

    // bottom-up 2x2 min/max mip loop with interleaved emission
    emit(node, srsDef, leafLod, leaf);

    auto child(std::move(leaf));
    const auto minLod(std::max(lodRange_.min, node.id.lod));
    for (auto lod(leafLod); lod > minLod;) {
        --lod;

        const vts::TileRange range
            (child.range.ll(0) >> 1, child.range.ll(1) >> 1
             , child.range.ur(0) >> 1, child.range.ur(1) >> 1);
        LodGrid parent(range);

        for (int j(0); j < parent.height(); ++j) {
            const auto y(range.ll(1) + j);
            for (int i(0); i < parent.width(); ++i) {
                const auto x(range.ll(0) + i);

                bool exists(false), watertight(true);
                float minZ(0.f), maxZ(0.f);
                int childCount(0);

                for (int cj(0); cj < 2; ++cj) {
                    for (int ci(0); ci < 2; ++ci) {
                        const auto cx(2 * x + ci), cy(2 * y + cj);
                        if ((cx < child.range.ll(0))
                            || (cx > child.range.ur(0))
                            || (cy < child.range.ll(1))
                            || (cy > child.range.ur(1)))
                        {
                            watertight = false;
                            continue;
                        }
                        const auto col(cx - child.range.ll(0));
                        const auto row(cy - child.range.ll(1));
                        if (!child.exists.at<std::uint8_t>(row, col)) {
                            watertight = false;
                            continue;
                        }

                        const auto cMin(child.minZ.at<float>(row, col));
                        const auto cMax(child.maxZ.at<float>(row, col));
                        if (!childCount) {
                            minZ = cMin;
                            maxZ = cMax;
                        } else {
                            minZ = std::min(minZ, cMin);
                            maxZ = std::max(maxZ, cMax);
                        }
                        ++childCount;
                        exists = true;
                        watertight = watertight
                            && child.watertight.at<std::uint8_t>
                            (row, col);
                    }
                }

                if (!exists) { continue; }
                parent.exists.at<std::uint8_t>(j, i) = 1;
                parent.watertight.at<std::uint8_t>(j, i)
                    = config_.forceWatertight || watertight;
                parent.minZ.at<float>(j, i) = minZ;
                parent.maxZ.at<float>(j, i) = maxZ;
            }
        }

        emit(node, srsDef, lod, parent);
        child = std::move(parent);
    }

    LOG(info3)
        << "Unified pass: division node " << node.id << " done.";
}

void UnifiedPass::emit(const vr::ReferenceFrame::Division::Node &node
                       , const geo::SrsDefinition &srsDef
                       , vts::Lod lod, const LodGrid &grid)
{
    // navtile eligibility: replicate the legacy rule — the navtile
    // flag survives while a tileSampling-per-tile warp still samples
    // coarser than the source (truescale < 1)
    const TrueScale trueScale(dem_, srsDef);
    const auto count(vts::TileRange::value_type(1) << (lod - node.id.lod));
    const auto nodeSize(math::size(node.extents));
    const math::Size2f ts(nodeSize.width / count, nodeSize.height / count);
    const math::Size2f samplePx(ts.width / config_.tileSampling
                                , ts.height / config_.tileSampling);
    const auto rangeExt(rangeExtents(node, lod, grid.range));

    const auto navtileEligible([&](int i, int j) -> bool
    {
        const math::Point2 center
            (rangeExt.ll(0) + (i + 0.5) * ts.width
             , rangeExt.ur(1) - (j + 0.5) * ts.height);
        return trueScale(center, samplePx) < 1.0;
    });

    std::size_t emitted(0);
    for (int j(0); j < grid.height(); ++j) {
        const auto y(grid.range.ll(1) + j);
        for (int i(0); i < grid.width(); ++i) {
            const auto x(grid.range.ll(0) + i);
            if (!grid.exists.at<std::uint8_t>(j, i)) { continue; }

            const vts::TileId tileId(lod, x, y);

            TiFlag::value_type flags(TiFlag::mesh);
            if (grid.watertight.at<std::uint8_t>(j, i)) {
                flags |= TiFlag::watertight;
            }
            if (navtileEligible(i, j)) {
                flags |= TiFlag::navtile;
            }
            tileIndex_.set(tileId, flags);

            // store page payload
            const auto pageId(storeHeader_.pageId(tileId));
            auto ipages(pages_.find(pageId));
            if (ipages == pages_.end()) {
                ipages = pages_.insert
                    ({ pageId, mnstore::Page(storeHeader_, pageId) })
                    .first;
            }
            auto &nodeData(ipages->second.node(tileId));
            nodeData.flags = mnstore::NodeData::mesh;
            if (grid.watertight.at<std::uint8_t>(j, i)) {
                nodeData.flags |= mnstore::NodeData::watertight;
            }
            nodeData.heightRange(grid.minZ.at<float>(j, i)
                                 , grid.maxZ.at<float>(j, i));
            ++emitted;
        }
    }

    LOG(info3)
        << "Unified pass: lod " << lod << ": emitted " << emitted
        << " tiles in range " << grid.range << ".";
}

} // namespace

UnifiedResult
generateUnified(const fs::path &dataset
                , const vr::ReferenceFrame &referenceFrame
                , const vts::LodRange &lodRange
                , const vts::LodTileRange::list &tileRanges
                , const UnifiedConfig &config)
{
    UnifiedPass pass(dataset, referenceFrame, lodRange, tileRanges
                     , config);
    return pass.result();
}

namespace {

/** Flushes file (or directory) content to disk.
 */
void fsyncPath(const fs::path &path)
{
    const int fd(::open(path.string().c_str(), O_RDONLY));
    if (fd < 0) {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot open " << path << " for fsync.";
    }
    ::fsync(fd);
    ::close(fd);
}

} // namespace

void publishUnified(const UnifiedResult &result
                    , const UnifiedConfig &config
                    , const std::string &referenceFrameId
                    , const vts::LodRange &lodRange
                    , const vts::LodTileRange::list &tileRanges
                    , const fs::path &tileIndexPath
                    , const fs::path &storePath)
{
    // source hash: every input that changes stored values
    const auto sourceHash([&]() -> std::string
    {
        std::ostringstream os;
        os << "referenceFrame=" << referenceFrameId
           << ";lodRange=" << lodRange
           << ";tileRanges=";
        for (const auto &range : tileRanges) { os << range << "+"; }
        os << ";geoidGrid="
           << (config.geoidGrid ? *config.geoidGrid : std::string())
           << ";heightFunction="
           << heightFunctionJson(config.heightFunction)
           << ";metaBinaryOrder=" << config.metaBinaryOrder
           << ";metaDepth=" << config.metaDepth;
        return utility::md5::hash_hex(os.str());
    }());

    // staged artifacts: write both to temporary names...
    const auto tiTmp(utility::addExtension(tileIndexPath, ".tmp"));
    LOG(info3) << "Saving tile index into staged " << tiTmp << ".";
    result.tileIndex.save(tiTmp);

    mnstore::Header header;
    header.metaBinaryOrder = config.metaBinaryOrder;
    header.metaDepth = config.metaDepth;
    header.referenceFrame = referenceFrameId;
    header.sourceHash = sourceHash;
    // ...pair the store with the staged flag index content...
    header.pairing = mnstore::fileDigest(tiTmp);
    if (config.geoidGrid) { header.geoidGrid = *config.geoidGrid; }
    header.heightFunction = heightFunctionJson(config.heightFunction);

    const auto storeTmp(utility::addExtension(storePath, ".tmp"));
    LOG(info3) << "Saving metanode store into staged " << storeTmp
               << " (" << result.pages.size() << " pages).";
    {
        mnstore::Writer writer(storeTmp, header);
        for (const auto &page : result.pages) { writer.write(page); }
        writer.close();
    }

    // ...fsync them and rename into place so a serving daemon sees
    // either the old pair or the new pair
    fsyncPath(tiTmp);
    fsyncPath(storeTmp);
    fs::rename(tiTmp, tileIndexPath);
    fs::rename(storeTmp, storePath);
    fsyncPath(tileIndexPath.parent_path());

    LOG(info3)
        << "Tile index " << tileIndexPath << " and metanode store "
        << storePath << " published (pairing " << header.pairing
        << ").";
}

} // namespace tiling
