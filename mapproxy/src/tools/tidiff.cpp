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

/** Diffs two vts tile indexes per tile over a lod/tile range; RFC 7
 *  phase-3 parity gate helper.
 */

#include <iostream>
#include <map>

#include "utility/buildsys.hpp"
#include "service/cmdline.hpp"

#include "vts-libs/vts/tileindex.hpp"
#include "vts-libs/vts/tileflags.hpp"
#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/io.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace vts = vtslibs::vts;

namespace {

class TiDiff : public service::Cmdline {
public:
    TiDiff()
        : service::Cmdline("mapproxy-tidiff", BUILD_TARGET_VERSION)
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd) override;

    void configure(const po::variables_map&) override {}

    bool help(std::ostream &out, const std::string &what) const override;

    int run() override;

    fs::path a_;
    fs::path b_;
    vts::LodRange lodRange_;
    vts::TileRange tileRange_;
    vts::Lod tileRangeLod_ = 0;
    unsigned int examples_ = 10;
};

void TiDiff::configuration(po::options_description &cmdline
                           , po::options_description &config
                           , po::positional_options_description &pd)
{
    cmdline.add_options()
        ("a", po::value(&a_)->required(), "First tile index.")
        ("b", po::value(&b_)->required(), "Second tile index.")
        ("lodRange", po::value(&lodRange_)->required()
         , "Lod range to compare.")
        ("tileRange", po::value(&tileRange_)->required()
         , "Tile range to compare (at tileRangeLod).")
        ("tileRangeLod", po::value(&tileRangeLod_)
         , "Lod the tile range is defined at (default: lodRange max).")
        ("examples", po::value(&examples_)->default_value(examples_)
         , "Number of difference examples to print per lod.")
        ;

    pd.add("a", 1).add("b", 1);

    (void) config;
}

bool TiDiff::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << ("vts tile index diff\n"
                "\n"
                "    mapproxy-tidiff a b --lodRange l1,l2 "
                "--tileRange xmin,ymin:xmax,ymax [--tileRangeLod lod]\n"
                "\n");
        return true;
    }

    return false;
}

int TiDiff::run()
{
    vts::TileIndex a, b;
    a.load(a_);
    b.load(b_);

    if (!tileRangeLod_) { tileRangeLod_ = lodRange_.max; }

    std::size_t totalCells(0), totalDiff(0);

    for (auto lod(lodRange_.min); lod <= lodRange_.max; ++lod) {
        const auto range
            (vts::shiftRange(tileRangeLod_, tileRange_, lod));

        std::size_t cells(0), diff(0);
        std::map<std::pair<int, int>, std::size_t> transitions;
        unsigned int shown(0);

        for (auto y(range.ll(1)); y <= range.ur(1); ++y) {
            for (auto x(range.ll(0)); x <= range.ur(0); ++x) {
                const vts::TileId tileId(lod, x, y);
                const auto fa(a.get(tileId));
                const auto fb(b.get(tileId));
                ++cells;
                if (fa == fb) { continue; }
                ++diff;
                ++transitions[{ int(fa), int(fb) }];
                if (shown < examples_) {
                    std::cout << "    " << tileId << ": "
                              << vts::TileFlags(fa) << " -> "
                              << vts::TileFlags(fb) << '\n';
                    ++shown;
                }
            }
        }

        totalCells += cells;
        totalDiff += diff;

        std::cout << "lod " << std::setw(2) << unsigned(lod) << ": "
                  << diff << " / " << cells << " tiles differ";
        if (diff) {
            std::cout << " [";
            bool first(true);
            for (const auto &item : transitions) {
                if (!first) { std::cout << ", "; }
                first = false;
                std::cout << vts::TileFlags(item.first.first) << "->"
                          << vts::TileFlags(item.first.second)
                          << ": " << item.second;
            }
            std::cout << "]";
        }
        std::cout << '\n';
    }

    std::cout << "total: " << totalDiff << " / " << totalCells
              << " tiles differ\n";

    return (totalDiff ? EXIT_FAILURE : EXIT_SUCCESS);
}

} // namespace

int main(int argc, char *argv[])
{
    return TiDiff()(argc, argv);
}
