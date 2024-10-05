/**
 * Copyright (c) 2024 Ondrej Prochazka
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

#ifndef mapproxy_generator_tms_normalmap_hpp_included_
#define mapproxy_generator_tms_normalmap_hpp_included_

#include "tms-raster.hpp"

#include "geo/landcover.hpp"

namespace generator {

class TmsNormalMap : public TmsRaster
{
public:

    // needed for Generator::registerType
    typedef resource::TmsNormalMap Definition;

    TmsNormalMap(const Params &params);

private:


    virtual void extraPrep() override;

    void generateTileImage(const vts::TileId &tileId
                                   , const Sink::FileInfo &fi
                                   , RasterFormat format
                                   , Sink &sink, Arsenal &arsenal
                                   , const ImageFlags &imageFlags
                                   = ImageFlags()) const override;

    RasterFormat format() const override;

    int generatorRevision() const override;

    // load landcover class def from file
    void loadLandcoverClassdef();

    // path to optional landcover
    boost::optional<const LandcoverDataset> landcover_;

    // normal map parameters
    struct {
        float zFactor;
        bool invertRelief;
    } params_;

    // loaded landcover class definition;
    geo::landcover::Classes lcClassdef_;

};

} // namespace generator

#endif // mapproxy_generator_tms_normalmap_hpp_included_
