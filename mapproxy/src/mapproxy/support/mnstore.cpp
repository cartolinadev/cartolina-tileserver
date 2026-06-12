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

#include <algorithm>
#include <fstream>

#include "dbglog/dbglog.hpp"

#include "utility/binaryio.hpp"
#include "utility/filesystem.hpp"
#include "utility/md5.hpp"
#include "utility/streams.hpp"

#include "half/half.hpp"

#include "mnstore.hpp"
#include "mmapped/memory-impl.hpp"

namespace fs = boost::filesystem;
namespace bin = utility::binaryio;
namespace half = half_float::detail;

namespace mnstore {

namespace {

const char MNSTORE_MAGIC[4] = { 'M', 'N', 'S', '0' };

/** Page-encoding quadrant tags.
 */
enum class Tag : std::uint8_t {
    empty = 0      // no node anywhere in the quadrant
    , uniform = 1  // one payload covers the whole quadrant
    , internal = 2 // 4 children follow depth-first (ul, ur, ll, lr)
};

const float halfMax(65504.f);

/** Steps a half-precision value one ulp toward minus infinity.
 */
std::uint16_t prevHalf(std::uint16_t value)
{
    if (value == 0x0000) { return 0x8001; } // +0 -> smallest negative
    if (value & 0x8000) { return value + 1; } // negative: grow magnitude
    return value - 1; // positive: shrink magnitude
}

/** Steps a half-precision value one ulp toward plus infinity.
 */
std::uint16_t nextHalf(std::uint16_t value)
{
    if (value == 0x8000) { return 0x0001; } // -0 -> smallest positive
    if (value & 0x8000) { return value - 1; } // negative: shrink magnitude
    return value + 1; // positive: grow magnitude
}

std::uint16_t halfFloor(double value)
{
    const float clamped
        (std::min(std::max(float(value), -halfMax), halfMax));
    auto h(half::float2half<std::round_to_nearest>(clamped));
    if (half::half2float(h) > value) { h = prevHalf(h); }
    return h;
}

std::uint16_t halfCeil(double value)
{
    const float clamped
        (std::min(std::max(float(value), -halfMax), halfMax));
    auto h(half::float2half<std::round_to_nearest>(clamped));
    if (half::half2float(h) < value) { h = nextHalf(h); }
    return h;
}

void writeString(std::ostream &out, const std::string &value)
{
    if (value.size() > 255) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store: string too long (" << value.size()
            << " bytes).";
    }
    bin::write(out, std::uint8_t(value.size()));
    out.write(value.data(), value.size());
}

std::string readString(std::istream &in)
{
    const auto size(bin::read<std::uint8_t>(in));
    std::string value(size, '\0');
    in.read(&value[0], size);
    return value;
}

void writeHeader(std::ostream &out, const Header &header)
{
    bin::write(out, MNSTORE_MAGIC);
    bin::write(out, std::uint16_t(header.version));
    bin::write(out, std::uint8_t(header.metaBinaryOrder));
    bin::write(out, std::uint8_t(header.metaDepth));
    writeString(out, header.referenceFrame);
    writeString(out, header.sourceHash);
    writeString(out, header.pairing);
    writeString(out, header.geoidGrid);
    writeString(out, header.heightFunction);
}

Header readHeader(std::istream &in)
{
    mmapped::checkHeader(in, MNSTORE_MAGIC, 0, "metanode store");

    Header header;
    header.version = bin::read<std::uint16_t>(in);
    if (header.version != Header::currentVersion) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store: unsupported version "
            << header.version << ".";
    }
    header.metaBinaryOrder = bin::read<std::uint8_t>(in);
    header.metaDepth = bin::read<std::uint8_t>(in);
    if (!header.metaDepth) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store: invalid metaDepth 0.";
    }
    header.referenceFrame = readString(in);
    header.sourceHash = readString(in);
    header.pairing = readString(in);
    header.geoidGrid = readString(in);
    header.heightFunction = readString(in);
    return header;
}

/** Page directory record.
 */
struct DirEntry {
    vts::TileId root;
    std::uint64_t offset;
    std::uint32_t size;

    DirEntry() : offset(), size() {}

    bool operator<(const DirEntry &other) const {
        if (root.lod != other.root.lod) {
            return root.lod < other.root.lod;
        }
        if (root.y != other.root.y) { return root.y < other.root.y; }
        return root.x < other.root.x;
    }
};

void writeDirEntry(std::ostream &out, const DirEntry &entry)
{
    bin::write(out, std::uint8_t(entry.root.lod));
    bin::write(out, std::uint32_t(entry.root.x));
    bin::write(out, std::uint32_t(entry.root.y));
    bin::write(out, entry.offset);
    bin::write(out, entry.size);
}

DirEntry readDirEntry(std::istream &in)
{
    DirEntry entry;
    entry.root.lod = bin::read<std::uint8_t>(in);
    entry.root.x = bin::read<std::uint32_t>(in);
    entry.root.y = bin::read<std::uint32_t>(in);
    entry.offset = bin::read<std::uint64_t>(in);
    entry.size = bin::read<std::uint32_t>(in);
    return entry;
}

void writeNode(std::ostream &out, const NodeData &node)
{
    bin::write(out, node.flags);
    bin::write(out, node.minZ);
    bin::write(out, node.maxZ);
}

NodeData readNode(mmapped::MemoryReader &reader)
{
    NodeData node;
    node.flags = reader.read<std::uint8_t>();
    node.minZ = reader.read<std::uint16_t>();
    node.maxZ = reader.read<std::uint16_t>();
    return node;
}

/** Encodes one level-grid quadrant recursively; collapses quadrants
 *  that are uniformly empty or hold one identical payload.
 */
void encodeQuadrant(std::ostream &out, const Page::Level &level
                    , unsigned int x, unsigned int y, unsigned int size)
{
    const auto &first(level.grid[y * level.size + x]);
    bool uniform(true);
    for (unsigned int j(y); uniform && (j < y + size); ++j) {
        for (unsigned int i(x); i < x + size; ++i) {
            if (level.grid[j * level.size + i] != first) {
                uniform = false;
                break;
            }
        }
    }

    if (uniform) {
        if (!first) {
            bin::write(out, std::uint8_t(Tag::empty));
        } else {
            bin::write(out, std::uint8_t(Tag::uniform));
            writeNode(out, first);
        }
        return;
    }

    bin::write(out, std::uint8_t(Tag::internal));
    const auto half(size / 2);
    encodeQuadrant(out, level, x, y, half);
    encodeQuadrant(out, level, x + half, y, half);
    encodeQuadrant(out, level, x, y + half, half);
    encodeQuadrant(out, level, x + half, y + half, half);
}

/** Decodes one level-grid quadrant recursively.
 */
void decodeQuadrant(mmapped::MemoryReader &reader, Page::Level &level
                    , unsigned int x, unsigned int y, unsigned int size)
{
    const auto tag(Tag(reader.read<std::uint8_t>()));

    switch (tag) {
    case Tag::empty:
        return;

    case Tag::uniform: {
        const auto node(readNode(reader));
        for (unsigned int j(y); j < y + size; ++j) {
            for (unsigned int i(x); i < x + size; ++i) {
                level.grid[j * level.size + i] = node;
            }
        }
        return;
    }

    case Tag::internal: {
        if (size < 2) {
            LOGTHROW(err2, std::runtime_error)
                << "Metanode store: internal quadrant at unit cell.";
        }
        const auto half(size / 2);
        decodeQuadrant(reader, level, x, y, half);
        decodeQuadrant(reader, level, x + half, y, half);
        decodeQuadrant(reader, level, x, y + half, half);
        decodeQuadrant(reader, level, x + half, y + half, half);
        return;
    }
    }

    LOGTHROW(err2, std::runtime_error)
        << "Metanode store: invalid quadrant tag "
        << unsigned(tag) << ".";
}

} // namespace

void NodeData::heightRange(double min, double max)
{
    minZ = halfFloor(min);
    maxZ = halfCeil(max);
}

float NodeData::min() const { return half::half2float(minZ); }
float NodeData::max() const { return half::half2float(maxZ); }

vts::TileId Header::pageId(const vts::TileId &tileId) const
{
    auto id(tileId);

    // ascend to the page root lod (global vertical phase)
    const vts::Lod rootLod(tileId.lod - (tileId.lod % metaDepth));
    const auto diff(tileId.lod - rootLod);
    id.lod = rootLod;
    id.x >>= diff;
    id.y >>= diff;

    // mask to the page grid
    const vts::TileId::index_type mask
        ((vts::TileId::index_type(1) << metaBinaryOrder) - 1);
    id.x &= ~mask;
    id.y &= ~mask;
    return id;
}

Page::Page(const Header &header, const vts::TileId &root)
    : root_(root)
{
    levels_.reserve(header.metaDepth);
    for (unsigned int level(0); level < header.metaDepth; ++level) {
        levels_.emplace_back(header.levelSize(level));
    }
}

NodeData& Page::node(unsigned int level, unsigned int x, unsigned int y)
{
    auto &grid(levels_[level]);
    return grid.grid[y * grid.size + x];
}

const NodeData& Page::node(unsigned int level, unsigned int x
                           , unsigned int y) const
{
    const auto &grid(levels_[level]);
    return grid.grid[y * grid.size + x];
}

NodeData& Page::node(const vts::TileId &tileId)
{
    const unsigned int level(tileId.lod - root_.lod);
    const auto scaledRoot(vts::TileId(tileId.lod, root_.x << level
                                      , root_.y << level));
    return node(level, tileId.x - scaledRoot.x, tileId.y - scaledRoot.y);
}

const NodeData& Page::node(const vts::TileId &tileId) const
{
    return const_cast<Page*>(this)->node(tileId);
}

bool Page::empty() const
{
    for (const auto &level : levels_) {
        for (const auto &node : level.grid) {
            if (node) { return false; }
        }
    }
    return true;
}

bool Page::operator==(const Page &other) const
{
    if (root_ != other.root_) { return false; }
    if (levels_.size() != other.levels_.size()) { return false; }
    for (std::size_t level(0); level < levels_.size(); ++level) {
        if (levels_[level].size != other.levels_[level].size) {
            return false;
        }
        if (levels_[level].grid != other.levels_[level].grid) {
            return false;
        }
    }
    return true;
}

struct Writer::Detail {
    fs::path path;
    Header header;
    utility::ofstreambuf file;
    std::vector<DirEntry> directory;
    bool closed;

    Detail(const fs::path &path, const Header &header)
        : path(path), header(header), closed(false)
    {
        file.exceptions(std::ostream::failbit | std::ostream::badbit);
        file.open(path.string()
                  , std::ostream::out | std::ostream::trunc);
        writeHeader(file, header);
    }
};

Writer::Writer(const fs::path &path, const Header &header)
    : detail_(std::make_unique<Detail>(path, header))
{}

Writer::~Writer()
{
    if (detail_ && !detail_->closed) {
        LOG(warn2) << "Metanode store writer for " << detail_->path
                   << " destroyed without close().";
    }
}

void Writer::write(const Page &page)
{
    if (page.empty()) { return; }

    auto &file(detail_->file);

    DirEntry entry;
    entry.root = page.root();
    entry.offset = file.tellp();

    for (const auto &level : page.levels_) {
        encodeQuadrant(file, level, 0, 0, level.size);
    }

    entry.size = std::uint64_t(file.tellp()) - entry.offset;
    detail_->directory.push_back(entry);
}

void Writer::close()
{
    auto &file(detail_->file);
    auto &directory(detail_->directory);

    std::sort(directory.begin(), directory.end());

    const std::uint64_t dirOffset(file.tellp());
    bin::write(file, std::uint32_t(directory.size()));
    for (const auto &entry : directory) {
        writeDirEntry(file, entry);
    }

    // trailer: directory offset, fixed distance from file end
    bin::write(file, dirOffset);

    file.close();
    detail_->closed = true;
}

struct Store::Detail {
    std::shared_ptr<mmapped::Memory> memory;
    Header header;
    std::map<vts::TileId, DirEntry> directory;

    Detail(const fs::path &path)
        : memory(std::make_shared<mmapped::Memory>(path))
    {
        auto &stream(memory->stream);
        header = readHeader(stream);

        if (memory->size < sizeof(std::uint64_t)) {
            LOGTHROW(err2, std::runtime_error)
                << "Metanode store " << path << ": truncated file.";
        }

        // directory offset lives in the trailer at the file end
        stream.seekg(memory->size - sizeof(std::uint64_t));
        const auto dirOffset(bin::read<std::uint64_t>(stream));
        if (dirOffset >= memory->size) {
            LOGTHROW(err2, std::runtime_error)
                << "Metanode store " << path
                << ": invalid directory offset.";
        }

        stream.seekg(dirOffset);
        const auto pageCount(bin::read<std::uint32_t>(stream));
        for (std::uint32_t page(0); page < pageCount; ++page) {
            const auto entry(readDirEntry(stream));
            directory.insert({ entry.root, entry });
        }
    }
};

Store::Store(const fs::path &path)
    : detail_(std::make_unique<Detail>(path))
{}

Store::~Store() = default;

const Header& Store::header() const { return detail_->header; }

std::size_t Store::pageCount() const
{
    return detail_->directory.size();
}

bool Store::read(const vts::TileId &tileId, Page &page) const
{
    const auto &header(detail_->header);
    const auto root(header.pageId(tileId));

    const auto entry(detail_->directory.find(root));
    if (entry == detail_->directory.end()) { return false; }

    page = Page(header, root);
    mmapped::MemoryReader reader
        (detail_->memory->addr(entry->second.offset));
    for (auto &level : page.levels_) {
        decodeQuadrant(reader, level, 0, 0, level.size);
    }

    if (reader.address() != entry->second.size) {
        LOGTHROW(err2, std::runtime_error)
            << "Metanode store: page " << root << " decode size mismatch ("
            << reader.address() << " != " << entry->second.size << ").";
    }

    return true;
}

std::vector<vts::TileId> Store::pageIds() const
{
    std::vector<vts::TileId> ids;
    ids.reserve(detail_->directory.size());
    for (const auto &item : detail_->directory) {
        ids.push_back(item.first);
    }
    return ids;
}

std::string fileDigest(const fs::path &path)
{
    std::ifstream file;
    file.exceptions(std::istream::badbit);
    file.open(path.string(), std::istream::in | std::istream::binary);
    if (!file) {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot open " << path << " for digest computation.";
    }

    utility::md5::Md5Sum sum;
    char buffer[65536];
    while (file) {
        file.read(buffer, sizeof(buffer));
        sum.append(buffer, file.gcount());
    }
    return sum.hash();
}

} // namespace mnstore
