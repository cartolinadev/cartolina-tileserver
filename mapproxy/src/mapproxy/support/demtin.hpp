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

#ifndef mapproxy_support_demtin_hpp_included_
#define mapproxy_support_demtin_hpp_included_

#include <utility>

#include <opencv2/core/core.hpp>

#include "../heightfunction.hpp"
#include "mesh.hpp"

namespace demtin {

std::pair<double, double> eigenvalues(double hxx, double hxy, double hyy);

double curvatureSaliency(double hxx, double hxy, double hyy, double alpha);

double normalizedError(double error, double tileScale);

} // namespace demtin

struct DemTinOptions {
    int maxFaces = 3000;
    double curvatureWeight = 1.0;
    double peakBonusAlpha = 0.75;
    double earlyStopFraction = 0.01;
};

struct DemTinInput {
    const cv::Mat &dem;
    const vts::NodeInfo &nodeInfo;
    const vts::NodeInfo::CoverageMask &coverage;
    const HeightFunction::pointer &heightFunction;
    OptHeight defaultHeight;
};

AugmentedMesh demTinMesh(const DemTinInput &input, const DemTinOptions &options);

#endif // mapproxy_support_demtin_hpp_included_
