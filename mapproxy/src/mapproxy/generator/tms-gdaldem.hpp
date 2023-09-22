/**
 * Copyright (c) 2023 Ondrej Prochazka
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

#ifndef mapproxy_generator_tms_gdaldem_hpp_included_
#define mapproxy_generator_tms_gdaldem_hpp_included_

#include "geo/geodataset.hpp"
#include "../support/mmapped/tileindex.hpp"

#include "../definition/tms.hpp"

#include "tms-raster-base.hpp"

namespace generator {

namespace detail {

/** Member from base idiom */
class TmsGdaldemMFB {

protected:
    TmsGdaldemMFB(const Generator::Params &params);
    typedef resource::TmsGdaldem Definition;
    const Definition &definition_;
};

} // namespace detail

class TmsGdaldem
    : private detail::TmsGdaldemMFB
    , public TmsRasterBase
{
public:
    TmsGdaldem(const Params &params
              , const boost::optional<RasterFormat> &format = boost::none);

    using detail::TmsGdaldemMFB::Definition;


private:
    virtual void prepare_impl(Arsenal &);

    virtual vts::MapConfig mapConfig_impl(ResourceRoot root) const;

    virtual Task generateVtsFile_impl(const FileInfo &fileInfo
                                      , Sink &sink) const;


    void generateTileImage(const vts::TileId &tileId
                                   , const Sink::FileInfo &fi
                                   , RasterFormat format
                                   , Sink &sink, Arsenal &arsenal
                                   , const ImageFlags &imageFlags
                                   = ImageFlags()) const;

    void generateTileMask(const vts::TileId &tileId
                          , const TmsFileInfo &fi
                          , Sink &sink, Arsenal &arsenal) const;


    void generateMetatile(const vts::TileId &tileId
                          , const TmsFileInfo &fi
                          , Sink &sink, Arsenal &arsenal) const;

    vr::BoundLayer boundLayer(ResourceRoot root) const;

    std::unique_ptr<mmapped::TileIndex> index_;
};

} // namespace generator

#endif // mapproxy_generator_tms_gdalem_hpp_included_
