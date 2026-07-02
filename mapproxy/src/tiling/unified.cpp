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

#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

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
#include "geo/srsfactors.hpp"

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

/** Bounded FIFO executor for filter-pass warps.
 */
class WarpPool {
public:
    WarpPool(int workers) {

        threads_.reserve(workers);
        for (int index(0); index < workers; ++index)
            threads_.emplace_back([this]() { run(); });
    }

    ~WarpPool() {

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto &thread : threads_) thread.join();
    }

    std::future<cv::Mat> submit(std::function<cv::Mat()> function) {

        auto task(std::make_shared<std::packaged_task<cv::Mat()>>
                  (std::move(function)));
        auto future(task->get_future());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace_back([task]() { (*task)(); });
        }
        ready_.notify_one();
        return future;
    }

private:
    void run() {

        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

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

/** Per-node spatial floor-depth lookup for the prune (spatially-varying
 *  bottom LOD). Mirrors mapproxy-calipers' Node::sample depth formula
 *
 *      bestLod(p) = 0.5 * log2( paneArea / (arealScale(p) * tileArea * G^2) )
 *
 *  but evaluates the SRS areal scale factor per tile centre rather than
 *  only at the node centre. Where the projection inflates area the same
 *  tile covers less ground, so fewer LODs resolve the source there; the
 *  prune drops tiles below that per-location floor. At the node centre
 *  the depth equals calipers' own localLod, so the coverage a calipers
 *  measurement promises is preserved.
 *
 *  The scale is sampled on a fixed lattice and log2(arealScale) is
 *  bilinearly interpolated (bestLod is linear in it, so ceil is stable).
 *  A denser adaptive lattice is a deliberately deferred optimization.
 */
class GsdGrid {
public:
    static const int lattice = 64; // (lattice + 1)^2 SrsFactors samples

    GsdGrid(const geo::SrsDefinition &srsDef, const math::Extents2 &extents
            , double gsd, int extraLods)
        : extents_(extents), extraLods_(extraLods)
        , log2scale_((lattice + 1) * (lattice + 1)
                     , std::numeric_limits<double>::quiet_NaN())
    {
        const auto paneSize(math::size(extents));

        // depth constant: bestLod(p) = depthConst_ - 0.5*log2(arealScale(p))
        depthConst_ = 0.5 * std::log2
            (paneSize.width * paneSize.height
             / (vr::BoundLayer::tileArea() * gsd * gsd));

        geo::SrsFactors factors(srsDef);
        for (int j(0); j <= lattice; ++j) {
            const double y(extents.ll(1) + paneSize.height * j / lattice);
            for (int i(0); i <= lattice; ++i) {
                const double x(extents.ll(0) + paneSize.width * i / lattice);
                try {
                    const auto scale
                        (factors(math::Point2(x, y)).arealScaleFactor);
                    if (scale > 0.0) {
                        log2scale_[j * (lattice + 1) + i]
                            = std::log2(scale);
                        anyValid_ = true;
                    }
                } catch (const std::exception&) {
                    // out-of-domain sample (e.g. past a pole): left NaN,
                    // resolved to the nearest valid sample at lookup
                }
            }
        }
    }

    bool anyValid() const { return anyValid_; }

    /** Local floor depth (relative to the node root) at an SDS point. */
    int floorDepth(const math::Point2 &p) const {
        const auto bestLod(depthConst_ - 0.5 * log2scaleAt(p));
        if (!(bestLod > 0.0)) { return extraLods_; }
        return int(std::ceil(bestLod)) + extraLods_;
    }

    /** Maximum floor depth over the sampled lattice: the deepest LOD
     *  any tile of the node can survive the prune, hence the node's
     *  leaf-LOD cap.
     */
    int maxFloorDepth() const {
        double minLog2(std::numeric_limits<double>::infinity());
        for (const auto v : log2scale_) {
            if (!std::isnan(v)) { minLog2 = std::min(minLog2, v); }
        }
        if (!std::isfinite(minLog2)) { return extraLods_; }
        const auto bestLod(depthConst_ - 0.5 * minLog2);
        if (!(bestLod > 0.0)) { return extraLods_; }
        return int(std::ceil(bestLod)) + extraLods_;
    }

private:
    /** Bilinearly interpolated log2(arealScale) at an SDS point,
     *  falling back to the nearest valid lattice sample when the
     *  interpolation stencil hits an out-of-domain (NaN) corner.
     */
    double log2scaleAt(const math::Point2 &p) const {
        const auto size(math::size(extents_));
        double fx((size.width > 0.0)
                  ? (p(0) - extents_.ll(0)) / size.width * lattice : 0.0);
        double fy((size.height > 0.0)
                  ? (p(1) - extents_.ll(1)) / size.height * lattice : 0.0);
        fx = std::min(std::max(fx, 0.0), double(lattice));
        fy = std::min(std::max(fy, 0.0), double(lattice));
        const int col(std::min(int(fx), lattice - 1));
        const int row(std::min(int(fy), lattice - 1));
        const double dx(fx - col), dy(fy - row);

        const auto at([&](int c, int r) -> double
        {
            return log2scale_[r * (lattice + 1) + c];
        });
        const double v00(at(col, row)), v10(at(col + 1, row));
        const double v01(at(col, row + 1)), v11(at(col + 1, row + 1));

        if (std::isnan(v00) || std::isnan(v10)
            || std::isnan(v01) || std::isnan(v11))
        {
            return nearestValid(fx, fy);
        }
        const double bottom(v00 * (1.0 - dx) + v10 * dx);
        const double top(v01 * (1.0 - dx) + v11 * dx);
        return bottom * (1.0 - dy) + top * dy;
    }

    double nearestValid(double fx, double fy) const {
        double best(std::numeric_limits<double>::quiet_NaN());
        double bestD2(std::numeric_limits<double>::infinity());
        for (int j(0); j <= lattice; ++j) {
            for (int i(0); i <= lattice; ++i) {
                const auto v(log2scale_[j * (lattice + 1) + i]);
                if (std::isnan(v)) { continue; }
                const double d2((i - fx) * (i - fx) + (j - fy) * (j - fy));
                if (d2 < bestD2) { bestD2 = d2; best = v; }
            }
        }
        return best;
    }

    math::Extents2 extents_;
    int extraLods_;
    double depthConst_ = 0.0;
    bool anyValid_ = false;
    std::vector<double> log2scale_;
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
    LOG(info3) << "Filter pass " << what << ": started.";

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

        /* All elevation warps enter the bounded FIFO pool before mask
         * warps. The main thread reduces each node as soon as all its
         * passes finish; reduction remains single-threaded because it
         * writes the shared tile index and store pages.
         */
        std::deque<NodeJob> jobs;
        WarpPool pool(config.warpConcurrency);
        for (const auto &item : referenceFrame.division.nodes) {
            const auto &node(item.second);
            if (node.partitioning.mode
                != vr::PartitioningMode::bisection)
            {
                continue;
            }
            prepareNode(node, jobs);
        }

        schedulePasses(pool, jobs);
        for (std::size_t reduced(0); reduced < jobs.size(); ++reduced) {
            NodeJob *job;
            {
                std::unique_lock<std::mutex> lock(completionMutex_);
                completion_.wait(lock, [&]() {
                    return !completedJobs_.empty();
                });
                job = completedJobs_.front();
                completedJobs_.pop_front();
            }
            reduceNode(*job);
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
    /** One division node's pooled warps plus the geometry the
     *  reduction needs.
     */
    struct NodeJob {
        vr::ReferenceFrame::Division::Node node;
        geo::SrsDefinition srsDef;
        vts::Lod leafLod;
        vts::TileRange leafRange;
        math::Extents2 extents;
        math::Size2i gridSize;
        std::shared_ptr<GsdGrid> pruneGrid;
        std::future<cv::Mat> maskMin, maskMax, elevMin, elevMax;
        int completedPasses = 0;
    };

    void prepareNode(const vr::ReferenceFrame::Division::Node &node
                     , std::deque<NodeJob> &jobs);

    void schedulePasses(WarpPool &pool, std::deque<NodeJob> &jobs);

    std::future<cv::Mat> schedulePass
        (WarpPool &pool, NodeJob &job, const fs::path &source
         , GDALDataType dataType, const boost::optional<double> &nodata
         , const char *resampling, const std::string &what);

    void passCompleted(NodeJob &job);

    void reduceNode(NodeJob &job);

    void emit(const vr::ReferenceFrame::Division::Node &node
              , const geo::SrsDefinition &srsDef
              , vts::Lod lod, const LodGrid &grid
              , const GsdGrid *pruneGrid);

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

    std::mutex completionMutex_;
    std::condition_variable completion_;
    std::deque<NodeJob*> completedJobs_;
};

void UnifiedPass::prepareNode
    (const vr::ReferenceFrame::Division::Node &node
     , std::deque<NodeJob> &jobs)
{
    if (node.id.lod > lodRange_.max) { return; }
    const auto srsDefinition(vr::system.srs(node.srs).srsDef);

    /* Prune: build the node's spatial floor-depth grid and cap the leaf
     * LOD at the deepest depth any tile of the node survives. A node
     * whose projection does not inflate area keeps the full analyzed
     * depth; where it does inflate, the cap is shallower and the warp
     * grid shrinks with it.
     */
    std::shared_ptr<GsdGrid> pruneGrid;
    auto leafLod(lodRange_.max);
    if (config_.pruneGsd) {
        pruneGrid = std::make_shared<GsdGrid>
            (srsDefinition, node.extents, *config_.pruneGsd
             , config_.pruneExtraLods);
        if (pruneGrid->anyValid()) {
            const auto capped(node.id.lod + pruneGrid->maxFloorDepth());
            leafLod = std::min<vts::Lod>(leafLod, capped);
        } else {
            LOG(warn3)
                << "Unified pass: division node " << node.id
                << " has no valid projection scale samples; prune "
                "disabled for it.";
            pruneGrid.reset();
        }
    }
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

    jobs.emplace_back();
    auto &job(jobs.back());
    job.node = node;
    job.srsDef = srsDefinition;
    job.leafLod = leafLod;
    job.pruneGrid = pruneGrid;
    job.leafRange = *leafRange;
    job.extents = rangeExtents(node, leafLod, *leafRange);
    job.gridSize = math::Size2i
        (leafRange->ur(0) - leafRange->ll(0) + 1
         , leafRange->ur(1) - leafRange->ll(1) + 1);

    LOG(info3)
        << "Unified pass: scheduling division node " << node.id
        << " (srs: " << node.srs << "), leaf range "
        << leafLod << "/" << *leafRange << ".";

}

std::future<cv::Mat> UnifiedPass::schedulePass
    (WarpPool &pool, NodeJob &job, const fs::path &source
     , GDALDataType dataType, const boost::optional<double> &nodata
     , const char *resampling, const std::string &what)
{
    const auto srsDef(job.srsDef);
    const auto gridSize(job.gridSize);
    const auto extents(job.extents);
    return pool.submit([this, &job, source, dataType, nodata, resampling
                        , what, srsDef, gridSize, extents]() {

        try {

            auto result(filterPass(source, srsDef, gridSize, extents
                                   , dataType, nodata, resampling, what));
            passCompleted(job);
            return result;

        } catch (...) {

            passCompleted(job);
            throw;
        }
    });
}

void UnifiedPass::passCompleted(NodeJob &job)
{
    std::lock_guard<std::mutex> lock(completionMutex_);
    ++job.completedPasses;
    const int required(config_.coverage ? 2 : 4);
    if (job.completedPasses == required) {
        completedJobs_.push_back(&job);
        completion_.notify_one();
    }
}

void UnifiedPass::schedulePasses
    (WarpPool &pool, std::deque<NodeJob> &jobs)
{
    /* The four one-pixel-per-tile filter passes follow RFC 7 sections
     * 4.2 and 4.3. Elevation dominates because it decompresses the
     * full source data; globally queue it first so mask work fills the
     * tail instead of delaying the long critical path.
     */
    std::vector<NodeJob*> order;
    order.reserve(jobs.size());
    for (auto &job : jobs) order.push_back(&job);
    std::stable_sort(order.begin(), order.end()
                     , [](const NodeJob *left, const NodeJob *right) {

        const auto cells([](const NodeJob *job) {

            return std::uint64_t(job->gridSize.width)
                * std::uint64_t(job->gridSize.height);
        });
        return cells(left) > cells(right);
    });

    if (!config_.coverage) {
        for (auto *job : order) {
            const auto name(str(boost::format("%s elevation min")
                                % job->node.id));
            job->elevMin = schedulePass
                (pool, *job, dataset_, GDT_Float32, ElevationNodata, "min"
                 , name);
            const auto maxName(str(boost::format("%s elevation max")
                                   % job->node.id));
            job->elevMax = schedulePass
                (pool, *job, dataset_, GDT_Float32, ElevationNodata, "max"
                 , maxName);
        }
    }

    for (auto *job : order) {
        const auto name(str(boost::format("%s mask min") % job->node.id));
        job->maskMin = schedulePass
            (pool, *job, maskVrt_.path(), GDT_Byte, boost::none, "min"
             , name);
        const auto maxName
            (str(boost::format("%s mask max") % job->node.id));
        job->maskMax = schedulePass
            (pool, *job, maskVrt_.path(), GDT_Byte, boost::none, "max"
             , maxName);
    }
}

void UnifiedPass::reduceNode(NodeJob &job)
{
    const auto &node(job.node);
    const auto &srsDef(job.srsDef);
    const auto leafLod(job.leafLod);
    const auto &leafRange(job.leafRange);
    const auto *pruneGrid(job.pruneGrid.get());

    const auto maskMin(job.maskMin.get());
    const auto maskMax(job.maskMax.get());
    cv::Mat elevMin, elevMax;
    if (!config_.coverage) {
        elevMin = job.elevMin.get();
        elevMax = job.elevMax.get();
    }

    LOG(info3)
        << "Unified pass: reducing division node " << node.id << ".";

    const ValueTransform transform(config_);

    // assemble leaf grid
    LodGrid leaf(leafRange);
    {
        std::size_t cells(0), holes(0);
        for (int j(0); j < leaf.height(); ++j) {
            const auto y(leafRange.ll(1) + j);
            for (int i(0); i < leaf.width(); ++i) {
                const auto x(leafRange.ll(0) + i);
                if (!world_(leafLod, x, y)) { continue; }

                if (!maskMax.at<std::uint8_t>(j, i)) { continue; }

                if (!config_.coverage) {
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

                    const auto range(transform(vMin, vMax));
                    leaf.minZ.at<float>(j, i) = range.first;
                    leaf.maxZ.at<float>(j, i) = range.second;
                }

                // coverage mode: existence is the mask alone
                leaf.exists.at<std::uint8_t>(j, i) = 1;
                const bool watertight
                    (config_.forceWatertight
                     || (maskMin.at<std::uint8_t>(j, i) == 255));
                leaf.watertight.at<std::uint8_t>(j, i) = watertight;
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
    emit(node, srsDef, leafLod, leaf, pruneGrid);

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

        emit(node, srsDef, lod, parent, pruneGrid);
        child = std::move(parent);
    }

    LOG(info3)
        << "Unified pass: division node " << node.id << " done.";
}

void UnifiedPass::emit(const vr::ReferenceFrame::Division::Node &node
                       , const geo::SrsDefinition &srsDef
                       , vts::Lod lod, const LodGrid &grid
                       , const GsdGrid *pruneGrid)
{
    // navtile eligibility: replicate the legacy rule — the navtile
    // flag survives while a tileSampling-per-tile warp still samples
    // coarser than the source (truescale < 1)
    const TrueScale trueScale(dem_, srsDef);
    const auto depth(lod - node.id.lod);
    const auto count(vts::TileRange::value_type(1) << depth);
    const auto nodeSize(math::size(node.extents));
    const math::Size2f ts(nodeSize.width / count, nodeSize.height / count);
    const math::Size2f samplePx(ts.width / config_.tileSampling
                                , ts.height / config_.tileSampling);
    const auto rangeExt(rangeExtents(node, lod, grid.range));

    const auto tileCenter([&](int i, int j) -> math::Point2
    {
        return math::Point2
            (rangeExt.ll(0) + (i + 0.5) * ts.width
             , rangeExt.ur(1) - (j + 0.5) * ts.height);
    });

    const auto allChildrenCanBePruned([&](auto x, auto y)
    {
        for (int c(0); c < 4; ++c) {

            const int i(int(((x >> 1) << 1) + (c & 1))
                        - int(grid.range.ll(0)));
            const int j(int(((y >> 1) << 1) + (c >> 1))
                        - int(grid.range.ll(1)));
            if (depth <= pruneGrid->floorDepth(tileCenter(i, j)))
                return false;
        }
        return true;
    });

    std::size_t emitted(0), pruned(0);
    for (int j(0); j < grid.height(); ++j) {
        const auto y(grid.range.ll(1) + j);
        for (int i(0); i < grid.width(); ++i) {
            const auto x(grid.range.ll(0) + i);
            if (!grid.exists.at<std::uint8_t>(j, i)) { continue; }

            const auto center(tileCenter(i, j));

            if (pruneGrid && (depth > 0)
                && allChildrenCanBePruned(x, y))
            {
                ++pruned;
                continue;
            }

            const vts::TileId tileId(lod, x, y);
            const bool watertight
                (grid.watertight.at<std::uint8_t>(j, i));

            // coverage mode emits the flag index only, no store
            if (!config_.coverage) {
                // store page payload records the tile's true coverage
                // (watertight independent of any index suppression)
                const auto pageId(storeHeader_.pageId(tileId));
                auto ipages(pages_.find(pageId));
                if (ipages == pages_.end()) {
                    ipages = pages_.insert
                        ({ pageId, mnstore::Page(storeHeader_, pageId) })
                        .first;
                }
                auto &nodeData(ipages->second.node(tileId));
                nodeData.coverage
                    = (watertight
                       ? mnstore::NodeData::Coverage::full
                       : mnstore::NodeData::Coverage::partial);
                nodeData.heightRange(grid.minZ.at<float>(j, i)
                                     , grid.maxZ.at<float>(j, i));
            }

            /* skipPartial: a non-watertight tile carries no mesh in the
             * served index — its whole flag entry stays 0 (a navtile-only
             * entry would get mesh resurrected by the served-index
             * combiner). Clearing mesh clears watertight with it (a
             * meshless node cannot be watertight), which the empty entry
             * satisfies by construction. Descendants keep their own entries;
             * validSubtree preserves the branch only while any survive.
             */
            if (config_.skipPartial && !watertight) { continue; }

            TiFlag::value_type flags(TiFlag::mesh);
            if (watertight) { flags |= TiFlag::watertight; }
            // navtile is a surface (DEM) concept; imagery has none, so
            // coverage mode never sets it
            if (!config_.coverage
                && (trueScale(center, samplePx) < 1.0))
            {
                flags |= TiFlag::navtile;
            }
            tileIndex_.set(tileId, flags);
            ++emitted;
        }
    }

    LOG(info3)
        << "Unified pass: lod " << lod << ": emitted " << emitted
        << (pruned ? str(boost::format(", pruned %d") % pruned)
            : std::string())
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

/** Publishes a flag tile index and metanode store pair. Stages both to
 *  temporary names, pairs the store to the staged index content
 *  (header.pairing filled here), fsyncs, and renames them sequentially. The
 *  digest makes a mixed generation detectable. Shared by the generation and
 *  re-flag paths.
 */
void publishPair(const vts::TileIndex &tileIndex
                 , const mnstore::Page::list &pages
                 , mnstore::Header header
                 , const fs::path &tileIndexPath
                 , const fs::path &storePath)
{
    // staged artifacts: write both to temporary names...
    const auto tiTmp(utility::addExtension(tileIndexPath, ".tmp"));
    LOG(info3) << "Saving tile index into staged " << tiTmp << ".";
    tileIndex.save(tiTmp);

    // ...pair the store with the staged flag index content...
    header.pairing = mnstore::fileDigest(tiTmp);

    const auto storeTmp(utility::addExtension(storePath, ".tmp"));
    LOG(info3) << "Saving metanode store into staged " << storeTmp
               << " (" << pages.size() << " pages).";
    {
        mnstore::Writer writer(storeTmp, header);
        for (const auto &page : pages) { writer.write(page); }
        writer.close();
    }

    // ...fsync them and replace both paths. The pairing digest makes the
    // intermediate mixed generation detectable by readers.
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

} // namespace

void publishUnified(const UnifiedResult &result
                    , const UnifiedConfig &config
                    , const std::string &referenceFrameId
                    , const vts::LodRange &lodRange
                    , const vts::LodTileRange::list &tileRanges
                    , const fs::path &tileIndexPath
                    , const fs::path &storePath)
{
    // source hash: every input that changes stored values. skipPartial
    // and prune are index-side delivery policies bound by the pairing
    // digest, not stored-value inputs, so they stay out of the hash —
    // the serving daemon recomputes this hash from the resource
    // definition and a change here would declare every deployed pair
    // stale.
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

    mnstore::Header header;
    header.metaBinaryOrder = config.metaBinaryOrder;
    header.metaDepth = config.metaDepth;
    header.referenceFrame = referenceFrameId;
    header.sourceHash = sourceHash;
    if (config.geoidGrid) { header.geoidGrid = *config.geoidGrid; }
    header.heightFunction = heightFunctionJson(config.heightFunction);

    publishPair(result.tileIndex, result.pages, header
                , tileIndexPath, storePath);
}

void publishUnifiedIndex(const UnifiedResult &result
                         , const fs::path &tileIndexPath)
{
    // staged write: temp name, fsync, rename, fsync directory
    const auto tiTmp(utility::addExtension(tileIndexPath, ".tmp"));
    LOG(info3) << "Saving tile index into staged " << tiTmp << ".";
    result.tileIndex.save(tiTmp);

    fsyncPath(tiTmp);
    fs::rename(tiTmp, tileIndexPath);
    fsyncPath(tileIndexPath.parent_path());

    LOG(info3)
        << "Tile index " << tileIndexPath
        << " published (coverage, no metanode store).";
}

namespace {

/** Per-division-node context the re-flag needs: the SDS srs, the truescale
 *  measure (navtile restoration) and the prune floor grid.
 */
struct ReflagNode {
    vr::ReferenceFrame::Division::Node node;
    geo::SrsDefinition srsDef;
    std::shared_ptr<TrueScale> trueScale;
    std::shared_ptr<GsdGrid> pruneGrid;
};

/** SDS extents (and centre) of one tile. */
math::Extents2 tileExtents(const vr::ReferenceFrame::Division::Node &node
                           , const vts::TileId &tileId)
{
    return rangeExtents(node, tileId.lod
                        , vts::TileRange(tileId.x, tileId.y
                                         , tileId.x, tileId.y));
}

/** @return true if all children of the tile's parent can be pruned */
bool allChildrenCanBePruned(const ReflagNode &ctx
                            , const vts::TileId &tileId)
{
    const auto depth(int(tileId.lod - ctx.node.id.lod));
    for (const auto &sibling : vts::children(vts::parent(tileId))) {

        const auto center(math::center(tileExtents(ctx.node, sibling)));
        if (depth <= ctx.pruneGrid->floorDepth(center)) return false;
    }
    return true;
}

} // namespace

ReflagStats reflag(const fs::path &dataset
                   , const vr::ReferenceFrame &referenceFrame
                   , const fs::path &tileIndexPath
                   , const fs::path &storePath
                   , const ReflagConfig &config
                   , bool apply)
{
    const bool suppress(config.skipPartial && *config.skipPartial);
    const bool restore(config.skipPartial && !*config.skipPartial);
    const bool prune(bool(config.pruneGsd));

    mnstore::Store store(storePath);
    auto header(store.header());

    if (header.referenceFrame != referenceFrame.id) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store " << storePath << " belongs to reference "
            << "frame <" << header.referenceFrame << ">, not <"
            << referenceFrame.id << ">; refusing to reflag it.";
    }

    // the pair must come from one run: the store is only a trustworthy
    // witness of the index's tiles while their pairing digest agrees
    const auto pairing(mnstore::fileDigest(tileIndexPath));
    if (header.pairing != pairing) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store " << storePath << " is not paired with "
            << tileIndexPath << " (store pairing " << header.pairing
            << ", index " << pairing << "); reflag needs a matching pair.";
    }

    vts::TileIndex index;
    index.load(tileIndexPath);

    // per-division-node contexts (bisection nodes only); truescale opens
    // the DEM, so build it only when restoring navtile bits
    boost::optional<geo::GeoDataset> dem;
    if (restore) { dem = geo::GeoDataset::open(dataset); }

    std::vector<ReflagNode> nodes;
    for (const auto &item : referenceFrame.division.nodes) {
        const auto &node(item.second);
        if (node.partitioning.mode != vr::PartitioningMode::bisection) {
            continue;
        }
        ReflagNode ctx;
        ctx.node = node;
        ctx.srsDef = vr::system.srs(node.srs).srsDef;
        if (restore) {
            ctx.trueScale
                = std::make_shared<TrueScale>(*dem, ctx.srsDef);
        }
        if (prune) {
            ctx.pruneGrid = std::make_shared<GsdGrid>
                (ctx.srsDef, node.extents, *config.pruneGsd
                 , config.pruneExtraLods);
            if (!ctx.pruneGrid->anyValid()) { ctx.pruneGrid.reset(); }
        }
        nodes.push_back(std::move(ctx));
    }

    // the bisection node whose subtree contains a tile (integer ancestry,
    // no projection pipeline)
    const auto findNode([&](const vts::TileId &tileId) -> ReflagNode*
    {
        for (auto &ctx : nodes) {
            const auto &nid(ctx.node.id);
            if (tileId.lod < nid.lod) { continue; }
            const auto d(tileId.lod - nid.lod);
            if (((tileId.x >> d) == nid.x) && ((tileId.y >> d) == nid.y)) {
                return &ctx;
            }
        }
        return nullptr;
    });

    ReflagStats stats;
    mnstore::Page::list pages;
    for (const auto &root : store.pageIds()) {
        mnstore::Page page;
        if (!store.read(root, page)) { continue; }

        for (unsigned int level(0); level < header.metaDepth; ++level) {
            const auto size(header.levelSize(level));
            const vts::Lod lod(root.lod + level);
            for (unsigned int y(0); y < size; ++y) {
                for (unsigned int x(0); x < size; ++x) {
                    auto &data(page.node(level, x, y));
                    if (!data) { continue; }
                    ++stats.total;

                    const vts::TileId tileId
                        (lod, (root.x << level) + x, (root.y << level) + y);
                    auto *ctx(findNode(tileId));
                    if (!ctx) { continue; }

                    const auto extents(tileExtents(ctx->node, tileId));
                    const auto center(math::center(extents));
                    const auto depth(int(tileId.lod - ctx->node.id.lod));

                    if (ctx->pruneGrid && (depth > 0)
                        && allChildrenCanBePruned(*ctx, tileId))
                    {
                        // drop from both store and index
                        data = mnstore::NodeData();
                        index.set(tileId, TiFlag::value_type(0));
                        ++stats.pruned;
                        continue;
                    }

                    const bool partial
                        (data.coverage
                         == mnstore::NodeData::Coverage::partial);
                    if (!partial) { continue; }

                    if (suppress) {
                        index.set(tileId, TiFlag::value_type(0));
                        ++stats.suppressed;
                        continue;
                    }
                    if (restore) {
                        const auto ts(math::size(extents));
                        const math::Size2f samplePx
                            (ts.width / config.tileSampling
                             , ts.height / config.tileSampling);
                        TiFlag::value_type flags(TiFlag::mesh);
                        if ((*ctx->trueScale)(center, samplePx) < 1.0) {
                            flags |= TiFlag::navtile;
                        }
                        index.set(tileId, flags);
                        ++stats.restored;
                    }
                }
            }
        }
        pages.push_back(std::move(page));
    }

    if (apply) {
        publishPair(index, pages, header, tileIndexPath, storePath);
    }
    return stats;
}

} // namespace tiling
