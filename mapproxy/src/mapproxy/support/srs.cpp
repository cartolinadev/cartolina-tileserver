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

#include "math/math.hpp"
#include "math/transform.hpp"
#include "geo/csconvertor.hpp"
#include "geo/srsdef.hpp"

#include "vts-libs/vts/csconvertor.hpp"

#include "srs.hpp"

namespace vr = vtslibs::registry;

vts::CsConvertor sds2srs(const std::string &sds, const std::string &dst
                         , const boost::optional<std::string> &geoidGrid)
{
    if (!geoidGrid) {
        return vts::CsConvertor(sds, dst);
    }

    // force given geoid
    return vts::CsConvertor
        (geo::setGeoid(vr::system.srs(sds).srsDef, *geoidGrid)
         , dst);
}

vts::CsConvertor sds2phys(const vts::NodeInfo &nodeInfo
                          , const boost::optional<std::string> &geoidGrid)
{
    return sds2srs(nodeInfo.srs()
                   , nodeInfo.referenceFrame().model.physicalSrs
                   , geoidGrid);
}

vts::CsConvertor sds2nav(const vts::NodeInfo &nodeInfo
                         , const boost::optional<std::string> &geoidGrid)
{
    return sds2srs(nodeInfo.srs()
                   , nodeInfo.referenceFrame().model.navigationSrs
                   , geoidGrid);
}

geo::SrsDefinition sds(const vts::NodeInfo &nodeInfo
                       , const boost::optional<std::string> &geoidGrid)
{
    if (!geoidGrid) { return nodeInfo.srsDef(); }

    // force given geoid
    return geo::setGeoid(nodeInfo.srsDef(), *geoidGrid);
}

vts::CsConvertor sdsg2sdsr(const vts::NodeInfo &nodeInfo
                           , const boost::optional<std::string> &geoidGrid)
{
    if (!geoidGrid) { return {}; }
    return vts::CsConvertor(sds(nodeInfo, geoidGrid), nodeInfo.srs());
}

vts::CsConvertor phys2sds(const vts::NodeInfo &nodeInfo
                         , const boost::optional<std::string> &geoidGrid)
{
    return vts::CsConvertor(nodeInfo.referenceFrame().model.physicalSrs
                            , sds(nodeInfo, geoidGrid));
}

void validateGeoidGrid(const boost::optional<std::string> &geoidGrid)
{
    if (!geoidGrid || geoidGrid->empty()) { return; }

    // Reproduce the serve/warp conversion: applying the geoid to a
    // geographic SRS and transforming one point forces PROJ to build
    // and run the vertical (vgridshift) pipeline, which loads the grid.
    // Grid resolution is a property of PROJ and the grid string, not of
    // any particular base SRS, so a generic WGS84 base suffices.
    const geo::SrsDefinition wgs84("+proj=longlat +datum=WGS84 +no_defs");
    try {
        const geo::CsConvertor conv
            (geo::setGeoid(wgs84, *geoidGrid), wgs84);
        conv(math::Point3(0.0, 0.0, 0.0));
    } catch (const std::exception &e) {
        LOGTHROW(err3, std::runtime_error)
            << "Geoid grid '" << *geoidGrid << "' is not loadable by "
            "PROJ/GDAL (" << e.what() << "). Use a PROJ-readable grid such "
            "as 'egm96_15.gtx'; the VTS registry geoid grids (the "
            "geoidgrid/*.jpg files) are read only by the VTS C++ stack, "
            "not by PROJ, and would build a metanode store that fails to "
            "serve every metatile with a 500.";
    }
}

std::array<math::Point3, 4>
physicalCorners(const vts::NodeInfo &nodeInfo
                , const boost::optional<std::string> &geoidGrid)
{
    const auto conv(sds2phys(nodeInfo, geoidGrid));
    const auto &extents(nodeInfo.extents());

    return {{
        conv(math::Point3(extents.ll(0), extents.ll(1), 0.0))
        , conv(math::Point3(extents.ur(0), extents.ll(1), 0.0))
        , conv(math::Point3(extents.ur(0), extents.ur(1), 0.0))
        , conv(math::Point3(extents.ll(0), extents.ur(1), 0.0))
    }};
}

math::Matrix3 nodeTangentSpace(const vts::NodeInfo &nodeInfo
                               , const boost::optional<std::string> &geoidGrid)
{
    const auto corners(physicalCorners(nodeInfo, geoidGrid));
    const auto &ll(corners[0]);
    const auto &lr(corners[1]);
    const auto &ur(corners[2]);
    const auto &ul(corners[3]);

    const math::Point3 t(0.5 * (lr - ll) + 0.5 * (ur - ul));
    const math::Point3 b(0.5 * (ul - ll) + 0.5 * (ur - lr));
    constexpr double collapseDelta(1e-12);

    if ((boost::numeric::ublas::norm_2(t) <= collapseDelta)
        || (boost::numeric::ublas::norm_2(b) <= collapseDelta))
    {
        LOG(warn2)
            << "Collapsed node tangent space for node " << nodeInfo.nodeId()
            << "; returning identity TBN.";
        return math::identity3();
    }

    const auto n(math::normalize(math::crossProduct(t, b)));
    const auto tt(math::normalize(math::crossProduct(b, n)));
    const auto bb(math::normalize(b));

    math::Matrix3 m(boost::numeric::ublas::zero_matrix<double>(3, 3));
    boost::numeric::ublas::column(m, 0) = tt;
    boost::numeric::ublas::column(m, 1) = bb;
    boost::numeric::ublas::column(m, 2) = n;
    return m;
}
