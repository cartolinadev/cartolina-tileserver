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

/** Metanode store (RFC 7) inspection and self-test tool.
 */

#include <iostream>
#include <map>
#include <random>
#include <vector>

#include <boost/optional.hpp>

#include "utility/buildsys.hpp"
#include "service/cmdline.hpp"

#include "vts-libs/registry/po.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/tileop.hpp"
#include "vts-libs/vts/tileindex.hpp"
#include "vts-libs/vts/nodeinfo.hpp"
#include "mapproxy/support/mnstore.hpp"
#include "mapproxy/support/metatile.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace vr = vtslibs::registry;

namespace {

class MnStoreTool : public service::Cmdline {
public:
    MnStoreTool()
        : service::Cmdline("mapproxy-mnstore", BUILD_TARGET_VERSION)
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd) override;

    void configure(const po::variables_map &vars) override;

    bool help(std::ostream &out, const std::string &what) const override;

    int run() override;

    int info();
    int dump();

    /** Verifies the store against its paired flag tile index: pairing
     *  digest, payload ↔ index-reachability agreement, range
     *  aggregation, and the prune sibling rule.
     * @return EXIT_SUCCESS when no violation is found
     */
    int check();

    int selftest();

    std::string command_;
    fs::path store_;
    fs::path tileIndex_;
    boost::optional<vts::TileId> pageId_;
};

void MnStoreTool::configuration(po::options_description &cmdline
                                , po::options_description &config
                                , po::positional_options_description &pd)
{
    cmdline.add_options()
        ("command", po::value(&command_)->required()
         , "Command: info, dump, check, selftest.")
        ("store", po::value(&store_)
         , "Path to metanode store file.")
        ("tileIndex", po::value(&tileIndex_)
         , "Path to the paired flag tile index (check; defaults to "
         "tiling.<rf> beside a metanodes.<rf> store).")
        ("page", po::value<vts::TileId>()
         , "Limit dump to a single page (page root tile id).")
        ;

    vr::registryConfiguration(cmdline, vr::defaultPath());

    pd.add("command", 1)
        .add("store", 1)
        .add("tileIndex", 1);

    (void) config;
}

void MnStoreTool::configure(const po::variables_map &vars)
{
    vr::registryConfigure(vars);

    if (vars.count("page")) {
        pageId_ = vars["page"].as<vts::TileId>();
    }
}

bool MnStoreTool::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << ("metanode store inspection tool\n"
                "\n"
                "    mapproxy-mnstore info <store>\n"
                "    mapproxy-mnstore dump <store> [--page lod-x-y]\n"
                "    mapproxy-mnstore check <store> [<tileIndex>]\n"
                "    mapproxy-mnstore selftest\n"
                "\n"
                "check verifies the store against its paired flag tile\n"
                "index: pairing digest, payload present exactly for the\n"
                "tiles the index reaches, parent ranges containing child\n"
                "ranges, and pruned sibling sets removed together (children\n"
                "outside the reference frame's constrained area are exempt).\n"
                "It does not support forceWatertight stores.\n"
                "\n");
        return true;
    }

    return false;
}

void dumpPage(const mnstore::Header &header, const mnstore::Page &page)
{
    std::cout << "page " << page.root() << '\n';
    for (unsigned int level(0); level < header.metaDepth; ++level) {
        const auto size(header.levelSize(level));
        const vts::Lod lod(page.root().lod + level);
        for (unsigned int y(0); y < size; ++y) {
            for (unsigned int x(0); x < size; ++x) {
                const auto &node(page.node(level, x, y));
                if (!node) { continue; }
                std::cout
                    << "    " << vts::TileId
                    (lod, (page.root().x << level) + x
                     , (page.root().y << level) + y)
                    << " minZ=" << node.min()
                    << " maxZ=" << node.max()
                    << '\n';
            }
        }
    }
}

int MnStoreTool::info()
{
    mnstore::Store store(store_);
    const auto &header(store.header());

    std::cout
        << "version: " << header.version
        << "\nmetaBinaryOrder: " << unsigned(header.metaBinaryOrder)
        << "\nmetaDepth: " << unsigned(header.metaDepth)
        << "\nreferenceFrame: " << header.referenceFrame
        << "\nsourceHash: " << header.sourceHash
        << "\npairing: " << header.pairing
        << "\npages: " << store.pageCount()
        << '\n';

    return EXIT_SUCCESS;
}

int MnStoreTool::dump()
{
    mnstore::Store store(store_);
    const auto &header(store.header());

    if (pageId_) {
        mnstore::Page page;
        if (!store.read(*pageId_, page)) {
            std::cerr << "page " << *pageId_ << " not found\n";
            return EXIT_FAILURE;
        }
        dumpPage(header, page);
        return EXIT_SUCCESS;
    }

    for (const auto &root : store.pageIds()) {
        mnstore::Page page;
        store.read(root, page);
        dumpPage(header, page);
    }

    return EXIT_SUCCESS;
}

int MnStoreTool::check()
{
    // default the paired index to tiling.<rf> beside a metanodes.<rf>
    // store
    if (tileIndex_.empty()) {

        const auto name(store_.filename().string());
        const std::string prefix("metanodes.");
        if (name.compare(0, prefix.size(), prefix) != 0) {

            std::cerr << "cannot derive the paired tile index path from "
                      << store_ << "; pass it explicitly\n";
            return EXIT_FAILURE;
        }
        tileIndex_ = store_.parent_path()
            / ("tiling." + name.substr(prefix.size()));
    }

    mnstore::Store store(store_);
    const auto &header(store.header());

    if (mnstore::fileDigest(tileIndex_) != header.pairing) {

        std::cout << "pairing mismatch: store " << store_
                  << " is not paired with " << tileIndex_ << "\n";
        return EXIT_FAILURE;
    }

    vts::TileIndex index;
    index.load(tileIndex_);
    const auto lodRange(index.lodRange());

    // decoded-page cache; wholesale eviction is crude but the access
    // pattern is page-local
    std::map<vts::TileId, boost::optional<mnstore::Page>> cache;
    const std::size_t cacheLimit(4096);
    const auto payload([&](const vts::TileId &tileId)
                       -> const mnstore::NodeData*
    {
        const auto root(header.pageId(tileId));
        auto icache(cache.find(root));
        if (icache == cache.end()) {

            if (cache.size() >= cacheLimit) cache.clear();
            mnstore::Page page;
            const bool found(store.read(root, page));
            icache = cache.insert
                ({ root, found
                   ? boost::optional<mnstore::Page>(std::move(page))
                   : boost::none }).first;
        }
        if (!icache->second) return nullptr;
        const auto &node(icache->second->node(tileId));
        return node ? &node : nullptr;
    });

    const std::size_t reportLimit(20);
    std::size_t checked(0), violations(0);
    const auto report([&](const vts::TileId &tileId, const char *what)
    {
        ++violations;
        if (violations <= reportLimit)
            std::cout << tileId << ": " << what << "\n";
    });

    /* Store side: every payload node must be index-reachable (it has
     * flags, or a flagged descendant makes it a served structural
     * node), its parent must carry payload too (the reachable set is
     * parent-closed), and the parent range must contain its range.
     */
    for (const auto &root : store.pageIds()) {

        mnstore::Page page;
        if (!store.read(root, page)) continue;

        for (unsigned int level(0); level < header.metaDepth; ++level) {

            const auto size(header.levelSize(level));
            const vts::Lod lod(root.lod + level);
            for (unsigned int y(0); y < size; ++y) {

                for (unsigned int x(0); x < size; ++x) {

                    const auto &node(page.node(level, x, y));
                    if (!node) continue;
                    ++checked;

                    const vts::TileId tileId
                        (lod, (root.x << level) + x
                         , (root.y << level) + y);

                    if (!index.validSubtree(tileId)) {

                        report(tileId, "payload the index never reaches");
                        continue;
                    }
                    if (tileId.lod <= lodRange.min) continue;

                    const auto *parent(payload(vts::parent(tileId)));
                    if (!parent) {

                        report(tileId, "payload without parent payload");
                        continue;
                    }
                    if ((parent->min() > node.min())
                        || (parent->max() < node.max()))
                        report(tileId
                               , "parent range does not contain child");
                }
            }
        }
    }

    // Children completely outside the reference frame's constrained
    // (partitioned) area are never emitted, so the sibling rule
    // applies to the remaining children only. Root infos are cached
    // so derived children share the constraint sampler (a
    // from-scratch NodeInfo builds a PROJ pipeline per construction).
    const auto *referenceFrame
        (vr::system.referenceFrames(header.referenceFrame
                                    , std::nothrow));
    if (!referenceFrame) {
        std::cout << "reference frame \"" << header.referenceFrame
                  << "\" not found in registry; treating every missing "
                  "child of a watertight tile as a sibling-rule "
                  "violation\n";
    }
    std::map<vts::TileId, vts::NodeInfo> rootInfos;
    const auto childValid([&](const vts::TileId &childId) -> bool
    {
        if (!referenceFrame) { return true; }
        const auto *root
            (referenceFrame->findSubtreeRoot(childId, std::nothrow));
        if (!root) { return false; }
        auto iroot(rootInfos.find(root->id));
        if (iroot == rootInfos.end()) {
            iroot = rootInfos.emplace
                (root->id, vts::NodeInfo(*referenceFrame, root->id))
                .first;
        }
        return deriveNodeInfo(iroot->second, childId).valid();
    });

    /* Index side: every flagged tile must carry payload, and a
     * watertight tile keeps all its reference-frame-valid children or
     * none (the prune sibling rule; unsupported for forceWatertight
     * stores).
     */
    typedef vts::TileIndex::Flag TiFlag;
    for (auto lod(lodRange.min); lod <= lodRange.max; ++lod) {

        const auto *tree(index.ctree(lod));
        if (!tree) continue;

        tree->forEachNode([&](unsigned int x, unsigned int y
                              , unsigned int size
                              , vts::QTree::value_type value)
        {
            if (!value) return;
            for (unsigned int row(y); row < y + size; ++row) {

                for (unsigned int col(x); col < x + size; ++col) {

                    const vts::TileId tileId(lod, col, row);
                    ++checked;

                    if (!payload(tileId)) {

                        report(tileId, "flagged tile without payload");
                        continue;
                    }

                    if (!(value & TiFlag::watertight)
                        || (lod >= lodRange.max))
                        continue;
                    unsigned int count(0);
                    for (const auto &childId : vts::children(tileId)) {
                        if (payload(childId)) ++count;
                    }
                    if (!count || (count == 4)) continue;

                    // partial set: a violation unless every missing
                    // child is invalid in the reference frame
                    for (const auto &childId : vts::children(tileId)) {

                        if (payload(childId) || !childValid(childId))
                            continue;
                        report(tileId, "watertight tile keeps a partial "
                               "child set");
                        break;
                    }
                }
            }
        });
    }

    if (violations > reportLimit)
        std::cout << "(" << (violations - reportLimit)
                  << " more violations not shown)\n";

    std::cout
        << "checked " << checked << " nodes against " << tileIndex_
        << ", " << violations << " violation"
        << ((violations == 1) ? "" : "s") << "\n";
    return violations ? EXIT_FAILURE : EXIT_SUCCESS;
}

/** Fills a synthetic node-payload tree spanning given lod range.
 *
 *  Node presence and payload are a deterministic function of the
 *  global tile id, so the same payload can be generated for any
 *  packaging shape and compared node by node.
 */
class SyntheticTree {
public:
    SyntheticTree(vts::Lod minLod, vts::Lod maxLod)
        : minLod_(minLod), maxLod_(maxLod)
    {}

    vts::LodRange lodRange() const { return { minLod_, maxLod_ }; }

    /** @return synthetic node payload for a tile; absent for ~1/4 of
     *  tiles (deterministically) */
    mnstore::NodeData node(const vts::TileId &tileId) const {
        mnstore::NodeData node;

        std::uint32_t hash(tileId.lod);
        hash = hash * 31 + tileId.x;
        hash = hash * 31 + tileId.y;
        hash ^= (hash >> 7);

        if ((hash & 3) == 3) { return node; } // absent

        node.heightRange(-500.0 + (hash % 9000)
                         , -500.0 + (hash % 9000) + (hash % 333));
        return node;
    }

private:
    vts::Lod minLod_;
    vts::Lod maxLod_;
};

/** Builds a store with given packaging from a synthetic tree; returns
 *  the file path.
 */
fs::path buildStore(const fs::path &path, const SyntheticTree &tree
                    , unsigned int metaBinaryOrder
                    , unsigned int metaDepth)
{
    mnstore::Header header;
    header.metaBinaryOrder = metaBinaryOrder;
    header.metaDepth = metaDepth;
    header.referenceFrame = "synthetic";
    header.sourceHash = "selftest";
    header.pairing = "selftest-pairing";

    mnstore::Writer writer(path, header);

    // collect page roots intersecting the synthetic world
    std::set<vts::TileId> roots;
    for (auto lod(tree.lodRange().min); lod <= tree.lodRange().max;
         ++lod)
    {
        const vts::TileId::index_type size(1u << lod);
        for (vts::TileId::index_type y(0); y < size; ++y) {
            for (vts::TileId::index_type x(0); x < size; ++x) {
                roots.insert(header.pageId(vts::TileId(lod, x, y)));
            }
        }
    }

    for (const auto &root : roots) {
        mnstore::Page page(header, root);
        for (unsigned int level(0); level < metaDepth; ++level) {
            const vts::Lod lod(root.lod + level);
            if (lod > tree.lodRange().max) { break; }
            const auto size(header.levelSize(level));
            const vts::TileId::index_type lodSize(1u << lod);
            for (unsigned int y(0); y < size; ++y) {
                for (unsigned int x(0); x < size; ++x) {
                    const vts::TileId tileId
                        (lod, (root.x << level) + x
                         , (root.y << level) + y);
                    if ((tileId.x >= lodSize) || (tileId.y >= lodSize)) {
                        continue;
                    }
                    if (lod < tree.lodRange().min) { continue; }
                    page.node(level, x, y) = tree.node(tileId);
                }
            }
        }
        writer.write(page);
    }

    writer.close();
    return path;
}

/** Verifies that every synthetic node reads back identical from the
 *  store.
 */
void verifyStore(const fs::path &path, const SyntheticTree &tree)
{
    mnstore::Store store(path);

    for (auto lod(tree.lodRange().min); lod <= tree.lodRange().max;
         ++lod)
    {
        const vts::TileId::index_type size(1u << lod);
        for (vts::TileId::index_type y(0); y < size; ++y) {
            for (vts::TileId::index_type x(0); x < size; ++x) {
                const vts::TileId tileId(lod, x, y);
                const auto expected(tree.node(tileId));

                mnstore::Page page;
                const bool found(store.read(tileId, page));
                const auto got(found ? page.node(tileId)
                               : mnstore::NodeData());

                if (got != expected) {
                    LOGTHROW(err3, std::runtime_error)
                        << "Selftest: node " << tileId
                        << " mismatch in store " << path << ".";
                }
            }
        }
    }
}

int MnStoreTool::selftest()
{
    const auto tmp(fs::temp_directory_path()
                   / fs::unique_path("mnstore-selftest-%%%%%%"));
    fs::create_directories(tmp);

    const SyntheticTree tree(2, 7);

    // current client-compatible packaging
    const auto storeDefault
        (buildStore(tmp / "default.mns", tree, 5, 1));
    // one non-default packaging (vertical subtrees)
    const auto storeSubtree
        (buildStore(tmp / "subtree.mns", tree, 2, 3));

    verifyStore(storeDefault, tree);
    verifyStore(storeSubtree, tree);

    // node payload equality across the two packaging shapes
    {
        mnstore::Store defaultStore(storeDefault);
        mnstore::Store subtreeStore(storeSubtree);
        for (auto lod(tree.lodRange().min);
             lod <= tree.lodRange().max; ++lod)
        {
            const vts::TileId::index_type size(1u << lod);
            for (vts::TileId::index_type y(0); y < size; ++y) {
                for (vts::TileId::index_type x(0); x < size; ++x) {
                    const vts::TileId tileId(lod, x, y);
                    mnstore::Page pageA, pageB;
                    const auto inA(defaultStore.read(tileId, pageA));
                    const auto inB(subtreeStore.read(tileId, pageB));
                    const auto nodeA(inA ? pageA.node(tileId)
                                     : mnstore::NodeData());
                    const auto nodeB(inB ? pageB.node(tileId)
                                     : mnstore::NodeData());
                    if (nodeA != nodeB) {
                        LOGTHROW(err3, std::runtime_error)
                            << "Selftest: payload differs between "
                            "packagings at " << tileId << ".";
                    }
                }
            }
        }
    }

    // header round trip
    {
        mnstore::Store store(storeSubtree);
        const auto &header(store.header());
        if ((header.metaBinaryOrder != 2) || (header.metaDepth != 3)
            || (header.referenceFrame != "synthetic")
            || (header.sourceHash != "selftest")
            || (header.pairing != "selftest-pairing"))
        {
            LOGTHROW(err3, std::runtime_error)
                << "Selftest: header round trip failed.";
        }
    }

    // page id phase rule
    {
        mnstore::Header header;
        header.metaBinaryOrder = 2;
        header.metaDepth = 3;
        const auto pageId(header.pageId(vts::TileId(7, 129, 66)));
        // root lod = 7 - (7 % 3) = 6; ancestor (6, 64, 33) masked by ~3
        if (pageId != vts::TileId(6, 64, 32)) {
            LOGTHROW(err3, std::runtime_error)
                << "Selftest: page id phase rule failed (got "
                << pageId << ").";
        }
    }

    // half quantisation is conservative
    {
        mnstore::NodeData node;
        node.heightRange(123.456, 4321.987);
        if ((node.min() > 123.456) || (node.max() < 4321.987)) {
            LOGTHROW(err3, std::runtime_error)
                << "Selftest: half bias is not conservative.";
        }
    }

    fs::remove_all(tmp);

    std::cout << "selftest passed\n";
    return EXIT_SUCCESS;
}

int MnStoreTool::run()
{
    if (command_ == "info") { return info(); }
    if (command_ == "dump") { return dump(); }
    if (command_ == "check") { return check(); }
    if (command_ == "selftest") { return selftest(); }

    std::cerr << "unknown command: " << command_ << "\n";
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[])
{
    return MnStoreTool()(argc, argv);
}
