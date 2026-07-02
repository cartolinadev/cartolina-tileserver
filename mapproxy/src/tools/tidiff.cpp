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

/** Diffs two vts tile indexes per tile. The default comparison covers the
 *  union of all nonzero tiles; optional LOD and tile ranges restrict it.
 */

#include <iostream>
#include <map>

#include <boost/optional.hpp>

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

    void configure(const po::variables_map&) override;

    bool help(std::ostream &out, const std::string &what) const override;

    int run() override;

    fs::path a_;
    fs::path b_;
    boost::optional<vts::LodRange> lodRange_;
    boost::optional<vts::TileRange> tileRange_;
    boost::optional<vts::Lod> tileRangeLod_;
    unsigned int examples_ = 10;
};

void TiDiff::configuration(po::options_description &cmdline
                           , po::options_description &config
                           , po::positional_options_description &pd)
{
    cmdline.add_options()
        ("a", po::value(&a_)->required(), "First tile index.")
        ("b", po::value(&b_)->required(), "Second tile index.")
        ("lodRange", po::value<vts::LodRange>()
         , "Restrict comparison to this LOD range.")
        ("tileRange", po::value<vts::TileRange>()
         , "Restrict comparison to this tile range (at tileRangeLod).")
        ("tileRangeLod", po::value<vts::Lod>()
         , "LOD the tile range is defined at (default: comparison max).")
        ("examples", po::value(&examples_)->default_value(examples_)
         , "Number of difference examples to print per lod.")
        ;

    pd.add("a", 1).add("b", 1);

    (void) config;
}

void TiDiff::configure(const po::variables_map &vars)
{
    if (vars.count("lodRange")) {
        lodRange_ = vars["lodRange"].as<vts::LodRange>();
    }
    if (vars.count("tileRange")) {
        tileRange_ = vars["tileRange"].as<vts::TileRange>();
    }
    if (vars.count("tileRangeLod")) {
        tileRangeLod_ = vars["tileRangeLod"].as<vts::Lod>();
    }
    if (tileRangeLod_ && !tileRange_) {
        throw po::error("--tileRangeLod requires --tileRange.");
    }
}

bool TiDiff::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << ("vts tile index diff\n"
                "\n"
                "    mapproxy-tidiff a b [--lodRange l1,l2]\n"
                "        [--tileRange xmin,ymin:xmax,ymax "
                "--tileRangeLod lod]\n"
                "\n"
                "    By default, compares the union of every nonzero tile in\n"
                "    both indexes. Optional ranges restrict the comparison.\n"
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

    if (!lodRange_) {
        if (a.empty() && b.empty()) {
            std::cout << "total: 0 / 0 tiles differ\n";
            return EXIT_SUCCESS;
        }
        if (a.empty()) {
            lodRange_ = b.lodRange();
        } else if (b.empty()) {
            lodRange_ = a.lodRange();
        } else {
            lodRange_ = unite(a.lodRange(), b.lodRange());
        }
    }

    if (tileRange_ && !tileRangeLod_) {
        tileRangeLod_ = lodRange_->max;
    }

    std::size_t totalCells(0), totalDiff(0);

    for (auto lod(lodRange_->min); lod <= lodRange_->max; ++lod) {
        std::size_t cells(0), diff(0);
        std::map<std::pair<int, int>, std::size_t> transitions;
        unsigned int shown(0);

        const auto compare([&](const vts::TileId &tileId
                               , vts::QTree::value_type from
                               , vts::QTree::value_type to)
        {
            ++cells;
            if (from == to) { return; }
            ++diff;
            ++transitions[{ int(from), int(to) }];
            if (shown < examples_) {
                std::cout << "    " << tileId << ": "
                          << vts::TileFlags(from) << " -> "
                          << vts::TileFlags(to) << '\n';
                ++shown;
            }
        });

        if (tileRange_) {
            const auto range
                (vts::shiftRange(*tileRangeLod_, *tileRange_, lod));
            for (auto y(range.ll(1)); y <= range.ur(1); ++y) {
                for (auto x(range.ll(0)); x <= range.ur(0); ++x) {
                    const vts::TileId tileId(lod, x, y);
                    compare(tileId, a.get(tileId), b.get(tileId));
                }
            }
        } else {
            /* Combine the indexes into compressed (from, to) blocks. This
             * keeps identical global coverage compact while retaining exact
             * per-tile counts; only printed examples are rasterized.
             */
            vts::QTree paired(lod);
            if (const auto *tree = a.ctree(lod)) { paired = *tree; }
            const vts::QTree empty(lod);
            const auto *other(b.ctree(lod));
            paired.combine(other ? *other : empty
                           , [](vts::QTree::value_type from
                                , vts::QTree::value_type to)
            {
                return ((from & 0xff) | ((to & 0xff) << 8));
            });

            paired.forEachNode
                ([&](unsigned int x, unsigned int y, unsigned int size
                      , vts::QTree::value_type value)
            {
                const auto from(value & 0xff);
                const auto to((value >> 8) & 0xff);
                if (!from && !to) { return; }

                const auto area(std::size_t(size) * std::size_t(size));
                cells += area;
                if (from == to) { return; }

                diff += area;
                transitions[{ int(from), int(to) }] += area;
                for (unsigned int row(y);
                     (row < y + size) && (shown < examples_); ++row)
                {
                    for (unsigned int col(x);
                         (col < x + size) && (shown < examples_); ++col)
                    {
                        const vts::TileId tileId(lod, col, row);
                        std::cout << "    " << tileId << ": "
                                  << vts::TileFlags(from) << " -> "
                                  << vts::TileFlags(to) << '\n';
                        ++shown;
                    }
                }
            });
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
