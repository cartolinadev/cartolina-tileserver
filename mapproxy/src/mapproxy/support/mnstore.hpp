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

#ifndef mapproxy_support_mnstore_hpp_included_
#define mapproxy_support_mnstore_hpp_included_

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <boost/filesystem.hpp>

#include "vts-libs/vts/basetypes.hpp"

namespace vts = vtslibs::vts;

/** RFC 7 metanode store (rfc-metanode-store.md).
 *
 * A paged, mmappable height sidecar of the flag tile index. The index
 * is the sole authority for tile existence and delivered flags
 * (mesh/watertight/navtile; metanode existence and child flags are
 * derived from it at delivery). The store carries the one thing the
 * index cannot: for every node the index serves, the source height
 * range over its cell — including geometry-less structural nodes,
 * whose range bounds every descendant mesh. Both artifacts are
 * written by one tiling run and bound by a pairing digest.
 *
 * A page is one metatile delivery unit in the resource's effective
 * packaging (effective metaBinaryOrder/metaDepth): metaDepth level
 * grids, each encoded as a two-dimensional local quadtree with uniform
 * quadrants collapsed. Page keys are metatile root ids: the tile's
 * ancestor at the root LOD with x/y masked by ~((1 << order) - 1) and
 * root LODs at lod % metaDepth == 0. Node presence is defined by the
 * page encoding's quadrant tags.
 *
 * Stored heights are in the geoid-shifted SDS vertical declared by
 * the header's geoidGrid (orthometric; raw SDS when no geoid grid is
 * configured) — flat water collapses, and the values are
 * body-generic. The v6 serializer converts to the raw SDS vertical
 * at delivery by adding the geoid undulation. Heights are quantised
 * to half floats biased outward (minZ down, maxZ up) so the stored
 * range is conservative.
 */
namespace mnstore {

/** Node payload: the height range. A default-constructed node is
 *  absent — the paired index does not reach the tile and no payload is
 *  stored for it.
 */
struct NodeData {
    /** True when the node carries payload.
     */
    bool present;

    /** Height range in the store's vertical datum (geoid-shifted
     *  SDS), half-float encoded.
     */
    std::uint16_t minZ;
    std::uint16_t maxZ;

    NodeData() : present(false), minZ(), maxZ() {}

    explicit operator bool() const { return present; }

    bool operator==(const NodeData &other) const {
        return ((present == other.present) && (minZ == other.minZ)
                && (maxZ == other.maxZ));
    }
    bool operator!=(const NodeData &other) const {
        return !operator==(other);
    }

    /** Quantises a height range, biasing outward to the next
     *  representable half so the stored range is conservative, and
     *  marks the node present.
     *
     * @param min height range minimum (store datum)
     * @param max height range maximum (store datum)
     */
    void heightRange(double min, double max);

    /** @return stored minimum as float */
    float min() const;

    /** @return stored maximum as float */
    float max() const;
};

/** Store metadata; serialised in the file header.
 */
struct Header {
    static const std::uint16_t currentVersion = 2;

    std::uint16_t version;
    std::uint8_t metaBinaryOrder;
    std::uint8_t metaDepth;
    std::string referenceFrame;

    /** Digest of value-affecting source inputs (computed by tooling).
     */
    std::string sourceHash;

    /** Digest of the paired flag tile index file content.
     */
    std::string pairing;

    /** SDS vertical datum geoid grid the stored heights assume
     *  (resource geoidGrid setting); empty when none.
     */
    std::string geoidGrid;

    /** Canonical JSON of the height function baked into the stored
     *  heights; empty when none.
     */
    std::string heightFunction;

    Header()
        : version(currentVersion), metaBinaryOrder(5), metaDepth(1) {}

    /** @return number of subtree-root cells per page edge */
    unsigned int rootSize() const { return 1u << metaBinaryOrder; }

    /** @return number of node cells per edge of page level
     *  @param level page level in [0, metaDepth) */
    unsigned int levelSize(unsigned int level) const {
        return rootSize() << level;
    }

    /** Computes the page key (metatile root id) of a tile.
     *
     * @param tileId any tile covered by the page
     * @return page root id (root LOD ancestor, masked to page grid)
     */
    vts::TileId pageId(const vts::TileId &tileId) const;
};

/** Decoded page: per-level dense node grids.
 */
class Page {
public:
    Page() {}

    /** Prepares an empty page shaped by header packaging.
     *
     * @param header store header (packaging source)
     * @param root page root id
     */
    Page(const Header &header, const vts::TileId &root);

    const vts::TileId& root() const { return root_; }

    /** Node accessor.
     *
     * @param level page level in [0, metaDepth)
     * @param x column within level grid
     * @param y row within level grid
     * @return node data; absent (false) if no node
     */
    NodeData& node(unsigned int level, unsigned int x, unsigned int y);
    const NodeData& node(unsigned int level, unsigned int x
                         , unsigned int y) const;

    /** Node accessor by global tile id (must belong to this page).
     *
     * @param tileId global tile id
     * @return node data; absent (false) if no node
     */
    NodeData& node(const vts::TileId &tileId);
    const NodeData& node(const vts::TileId &tileId) const;

    /** @return true if no node is present */
    bool empty() const;

    bool operator==(const Page &other) const;

    typedef std::vector<Page> list;

    /** One level grid (page-internal representation; public for the
     *  codec).
     */
    struct Level {
        unsigned int size;
        std::vector<NodeData> grid;

        Level(unsigned int size = 0) : size(size), grid(size * size) {}
    };

private:
    friend class Store;
    friend class Writer;

    vts::TileId root_;
    std::vector<Level> levels_;
};

/** Store writer. Pages may be added in any order; close() emits the
 *  page directory and finalises the file. The caller is responsible
 *  for the temp-write/fsync/rename publication sequence.
 */
class Writer {
public:
    /** Opens a store file for writing.
     *
     * @param path output file path
     * @param header store metadata
     */
    Writer(const boost::filesystem::path &path, const Header &header);
    ~Writer();

    /** Encodes and appends one page. Empty pages are skipped.
     *
     * @param page decoded page to write
     */
    void write(const Page &page);

    /** Writes the page directory and closes the file.
     */
    void close();

private:
    struct Detail;
    std::unique_ptr<Detail> detail_;
};

/** Mmapped store reader.
 */
class Store {
public:
    /** Opens and validates a store file.
     *
     * @param path store file path
     */
    Store(const boost::filesystem::path &path);
    ~Store();

    const Header& header() const;

    /** Number of pages in the directory.
     */
    std::size_t pageCount() const;

    /** Reads and decodes the page holding given tile.
     *
     * @param tileId any tile covered by the requested page
     * @param page decoded page output
     * @return false if the page is not present in the store
     */
    bool read(const vts::TileId &tileId, Page &page) const;

    /** Lists all page root ids (directory order).
     */
    std::vector<vts::TileId> pageIds() const;

private:
    struct Detail;
    std::unique_ptr<Detail> detail_;
};

/** Computes the hex digest of a file's content; used as the pairing
 *  value binding the store to its flag tile index.
 *
 * @param path file to digest
 * @return hex digest string
 */
std::string fileDigest(const boost::filesystem::path &path);

} // namespace mnstore

#endif // mapproxy_support_mnstore_hpp_included_
