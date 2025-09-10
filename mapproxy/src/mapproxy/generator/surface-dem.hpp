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

#ifndef mapproxy_generator_surface_dem_hpp_included_
#define mapproxy_generator_surface_dem_hpp_included_

#include "vts-libs/vts/tileset/tilesetindex.hpp"
#include "vts-libs/vts/tileset/properties.hpp"
#include "geo/landcover.hpp"

#include "surface.hpp"

#include "../support/coverage.hpp"

namespace vts = vtslibs::vts;
//namespace vr = vtslibs::registry;

namespace generator {

class SurfaceDem : public SurfaceBase {
public:
    SurfaceDem(const Params &params);

    ~SurfaceDem();

    typedef resource::SurfaceDem Definition;

private:
    virtual void prepare_impl(Arsenal &arsenal);
    virtual vts::MapConfig mapConfig_impl(ResourceRoot root) const;

    virtual void generateMetatile(const vts::TileId &tileId
                                  , Sink &sink
                                  , const SurfaceFileInfo &fileInfo
                                  , Arsenal &arsenal
                                  , const MetatileOverrides &overrides)
        const;

    virtual AugmentedMesh
    generateMeshImpl(const vts::NodeInfo &nodeInfo, Sink &sink
                     , Arsenal &arsenal, const OptHeight &defaultHeight) const;

    virtual cv::Mat
    generateNormalMapImpl(const vts::NodeInfo &nodeInfo
                          , Sink &sink, Arsenal &arsenal) const;

    virtual void generateNavtile(const vts::TileId &tileId
                                 , Sink &sink
                                 , const SurfaceFileInfo &fileInfo
                                 , Arsenal &arsenal) const;

    vts::MetaTile generateMetatileImpl(const vts::TileId &tileId
                                       , Sink &sink, Arsenal &arsenal
                                       , const MetatileOverrides &overrides
                                       = {}) const;

    void addToRegistry();

    void removeFromRegistry();

    void loadLandcoverClassdef();

    virtual unsigned int generatorRevision() const;

    const Definition &definition_;

    /** Path to original dataset (must contain overviews)
     */
    const DemDataset dem_;

    // path to optional landcover
    boost::optional<const LandcoverDataset> landcover_;

    // loaded landcover class definition;
    geo::landcover::Classes lcClassdef_;

    // mask tree
    MaskTree maskTree_;
};

} // namespace generator

#endif // mapproxy_generator_surface_dem_hpp_included_
