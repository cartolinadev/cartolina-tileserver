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

/** Computes eigenvalues of a 2x2 symmetric Hessian matrix.
 *
 * @param hxx second derivative in X direction
 * @param hxy mixed second derivative
 * @param hyy second derivative in Y direction
 * @return pair of eigenvalues ordered from smaller to larger
 */
std::pair<double, double> eigenvalues(double hxx, double hxy, double hyy);

/** Computes curvature saliency from Hessian coefficients.
 *
 * Negative principal curvatures increase saliency; the second negative
 * eigenvalue is weighted by @p alpha to slightly prefer peak-like features
 * over ridge-like ones.
 *
 * @param hxx second derivative in X direction
 * @param hxy mixed second derivative
 * @param hyy second derivative in Y direction
 * @param alpha weight of the second negative eigenvalue
 * @return non-negative curvature saliency
 */
double curvatureSaliency(double hxx, double hxy, double hyy, double alpha);

/** Normalizes geometric error by tile scale.
 *
 * @param error absolute geometric error in tile SRS units
 * @param tileScale normalization scale, typically max(tileWidth, tileHeight)
 * @return normalized error
 */
double normalizedError(double error, double tileScale);

} // namespace demtin

struct DemTinOptions {
    /** Maximum number of output faces. */
    int maxFaces = 3000;
    /** Weight of the curvature term in the split error metric. */
    double curvatureWeight = 1.;
    /** Relative preference of peak-like features over ridge-like features. */
    double peakBonusAlpha = 0.75;
    /** Early-exit threshold expressed as a fraction of tile size. */
    double earlyStopFraction = 0.003;
};

struct DemTinInput {
    /** Warped DEM in grid registration, typically 129x129 samples. */
    const cv::Mat &dem;
    /** Tile metadata and extents used for geometry generation. */
    const vts::NodeInfo &nodeInfo;
    /** Validity/coverage mask for DEM samples. */
    const vts::NodeInfo::CoverageMask &coverage;
    /** Optional post-processing of sampled heights. */
    const HeightFunction::pointer &heightFunction;
};

/** Builds a terrain mesh directly from a DEM tile using adaptive RTIN refinement.
 *
 * The implementation follows the standard right-triangulated irregular network
 * (RTIN) idea and adaptive error-driven terrain refinement: it starts from two
 * root triangles and repeatedly splits the current highest-error triangle until
 * the face budget is exhausted or the normalized error drops below a threshold.
 * See Evans, Kirkpatrick, Townsend (Algorithmica, 2001) and Lindstrom/Pascucci
 * for the general terrain refinement approach.
 *
 * @param input DEM tile, coverage, and per-tile geometry context
 * @param options refinement budget and error-metric parameters
 * @return generated local mesh with coverage metadata
 */
AugmentedMesh demTinMesh(const DemTinInput &input, const DemTinOptions &options);

#endif // mapproxy_support_demtin_hpp_included_
