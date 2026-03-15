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

#include <cmath>
#include <iostream>
#include <stdexcept>

#include "../support/demtin.hpp"

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool nearlyEqual(double lhs, double rhs, double eps = 1e-9)
{
    return std::abs(lhs - rhs) < eps;
}

} // namespace

int main()
{
    const auto ev(demtin::eigenvalues(-4.0, 0.0, -1.0));
    require(ev.first <= ev.second, "eigenvalues are not sorted");
    require(nearlyEqual(ev.first, -4.0), "unexpected first eigenvalue");
    require(nearlyEqual(ev.second, -1.0), "unexpected second eigenvalue");

    const auto ridge(demtin::curvatureSaliency(-2.0, 0.0, 1.0, 0.75));
    const auto peak(demtin::curvatureSaliency(-2.0, 0.0, -1.0, 0.75));
    require(ridge > 0.0, "ridge saliency must be positive");
    require(peak > ridge, "peak saliency must exceed ridge saliency");
    require(nearlyEqual(peak, ridge + 0.75), "unexpected peak saliency delta");

    require(nearlyEqual(demtin::normalizedError(10.0, 1000.0), 0.01)
            , "normalized error mismatch for large tile");
    require(nearlyEqual(demtin::normalizedError(10.0, 100.0), 0.1)
            , "normalized error mismatch for small tile");

    std::cout << "demtin tests passed\n";
    return 0;
}
