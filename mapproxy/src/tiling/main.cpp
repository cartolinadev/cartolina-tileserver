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

/** mapproxy-tiling: surveys a source dataset against a reference frame and,
 *  on demand, tiles it into the paired flag tile index and metanode store.
 *
 *  The tool merges the former mapproxy-calipers measurement into the tiling
 *  pass: it measures the dataset itself (no hand-copied lod/tile ranges) and
 *  derives the floor LODs from a target GSD. The default run is a dry
 *  survey — it prints what it would produce and a resource-config template —
 *  and only writes artifacts when asked to (--apply).
 */

#include <cstdlib>
#include <fstream>
#include <iostream>

#include <boost/optional.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>

#include "utility/streams.hpp"
#include "utility/buildsys.hpp"
#include "service/cmdline.hpp"

#include "geo/geodataset.hpp"
#include "geo/gdal.hpp"

#include "gdal-drivers/register.hpp"

#include "vts-libs/registry/po.hpp"
#include "vts-libs/registry/io.hpp"
#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/io.hpp"

#include "jsoncpp/json.hpp"
#include "jsoncpp/io.hpp"

#include "calipers/calipers.hpp"

#include "mapproxy/resource.hpp"
#include "mapproxy/definition.hpp"
#include "mapproxy/support/srs.hpp"
#include "mapproxy/heightfunction.hpp"

#include "./unified.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace vts = vtslibs::vts;
namespace vr = vtslibs::registry;

namespace {

/** Opens a raster or vector dataset and returns its descriptor. Vector
 *  datasets carry no raster bands, so their resolution is taken from
 *  vectorResolution and the extents are measured from the layers.
 */
geo::GeoDataset::Descriptor probe(const fs::path &path
                                  , double vectorResolution)
{
    auto handle(::GDALOpenEx
                (path.c_str()
                 , GDAL_OF_RASTER | GDAL_OF_VECTOR | GDAL_OF_READONLY
                 , nullptr, nullptr, nullptr));

    if (!handle) {
        LOGTHROW(err2, std::runtime_error)
            << "Failed to open dataset " << path << ".";
    }

    std::unique_ptr< ::GDALDataset>
        dataset(static_cast< ::GDALDataset*>(handle));

    if (dataset->GetRasterCount()) {
        return geo::GeoDataset::use(std::move(dataset)).descriptor();
    }

    // valid GDAL dataset without any raster -> vector dataset
    geo::GeoDataset::Descriptor ds;
    ds.resolution(0) = ds.resolution(1) = vectorResolution;
    ds.driverName = dataset->GetDriverName();

    bool hasSrs(false);
    ds.extents = math::Extents2(math::InvalidExtents{});
    for (int i(0), e(dataset->GetLayerCount()); i < e; ++i) {
        auto layer(dataset->GetLayer(i));
        if (!layer->GetFeatureCount(TRUE)) { continue; }

        ::OGREnvelope envelope;
        if (layer->GetExtent(&envelope, TRUE) == OGRERR_NONE) {
            update(ds.extents, envelope.MinX, envelope.MinY);
            update(ds.extents, envelope.MaxX, envelope.MaxY);
        }
        if (!hasSrs) {
            if (const auto *ref = layer->GetSpatialRef()) {
                ds.srs = geo::SrsDefinition::fromReference(*ref);
                hasSrs = true;
            }
        }
    }

    if (!valid(ds.extents)) {
        LOGTHROW(err2, std::runtime_error)
            << "Failed to measure dataset " << path << ".";
    }

    auto size(math::size(ds.extents));
    ds.size.width = int(size.width / ds.resolution(0));
    ds.size.height = int(size.height / ds.resolution(0));

    return ds;
}

/** Builds a resource-config template from a measurement: a single-resource
 *  JSON the operator edits (attribution/credits, dataset path) and copies
 *  into a mapproxy resource definition directory. It is intentionally not
 *  a live config: it carries no credits and a loud comment.
 */
Resource buildTemplate(const calipers::Measurement &m
                       , const std::string &referenceFrame
                       , const std::string &id
                       , const fs::path &datasetPath
                       , const boost::optional<std::string> &geoidGrid)
{
    Resource r({});
    r.id = Resource::Id(referenceFrame, "TEMPLATE", id);
    r.comment = "TEMPLATE — add credits/attribution and set the dataset "
        "path before installing.";
    r.lodRange = m.lodRange;
    r.tileRange = m.tileRange;

    switch (m.datasetType) {
    case calipers::DatasetType::dem: {
        r.generator = Resource::Generator::from<resource::SurfaceDem>();
        auto def(std::make_shared<resource::SurfaceDem>());
        def->dem.dataset = datasetPath.string();
        if (geoidGrid) { def->dem.geoidGrid = *geoidGrid; }
        def->introspection.position = m.position;
        r.definition(def);
        break;
    }

    case calipers::DatasetType::ophoto: {
        r.generator = Resource::Generator::from<resource::TmsRaster>();
        auto def(std::make_shared<resource::TmsRaster>());
        def->dataset = datasetPath.string();
        r.definition(def);
        break;
    }

    default:
        LOGTHROW(err2, std::runtime_error)
            << "No resource template for dataset type <"
            << m.datasetType << "> (author the geodata config manually).";
    }

    return r;
}

class Tiling : public service::Cmdline {
public:
    Tiling()
        : service::Cmdline("mapproxy-tiling", BUILD_TARGET_VERSION
                           , (service::DISABLE_EXCESSIVE_LOGGING))
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd);

    void configure(const po::variables_map &vars);

    boost::optional<fs::path>
    operatingDirectory(const po::variables_map &vars) const;

    bool help(std::ostream &out, const std::string &what) const;

    int run();

    int runImpl();

    void report(const calipers::Measurement &m
                , const vr::ReferenceFrame &rf, std::ostream &out) const;

    void writeTemplate(const calipers::Measurement &m) const;

    int apply(const vr::ReferenceFrame &rf, calipers::Measurement m);

    fs::path input_;
    fs::path dataset_;
    std::string referenceFrame_;

    calipers::Config calipersConfig_;
    double vectorResolution_ = 1.0;
    boost::optional<vts::Lod> bottomLod_;

    bool apply_ = false;
    bool prune_ = true;
    bool skipPartial_ = false;

    tiling::UnifiedConfig unifiedConfig_;
    boost::optional<unsigned int> metaBinaryOrder_;

    fs::path output_;
    fs::path storeOutput_;
    boost::optional<fs::path> resourceConfig_;

    bool noexcept_ = false;
};

void Tiling::configuration(po::options_description &cmdline
                           , po::options_description &config
                           , po::positional_options_description &pd)
{
    vr::registryConfiguration(cmdline, vr::defaultPath());

    cmdline.add_options()
        ("input", po::value(&input_)->required()
         , "Path to the dataset (or a dataset dir containing dem/ophoto).")
        ("referenceFrame", po::value(&referenceFrame_)->required()
         , "Reference frame to tile in.")

        ("gsd", po::value<double>()
         , "Target floor GSD (ground sampling distance, meters per pixel) "
         "the finest LOD resolves. When omitted, the native GSD measured "
         "from the dataset is used (scaled finer for a DEM by "
         "--demToOphotoScale).")
        ("datasetType", po::value<calipers::DatasetType>()
         , "Override dataset type autodetection (dem or ophoto).")
        ("demToOphotoScale", po::value(&calipersConfig_.demToOphotoScale)
         ->default_value(calipersConfig_.demToOphotoScale)
         , "DEM-only default floor scale used when --gsd is not given: the "
         "inverse resolution ratio of the finest ophoto/mesh draped on the "
         "DEM (3 = tile three times finer than the DEM sample spacing).")
        ("tileFractionLimit", po::value(&calipersConfig_.tileFractionLimit)
         ->default_value(calipersConfig_.tileFractionLimit)
         , "Dataset-border tracing granularity (expert): inverse tile "
         "fraction at which border refinement stops.")
        ("vectorResolution", po::value(&vectorResolution_)
         ->default_value(vectorResolution_)
         , "Assumed resolution (meters per pixel) of a vector dataset.")
        ("bottomLod", po::value<vts::Lod>()
         , "Floor LOD override: widen the analyzed lod range to at least "
         "this LOD (the prune floor is extended by the same amount).")

        ("apply", "Actually run the tiling and publish the artifacts. "
         "Without it the tool performs a dry survey only (measurement "
         "report and resource-config template).")
        ("prune", po::value(&prune_)->default_value(prune_)
         ->implicit_value(true)
         , "Spatially-varying bottom LOD: drop tiles finer than the target "
         "GSD resolves at their own location (metanode/DEM path only). "
         "'--prune false' tiles the full lod range everywhere.")
        ("skipPartial", po::value(&skipPartial_)->default_value(skipPartial_)
         ->implicit_value(true)
         , "Discard non-watertight (partial) tile meshes, removing their "
         "boundary cracks and framebuffer switches at the cost of their "
         "valid partial content. Useful when a global base surface can "
         "replace the whole tile. Metanode/DEM path only; mutually "
         "exclusive with --forceWatertight.")
        ("forceWatertight", po::value(&unifiedConfig_.forceWatertight)
         ->default_value(unifiedConfig_.forceWatertight)->implicit_value(true)
         , "Treat all partial tiles as watertight (lies about holes).")

        ("output", po::value<fs::path>(&output_)
         , "Flag tile index output (default input/tiling.referenceFrame).")
        ("store", po::value<fs::path>(&storeOutput_)
         , "Metanode store output (default "
         "input/metanodes.referenceFrame).")
        ("resourceConfig", po::value<fs::path>()
         , "Resource-config template destination; '-' means stdout. "
         "Default: stdout in a dry run, input/resource.referenceFrame.json "
         "with --apply.")

        ("tileSampling", po::value(&unifiedConfig_.tileSampling)
         ->default_value(unifiedConfig_.tileSampling)
         , "Samples per tile edge for the navtile-eligibility measure.")
        ("metaBinaryOrder", po::value<unsigned int>()
         , "Metatile packaging horizontal binary order override "
         "(defaults to the reference frame value).")
        ("metaDepth", po::value(&unifiedConfig_.metaDepth)
         ->default_value(unifiedConfig_.metaDepth)
         , "Metatile packaging: LOD levels per delivery unit.")
        ("warpConcurrency", po::value(&unifiedConfig_.warpConcurrency)
         ->default_value(unifiedConfig_.warpConcurrency)
         , "Maximum concurrent filter-pass warps (source-read bound).")
        ("geoidGrid", po::value<std::string>()
         , "Geoid grid of the SDS vertical datum (resource geoidGrid "
         "setting). Must be PROJ-readable; validated at startup. Omit for "
         "the reference frame body default; pass an empty string for an "
         "already-ellipsoidal source.")
        ("heightFunction", po::value<fs::path>()
         , "Path to a JSON {\"heightFunction\": ...} baked into stored "
         "heights.")

        ("noexcept", "Do not catch exceptions, let the program crash.")
        ;

    pd.add("input", 1)
        .add("referenceFrame", 1)
        ;

    (void) config;
}

boost::optional<fs::path>
Tiling::operatingDirectory(const po::variables_map &vars) const
{
    // Without --apply this is a dry survey: a diagnostic report only, not
    // a data-writing run, so it must not leave a log trace.
    if (!vars.count("apply")) { return boost::none; }

    // mapproxy-tiling writes data (the flag tile index and metanode store)
    // into the dataset directory named by the "input" argument; that's
    // where the log/ subdirectory and startup banner belong. "input" may
    // also name a single raw dataset file (--output/--store then
    // required); use its parent directory in that case.
    if (!vars.count("input")) { return boost::none; }
    const auto dir(fs::absolute(vars["input"].as<fs::path>()));
    return fs::is_directory(dir) ? dir : dir.parent_path();
}

void Tiling::configure(const po::variables_map &vars)
{
    vr::registryConfigure(vars);

    bool complexDataset(true);
    if (fs::exists(input_ / "dem")) {
        dataset_ = input_ / "dem";
    } else if (fs::exists(input_ / "ophoto")) {
        dataset_ = input_ / "ophoto";
    } else {
        dataset_ = input_;
        complexDataset = false;
    }

    if (vars.count("datasetType")) {
        calipersConfig_.datasetType
            = vars["datasetType"].as<calipers::DatasetType>();
    }
    if (vars.count("gsd")) {
        const auto gsd(vars["gsd"].as<double>());
        if (gsd <= 0.0) {
            throw po::error("--gsd must be a positive value in meters.");
        }
        calipersConfig_.gsd = gsd;
    }
    if (vars.count("bottomLod")) {
        bottomLod_ = vars["bottomLod"].as<vts::Lod>();
    }

    apply_ = vars.count("apply");
    noexcept_ = vars.count("noexcept");

    if (skipPartial_ && unifiedConfig_.forceWatertight) {
        throw po::error("--skipPartial and --forceWatertight are mutually "
                        "exclusive.");
    }

    if (vars.count("metaBinaryOrder")) {
        metaBinaryOrder_ = vars["metaBinaryOrder"].as<unsigned int>();
    }
    if (vars.count("geoidGrid")) {
        unifiedConfig_.geoidGrid = vars["geoidGrid"].as<std::string>();
    }
    if (vars.count("heightFunction")) {
        const auto hfPath(vars["heightFunction"].as<fs::path>());
        std::ifstream file(hfPath.string());
        const auto value(Json::read(file, hfPath));
        unifiedConfig_.heightFunction
            = HeightFunction::parse(value, "heightFunction");
    }

    // --apply writes artifacts, so it needs resolved output/store paths
    const bool needPaths(apply_);
    if (vars.count("output")) {
        output_ = vars["output"].as<fs::path>();
    } else if (complexDataset) {
        output_ = input_ / ("tiling." + referenceFrame_);
    } else if (needPaths) {
        throw po::required_option("output");
    }

    if (vars.count("store")) {
        storeOutput_ = vars["store"].as<fs::path>();
    } else if (complexDataset) {
        storeOutput_ = input_ / ("metanodes." + referenceFrame_);
    } else if (needPaths) {
        throw po::required_option("store");
    }

    if (vars.count("resourceConfig")) {
        resourceConfig_ = vars["resourceConfig"].as<fs::path>();
    }
}

bool Tiling::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << ("mapproxy-tiling input referenceFrame [ options ]\n"
                "    Surveys a dataset against a reference frame and, with\n"
                "    --apply, tiles it into the paired flag tile index and\n"
                "    metanode store; also emits a resource-config template.\n"
                "\n"
                "    Default run is a dry survey: it measures the dataset\n"
                "    (native and target GSD, per-node lod/tile ranges,\n"
                "    suggested position) and prints the report and the\n"
                "    resource-config template, without writing artifacts.\n"
                "    Add --apply to run the tiling and publish.\n"
                "\n"
                "    --gsd sets the target floor resolution; the floor LODs\n"
                "    are derived from it. --prune (default on) drops tiles\n"
                "    that would resolve finer than the source at their own\n"
                "    location. --skipPartial sacrifices partial tile content\n"
                "    to remove its boundary cracks and framebuffer switches.\n"
                "\n"
                );
        return true;
    }
    return false;
}

void Tiling::report(const calipers::Measurement &m
                    , const vr::ReferenceFrame&, std::ostream &out) const
{
    out << "Dataset: " << dataset_ << " (" << m.datasetType << ")\n"
        << "Reference frame: " << referenceFrame_ << "\n"
        << "Native GSD: " << m.gsd << " m/px\n"
        << "Target floor GSD: " << m.targetGsd << " m/px"
        << (calipersConfig_.gsd ? " (--gsd)" : "") << "\n";
    if (m.xOverlap) {
        out << "Horizontal wrap: " << *m.xOverlap << " px\n";
    }

    out << "Spatial division nodes:\n";
    for (const auto &node : m.nodes) {
        const auto lr(node.ranges.lodRange());
        out << "  " << node.srs << ": lodRange " << lr
            << " tileRange@" << lr.max << " " << node.ranges.tileRange(lr.max)
            << "\n";
    }
    out << "Overall: lodRange " << m.lodRange
        << " tileRange " << m.tileRange << "\n";
    out << std::fixed << "Suggested position: " << m.position << "\n";

    const bool dem(m.datasetType == calipers::DatasetType::dem);
    out << "Prune: " << ((dem && prune_)
                         ? str(boost::format("on (floor GSD %g m)")
                               % m.targetGsd)
                         : std::string("off"))
        << " | skipPartial: " << ((dem && skipPartial_) ? "on" : "off")
        << " | forceWatertight: "
        << (unifiedConfig_.forceWatertight ? "on" : "off") << "\n";

    if (apply_) {
        out << "Artifacts:\n  tile index: " << output_ << "\n";
        if (dem) { out << "  metanode store: " << storeOutput_ << "\n"; }
    }
}

void Tiling::writeTemplate(const calipers::Measurement &m) const
{
    if (m.datasetType == calipers::DatasetType::vector) {
        LOG(info3)
            << "Vector dataset: no resource template emitted (author the "
            "geodata resource config manually from the ranges above).";
        return;
    }

    const auto r(buildTemplate(m, referenceFrame_, input_.filename().string()
                               , dataset_, unifiedConfig_.geoidGrid));

    // destination: explicit --resourceConfig ('-' = stdout) wins; else
    // stdout in a dry run, a file beside the artifacts with --apply
    const bool toStdout
        (resourceConfig_
         ? (resourceConfig_->string() == "-")
         : !apply_);

    if (toStdout) {
        std::cout << "\n--- resource configuration template ---\n";
        save(std::cout, r);
        std::cout << std::flush;
        return;
    }

    const auto path
        (resourceConfig_
         ? *resourceConfig_
         : input_ / ("resource." + referenceFrame_ + ".json"));
    save(path, r);
    LOG(info4)
        << "Resource configuration template written to " << path
        << "; edit its credits/attribution and dataset path and copy it "
        "into your mapproxy resource definition directory.";
}

int Tiling::apply(const vr::ReferenceFrame &rf, calipers::Measurement m)
{
    if (m.datasetType == calipers::DatasetType::vector) {
        LOG(err3, log_)
            << "Cannot tile a vector dataset; run without --apply to "
            "measure it for a geodata resource config.";
        return EXIT_FAILURE;
    }

    unifiedConfig_.metaBinaryOrder
        = (metaBinaryOrder_ ? *metaBinaryOrder_ : rf.metaBinaryOrder);

    // any raster that is not a DEM is imagery: coverage (mask-only) pass,
    // flag index alone, no metanode store
    unifiedConfig_.coverage = (m.datasetType != calipers::DatasetType::dem);

    if (unifiedConfig_.coverage && skipPartial_) {
        LOG(err3, log_)
            << "--skipPartial applies to the DEM (metanode) path only.";
        return EXIT_FAILURE;
    }

    if (unifiedConfig_.coverage) {
        unifiedConfig_.pruneGsd = boost::none;
    } else if (prune_) {
        unifiedConfig_.pruneGsd = m.targetGsd;
        // an operator floor deeper than the measured one must not be
        // pruned away: give the prune that many extra local LODs
        vts::Lod measuredMaxLod(0);
        for (const auto &node : m.nodes) {
            measuredMaxLod
                = std::max(measuredMaxLod, node.ranges.lodRange().max);
        }
        if (m.lodRange.max > measuredMaxLod) {
            unifiedConfig_.pruneExtraLods
                = m.lodRange.max - measuredMaxLod;
        }
    }
    unifiedConfig_.skipPartial = skipPartial_;

    const auto tileRanges(m.lodTileRanges());
    auto result(tiling::generateUnified
                (dataset_, rf, m.lodRange, tileRanges, unifiedConfig_));

    if (result.tileIndex.empty()) {
        LOG(err3, log_) << "Tiling emitted no tiles.";
        return EXIT_FAILURE;
    }

    if (unifiedConfig_.pruneGsd
        && (result.tileIndex.maxLod() < m.lodRange.max))
    {
        LOG(info3, log_)
            << "Spatial prune reduced the final maximum LOD from "
            << m.lodRange.max << " to " << result.tileIndex.maxLod()
            << ".";
        m.lodRange.max = result.tileIndex.maxLod();
    }

    if (unifiedConfig_.coverage) {
        tiling::publishUnifiedIndex(result, output_);
    } else {
        tiling::publishUnified(result, unifiedConfig_, referenceFrame_
                               , m.lodRange, tileRanges, output_, storeOutput_);
    }

    writeTemplate(m);
    LOG(info4) << "Done.";
    return EXIT_SUCCESS;
}

int Tiling::runImpl()
{
    auto rf(vr::system.referenceFrames(referenceFrame_));

    LOG(info4) << "Processing dataset " << dataset_ << ".";

    // Resolve the geoid grid: an explicit value wins, an explicit empty
    // string means "no geoid", an omitted grid falls back to the
    // reference frame body default. Fail fast on a grid PROJ cannot read.
    if (!unifiedConfig_.geoidGrid) {
        if (rf.body) {
            unifiedConfig_.geoidGrid
                = vr::system.bodies(*rf.body).defaultGeoidGrid;
        }
    } else if (unifiedConfig_.geoidGrid->empty()) {
        unifiedConfig_.geoidGrid = boost::none;
    }
    validateGeoidGrid(unifiedConfig_.geoidGrid);
    LOG(info3)
        << "Geoid grid: "
        << (unifiedConfig_.geoidGrid
            ? *unifiedConfig_.geoidGrid : std::string("none")) << ".";

    const auto ds(probe(dataset_, vectorResolution_));
    auto m(calipers::measure(rf, ds, calipersConfig_));

    if (m.nodes.empty()) {
        LOG(err3, log_)
            << "Dataset does not fall inside any node of reference frame <"
            << referenceFrame_ << ">; nothing to tile.";
        return EXIT_FAILURE;
    }

    if (bottomLod_) {
        m.lodRange.max = std::max(m.lodRange.max, *bottomLod_);
    }

    report(m, rf, std::cout);

    if (!apply_) {
        writeTemplate(m);
        return EXIT_SUCCESS;
    }

    return apply(rf, m);
}

int Tiling::run()
{
    if (noexcept_) { return runImpl(); }

    try {
        return runImpl();
    } catch (const std::exception &e) {
        std::cerr << "mapproxy-tiling: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    gdal_drivers::registerAll();
    ::OGRRegisterAll();
    // force VRT not to share underlying datasets
    geo::Gdal::setOption("VRT_SHARED_SOURCE", 0);
    return Tiling()(argc, argv);
}
