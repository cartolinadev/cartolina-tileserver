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

/** RFC 7 phase-1 calibration spike (rfc-metanode-store.md, section 5.2).
 *
 * Reads v6 metatile files and emits one CSV row per geometry node:
 * the warp-derived ("true") texelSize, the stored SDS height range, and
 * the analytic planar texelSize a metanode-store delivery path would
 * derive without a warp. Child metatile ids are emitted as separate
 * rows so a harvesting script can crawl the metatile tree.
 */

#include <iostream>

#include <boost/optional.hpp>

#include "utility/buildsys.hpp"
#include "service/cmdline.hpp"

#include "vts-libs/registry/po.hpp"
#include "vts-libs/vts/metatile.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "vts-libs/vts/tileop.hpp"

#include "mapproxy/support/srs.hpp"
#include "mapproxy/support/mesh.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace vr = vtslibs::registry;
namespace vts = vtslibs::vts;

namespace {

/** Samples-per-tile grid used by the serve-time warp; the planar
 *  analytic area is computed over the same grid so flat terrain
 *  reproduces the warped value exactly.
 */
const int samplesPerTile(8);

class TexelSpike : public service::Cmdline {
public:
    TexelSpike()
        : service::Cmdline("mapproxy-texel-spike", BUILD_TARGET_VERSION)
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd) override;

    void configure(const po::variables_map &vars) override;

    bool help(std::ostream &out, const std::string &what) const override;

    int run() override;

    void processMetatile(const fs::path &path);

    std::vector<fs::path> metatiles_;
    std::string referenceFrameId_;
    boost::optional<std::string> geoidGrid_;

    const vr::ReferenceFrame *referenceFrame_ = nullptr;
};

void TexelSpike::configuration(po::options_description &cmdline
                               , po::options_description &config
                               , po::positional_options_description &pd)
{
    vr::registryConfiguration(cmdline, vr::defaultPath());

    cmdline.add_options()
        ("referenceFrame", po::value(&referenceFrameId_)->required()
         , "Reference frame the metatiles belong to.")
        ("geoidGrid", po::value<std::string>()
         , "Geoid grid of the SDS vertical datum (resource setting).")
        ("metatile", po::value(&metatiles_)->composing()->required()
         , "Path to a metatile file; can be used multiple times.")
        ;

    pd.add("metatile", -1);

    (void) config;
}

void TexelSpike::configure(const po::variables_map &vars)
{
    vr::registryConfigure(vars);
    referenceFrame_ = &vr::system.referenceFrames(referenceFrameId_);

    if (vars.count("geoidGrid")) {
        geoidGrid_ = vars["geoidGrid"].as<std::string>();
    }
}

bool TexelSpike::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << ("RFC 7 texelSize calibration spike\n"
                "\n"
                "    Emits CSV on stdout, one row per metanode:\n"
                "    N,lod,x,y,srs,watertight,texel,minZ,maxZ,planar\n"
                "    and one row per child metatile:\n"
                "    C,lod,x,y\n"
                "\n");
        return true;
    }

    return false;
}

/** Computes the physical-space area of the node's full SDS cell, walked
 *  on the same grid the serve-time warp samples, at zero SDS height.
 *
 * @param nodeInfo node whose SDS extents are measured
 * @param geoidGrid SDS vertical datum geoid grid, if any
 * @return flat-terrain physical area of the node cell
 */
double planarCellArea(const vts::NodeInfo &nodeInfo
                      , const boost::optional<std::string> &geoidGrid)
{
    const auto conv(sds2phys(nodeInfo, geoidGrid));
    const auto extents(nodeInfo.extents());
    const auto cellSize(math::size(extents));

    const math::Size2f step(cellSize.width / samplesPerTile
                            , cellSize.height / samplesPerTile);

    // physical-space grid of (samplesPerTile + 1)^2 corner points
    std::vector<math::Point3> grid;
    grid.reserve((samplesPerTile + 1) * (samplesPerTile + 1));
    for (int row(0); row <= samplesPerTile; ++row) {
        const double y(extents.ur(1) - row * step.height);
        for (int col(0); col <= samplesPerTile; ++col) {
            const double x(extents.ll(0) + col * step.width);
            grid.push_back(conv(math::Point3(x, y, 0.0)));
        }
    }

    const auto point([&](int row, int col) -> const math::Point3*
    {
        return &grid[row * (samplesPerTile + 1) + col];
    });

    double area(0.0);
    for (int row(1); row <= samplesPerTile; ++row) {
        for (int col(1); col <= samplesPerTile; ++col) {
            const auto qa(quadArea(point(row - 1, col - 1)
                                   , point(row, col)
                                   , point(row - 1, col)
                                   , point(row, col - 1)));
            area += std::get<0>(qa);
        }
    }

    return area;
}

void TexelSpike::processMetatile(const fs::path &path)
{
    const auto binaryOrder(referenceFrame_->metaBinaryOrder);
    const auto meta(vts::loadMetaTile(path, binaryOrder));

    const vts::TileId::index_type metaMask
        ((vts::TileId::index_type(1) << binaryOrder) - 1);

    std::set<vts::TileId> childMetaIds;

    meta.for_each([&](const vts::TileId &tileId, const vts::MetaNode &node)
    {
        if (!node.flags()) { return; }

        for (const auto &childId : vts::children(node, tileId)) {
            childMetaIds.emplace(childId.lod, childId.x & ~metaMask
                                 , childId.y & ~metaMask);
        }

        if (!node.geometry() || !node.applyTexelSize()) { return; }

        const vts::NodeInfo nodeInfo(*referenceFrame_, tileId);
        if (!nodeInfo.valid() || !nodeInfo.productive()) { return; }

        const auto area(planarCellArea(nodeInfo, geoidGrid_));
        const double textureArea(vr::BoundLayer::tileArea());
        const double planar(std::sqrt(area / textureArea));

        const auto &ge(node.geomExtents);
        std::cout << "N," << tileId.lod
                  << ',' << tileId.x
                  << ',' << tileId.y
                  << ',' << nodeInfo.srs()
                  << ',' << (node.flags() & vts::MetaNode::Flag::watertight
                             ? 1 : 0)
                  << ',' << node.texelSize
                  << ',' << ge.z.min
                  << ',' << ge.z.max
                  << ',' << planar
                  << '\n';
    });

    for (const auto &metaId : childMetaIds) {
        std::cout << "C," << metaId.lod
                  << ',' << metaId.x
                  << ',' << metaId.y
                  << '\n';
    }
}

int TexelSpike::run()
{
    std::cout << std::setprecision(9);

    for (const auto &path : metatiles_) {
        processMetatile(path);
    }

    std::cout.flush();
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char *argv[])
{
    return TexelSpike()(argc, argv);
}
