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

#ifndef mapproxy_support_tileindex_hpp_included_
#define mapproxy_support_tileindex_hpp_included_

#include <boost/filesystem.hpp>

#include "vts-libs/vts/tileindex.hpp"
#include "vts-libs/vts/tileset/tilesetindex.hpp"

#include "../resource.hpp"
#include "coverage.hpp"

namespace vts = vtslibs::vts;

enum class TileIndexExpansion {
    allow
    , reject
};

void prepareTileIndex(vts::TileIndex &index
                      , const boost::filesystem::path *tilesPath
                      , const Resource &resource
                      , bool navtiles = false
                      , const MaskTree &maskTree = MaskTree()
                      , TileIndexExpansion expansion
                      = TileIndexExpansion::allow);

void prepareTileIndex(vts::TileIndex &index
                      , const boost::filesystem::path &tilesPath
                      , const Resource &resource
                      , bool navtiles = false
                      , const MaskTree &maskTree = MaskTree()
                      , TileIndexExpansion expansion
                      = TileIndexExpansion::allow);

void prepareTileIndex(vts::TileIndex &index
                      , const Resource &resource
                      , bool navtiles = false
                      , const MaskTree &maskTree = MaskTree());

void prepareTileIndex(vts::tileset::Index &index
                      , const boost::filesystem::path &tilesPath
                      , const Resource &resource
                      , bool navtiles = false
                      , const MaskTree &maskTree = MaskTree()
                      , TileIndexExpansion expansion
                      = TileIndexExpansion::allow);

/** Records into root/delivery.index.src which flag tile index the cached
 *  delivery index was derived from (the tiling's digest) and by which
 *  derivation (deliveryIndexDerivation at build time).
 */
void saveDeliveryIndexSource(const boost::filesystem::path &root
                             , const std::string &pairing);

/** True when root/delivery.index.src records the given tiling digest and
 *  the current derivation revision; anything else means the cached
 *  delivery index is stale and the generator must re-prepare.
 */
bool deliveryIndexCurrent(const boost::filesystem::path &root
                          , const std::string &pairing);

/** Identifies the derivation prepareTileIndex implements. Bump whenever
 *  prepareTileIndex starts deriving different output from the same inputs
 *  so that cached delivery indexes are rebuilt on reopen.
 */
constexpr unsigned int deliveryIndexDerivation = 2;

// inlines

inline void prepareTileIndex(vts::tileset::Index &index
                             , const boost::filesystem::path &tilesPath
                             , const Resource &resource
                             , bool navtiles
                             , const MaskTree &maskTree
                             , TileIndexExpansion expansion)
{
    prepareTileIndex(index.tileIndex, tilesPath, resource, navtiles, maskTree
                     , expansion);
}

inline void prepareTileIndex(vts::TileIndex &index
                             , const boost::filesystem::path &tilesPath
                             , const Resource &resource
                             , bool navtiles
                             , const MaskTree &maskTree
                             , TileIndexExpansion expansion)
{
    return prepareTileIndex(index, &tilesPath, resource, navtiles, maskTree
                            , expansion);
}

inline void prepareTileIndex(vts::TileIndex &index
                             , const Resource &resource
                             , bool navtiles
                             , const MaskTree &maskTree)
{
    return prepareTileIndex(index, nullptr, resource, navtiles, maskTree);
}

#endif // mapproxy_support_tileindex_hpp_included_
