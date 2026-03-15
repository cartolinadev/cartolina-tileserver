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
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "dbglog/dbglog.hpp"

#include "math/math.hpp"

#include "geo/coordinates.hpp"

#include "demtin.hpp"

namespace {

inline bool validSampleValue(double value)
{
    return (value >= -1e6);
}

struct GridPoint {
    int x;
    int y;

    bool operator==(const GridPoint &other) const
    {
        return (x == other.x) && (y == other.y);
    }
};

struct SampleInfo {
    double height = 0.0;
    double curvature = 0.0;
    bool valid = false;
};

struct EdgeKey {
    int a;
    int b;

    EdgeKey(int aa, int bb)
        : a(std::min(aa, bb)), b(std::max(aa, bb))
    {}

    bool operator==(const EdgeKey &other) const
    {
        return (a == other.a) && (b == other.b);
    }
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey &key) const
    {
        return (std::hash<int>()(key.a) << 1) ^ std::hash<int>()(key.b);
    }
};

struct EdgeOwner {
    int first = -1;
    int second = -1;
};

struct TriangleState {
    std::array<int, 3> vertices;
    int splitA = -1;
    int splitB = -1;
    int splitMid = -1;
    double error = 0.0;
    bool splittable = false;
    bool active = true;
};

struct HeapItem {
    double error;
    int triangle;

    bool operator<(const HeapItem &other) const
    {
        return error < other.error;
    }
};

class DemTinBuilder {
public:
    DemTinBuilder(const DemTinInput &input, const DemTinOptions &options)
        : input_(input), options_(options), rows_(input.dem.rows)
        , cols_(input.dem.cols)
        , width_(math::size(input.nodeInfo.extents()).width)
        , height_(math::size(input.nodeInfo.extents()).height)
        , tileScale_(std::max(width_, height_))
        , sampleDx_(width_ / std::max(cols_ - 1, 1))
        , sampleDy_(height_ / std::max(rows_ - 1, 1))
        , sampleScale2_(std::pow(std::max(sampleDx_, sampleDy_), 2.0))
        , fullyCovered_(input.coverage.full())
        , g2l_(geo::geo2local(input.nodeInfo.extents()))
    {
        samples_.resize(rows_ * cols_);
    }

    AugmentedMesh build()
    {
        LOG(info1) << "DEM TIN early stop fraction: "
                   << options_.earlyStopFraction
                   << ", tile size: " << width_ << "x" << height_
                   << ", derived early stop threshold: "
                   << (options_.earlyStopFraction * tileScale_) << ".";

        preprocessSamples();
        initialize();
        refine();
        return emit();
    }

private:
    void preprocessSamples()
    {
        for (int y = 0; y < rows_; ++y) {
            for (int x = 0; x < cols_; ++x) {
                sampleInfo(x, y) = sampleAt(x, y);
            }
        }

        for (int y = 0; y < rows_; ++y) {
            for (int x = 0; x < cols_; ++x) {
                auto &sample(sampleInfo(x, y));
                if (!sample.valid) { continue; }

                // approximate derivatives by finite difference
                const auto hxx(secondDerivativeX(x, y));
                const auto hxy(mixedDerivative(x, y));
                const auto hyy(secondDerivativeY(x, y));

                sample.curvature = options_.curvatureWeight
                    * demtin::curvatureSaliency
                    (hxx, hxy, hyy, options_.peakBonusAlpha)
                    * sampleScale2_;
            }
        }
    }

    SampleInfo sampleAt(int x, int y)
    {
        SampleInfo sample;

        if (!input_.coverage.get(x, y)) {
            fullyCovered_ = false;
            return sample;
        }

        auto height(rawHeight(x, y));
        if (!validSampleValue(height)) {
            double sum(0.0);
            int count(0);
            for (int yy = y - 1; yy <= y + 1; ++yy) {
                for (int xx = x - 1; xx <= x + 1; ++xx) {
                    if ((xx == x) && (yy == y)) { continue; }
                    if (!inside(xx, yy)) { continue; }
                    if (!input_.coverage.get(xx, yy)) { continue; }
                    const auto neighbor(rawHeight(xx, yy));
                    if (!validSampleValue(neighbor)) { continue; }
                    sum += neighbor;
                    ++count;
                }
            }

            if (!count) {
                fullyCovered_ = false;
                return sample;
            }

            height = sum / count;
            fullyCovered_ = false;
        }

        if (input_.heightFunction) {
            height = (*input_.heightFunction)(height);
        }

        sample.height = height;
        sample.valid = true;
        return sample;
    }

    void initialize()
    {
        triangles_.reserve(std::max(options_.maxFaces * 2, 16));

        const int topLeft(index(0, 0));
        const int topRight(index(cols_ - 1, 0));
        const int bottomLeft(index(0, rows_ - 1));
        const int bottomRight(index(cols_ - 1, rows_ - 1));

        addTriangle({ topLeft, bottomLeft, bottomRight });
        addTriangle({ topLeft, bottomRight, topRight });
        leafCount_ = 2;
    }

    void refine()
    {
        while (!queue_.empty() && (leafCount_ < options_.maxFaces)) {
            const auto top(queue_.top());
            queue_.pop();

            if ((top.triangle < 0) || (top.triangle >= int(triangles_.size()))) {
                continue;
            }

            auto &triangle(triangles_[top.triangle]);
            if (!triangle.active || !triangle.splittable) { continue; }
            if (triangle.error != top.error) { continue; }

            if (demtin::normalizedError(triangle.error, tileScale_)
                < options_.earlyStopFraction)
            {
                break;
            }

            const auto key(EdgeKey(triangle.splitA, triangle.splitB));
            const auto ownerIt(edgeOwners_.find(key));
            const int neighbor((ownerIt == edgeOwners_.end())
                               ? -1 : otherOwner(ownerIt->second, top.triangle));
            const int splitCost((neighbor >= 0) ? 2 : 1);
            if ((leafCount_ + splitCost) > options_.maxFaces) {
                break;
            }

            splitTriangle(top.triangle, triangle.splitA, triangle.splitB
                          , triangle.splitMid);
            if (neighbor >= 0) {
                splitTriangle(neighbor, triangle.splitA, triangle.splitB
                              , triangle.splitMid);
            }

            leafCount_ += splitCost;
        }
    }

    AugmentedMesh emit()
    {
        AugmentedMesh out;
        out.fullyCovered = fullyCovered_;

        auto &mesh(out.mesh);
        mesh.vertices.reserve(leafCount_ * 3);
        mesh.faces.reserve(leafCount_);

        for (const auto &triangle : triangles_) {
            if (!triangle.active) { continue; }
            if (!(sample(triangle.vertices[0]).valid
                  && sample(triangle.vertices[1]).valid
                  && sample(triangle.vertices[2]).valid))
            {
                continue;
            }

            const auto a(vertexIndex(mesh, triangle.vertices[0]));
            const auto b(vertexIndex(mesh, triangle.vertices[1]));
            const auto c(vertexIndex(mesh, triangle.vertices[2]));
            addFace(mesh, a, b, c);
        }

        LOG(info2) << "Generated DEM TIN with " << mesh.faces.size()
                   << " faces, normalized residual score: "
                   << maxNormalizedResidualScore() << ".";

        return out;
    }

    int vertexIndex(geometry::Mesh &mesh, int sampleIndex)
    {
        auto it(vertexIndices_.find(sampleIndex));
        if (it != vertexIndices_.end()) {
            return it->second;
        }

        const auto gp(point(sampleIndex));
        const auto x(input_.nodeInfo.extents().ll(0) + gp.x * sampleDx_);
        const auto y(input_.nodeInfo.extents().ur(1) - gp.y * sampleDy_);
        const auto local(transform(g2l_, math::Point3(x, y
            , sample(sampleIndex).height)));

        const int vertex(mesh.vertices.size());
        mesh.vertices.push_back(local);
        vertexIndices_.emplace(sampleIndex, vertex);
        return vertex;
    }

    void addFace(geometry::Mesh &mesh, int a, int b, int c)
    {
        const auto &va(mesh.vertices[a]);
        const auto &vb(mesh.vertices[b]);
        const auto &vc(mesh.vertices[c]);
        const auto cross(((vb(0) - va(0)) * (vc(1) - va(1)))
                         - ((vb(1) - va(1)) * (vc(0) - va(0))));

        if (cross >= 0.0) {
            mesh.addFace(a, b, c);
        } else {
            mesh.addFace(a, c, b);
        }
    }

    void addTriangle(const std::array<int, 3> &vertices)
    {
        const int index(triangles_.size());
        TriangleState triangle;
        triangle.vertices = vertices;
        triangles_.push_back(triangle);
        registerTriangle(index);
        evaluate(index);
    }

    void splitTriangle(int triangleIndex, int splitA, int splitB, int splitMid)
    {
        auto &triangle(triangles_[triangleIndex]);
        if (!triangle.active) { return; }

        unregisterTriangle(triangleIndex);
        triangle.active = false;

        const int opposite(oppositeVertex(triangle, splitA, splitB));
        addTriangle({ opposite, splitA, splitMid });
        addTriangle({ opposite, splitMid, splitB });
    }

    void evaluate(int triangleIndex)
    {
        auto &triangle(triangles_[triangleIndex]);
        const auto best(splitEdge(triangle));
        triangle.splitA = std::get<0>(best);
        triangle.splitB = std::get<1>(best);
        triangle.splitMid = std::get<2>(best);
        triangle.error = std::get<3>(best);
        triangle.splittable = (triangle.splitMid >= 0);

        if (triangle.splittable) {
            queue_.push({ triangle.error, triangleIndex });
        }
    }

    std::tuple<int, int, int, double> splitEdge(const TriangleState &triangle) const
    {
        double bestLength(-1.0);
        std::pair<int, int> edge(-1, -1);
        for (const auto &candidate : { std::make_pair(0, 1)
                                      , std::make_pair(1, 2)
                                      , std::make_pair(2, 0) })
        {
            const auto a(triangle.vertices[candidate.first]);
            const auto b(triangle.vertices[candidate.second]);
            const auto len(edgeLength2(a, b));
            if (len > bestLength) {
                bestLength = len;
                edge = { a, b };
            }
        }

        if (edge.first < 0) {
            return { -1, -1, -1, 0.0 };
        }

        const auto pa(point(edge.first));
        const auto pb(point(edge.second));
        if (((pa.x + pb.x) & 1) || ((pa.y + pb.y) & 1)) {
            return { edge.first, edge.second, -1, 0.0 };
        }

        const int mx((pa.x + pb.x) / 2);
        const int my((pa.y + pb.y) / 2);
        const int mid(index(mx, my));
        if (!sample(mid).valid || (mid == edge.first) || (mid == edge.second)) {
            return { edge.first, edge.second, -1, 0.0 };
        }

        const double linear(0.5 * (sample(edge.first).height
                                   + sample(edge.second).height));
        const double error(std::abs(sample(mid).height - linear)
                           + sample(mid).curvature);
        return { edge.first, edge.second, mid, error };
    }

    double maxNormalizedResidualScore() const
    {
        double maxError(0.0);
        for (const auto &triangle : triangles_) {
            if (!triangle.active || !triangle.splittable) { continue; }
            maxError = std::max(maxError
                                , demtin::normalizedError(triangle.error
                                                          , tileScale_));
        }
        return maxError;
    }

    int oppositeVertex(const TriangleState &triangle, int a, int b) const
    {
        for (const auto vertex : triangle.vertices) {
            if ((vertex != a) && (vertex != b)) { return vertex; }
        }
        return triangle.vertices[0];
    }

    double edgeLength2(int a, int b) const
    {
        const auto pa(point(a));
        const auto pb(point(b));
        const auto dx(pa.x - pb.x);
        const auto dy(pa.y - pb.y);
        return (dx * dx * sampleDx_ * sampleDx_)
            + (dy * dy * sampleDy_ * sampleDy_);
    }

    void registerTriangle(int triangleIndex)
    {
        auto &owners(edgeOwners_);
        const auto &triangle(triangles_[triangleIndex]);
        for (const auto edge : edges(triangle)) {
            auto &owner(owners[edge]);
            if (owner.first < 0) {
                owner.first = triangleIndex;
            } else if (owner.second < 0) {
                owner.second = triangleIndex;
            }
        }
    }

    void unregisterTriangle(int triangleIndex)
    {
        const auto &triangle(triangles_[triangleIndex]);
        for (const auto edge : edges(triangle)) {
            auto it(edgeOwners_.find(edge));
            if (it == edgeOwners_.end()) { continue; }

            auto &owner(it->second);
            if (owner.first == triangleIndex) {
                owner.first = owner.second;
                owner.second = -1;
            } else if (owner.second == triangleIndex) {
                owner.second = -1;
            }

            if ((owner.first < 0) && (owner.second < 0)) {
                edgeOwners_.erase(it);
            }
        }
    }

    std::array<EdgeKey, 3> edges(const TriangleState &triangle) const
    {
        return { EdgeKey(triangle.vertices[0], triangle.vertices[1])
               , EdgeKey(triangle.vertices[1], triangle.vertices[2])
               , EdgeKey(triangle.vertices[2], triangle.vertices[0]) };
    }

    int otherOwner(const EdgeOwner &owner, int triangle) const
    {
        if (owner.first == triangle) { return owner.second; }
        if (owner.second == triangle) { return owner.first; }
        return -1;
    }

    const SampleInfo& sample(int linearIndex) const
    {
        return samples_[linearIndex];
    }

    SampleInfo& sampleInfo(int x, int y)
    {
        return samples_[index(x, y)];
    }

    GridPoint point(int linearIndex) const
    {
        return { linearIndex % cols_, linearIndex / cols_ };
    }

    int index(int x, int y) const
    {
        return y * cols_ + x;
    }

    bool inside(int x, int y) const
    {
        return (x >= 0) && (x < cols_) && (y >= 0) && (y < rows_);
    }

    double rawHeight(int x, int y) const
    {
        return input_.dem.at<double>(y, x);
    }

    double heightAt(int x, int y) const
    {
        if (!inside(x, y)) { return std::numeric_limits<double>::quiet_NaN(); }
        const auto &s(sample(index(x, y)));
        return s.valid ? s.height : std::numeric_limits<double>::quiet_NaN();
    }

    double secondDerivativeX(int x, int y) const
    {
        if (cols_ < 3) { return 0.0; }
        if (!sample(index(x, y)).valid) { return 0.0; }

        if ((x > 0) && (x + 1 < cols_)
            && sample(index(x - 1, y)).valid && sample(index(x + 1, y)).valid)
        {
            return (heightAt(x - 1, y) - 2.0 * heightAt(x, y)
                    + heightAt(x + 1, y)) / (sampleDx_ * sampleDx_);
        }

        if ((x + 2 < cols_) && sample(index(x + 1, y)).valid
            && sample(index(x + 2, y)).valid)
        {
            return (heightAt(x, y) - 2.0 * heightAt(x + 1, y)
                    + heightAt(x + 2, y)) / (sampleDx_ * sampleDx_);
        }

        if ((x - 2 >= 0) && sample(index(x - 1, y)).valid
            && sample(index(x - 2, y)).valid)
        {
            return (heightAt(x, y) - 2.0 * heightAt(x - 1, y)
                    + heightAt(x - 2, y)) / (sampleDx_ * sampleDx_);
        }

        return 0.0;
    }

    double secondDerivativeY(int x, int y) const
    {
        if (rows_ < 3) { return 0.0; }
        if (!sample(index(x, y)).valid) { return 0.0; }

        if ((y > 0) && (y + 1 < rows_)
            && sample(index(x, y - 1)).valid && sample(index(x, y + 1)).valid)
        {
            return (heightAt(x, y - 1) - 2.0 * heightAt(x, y)
                    + heightAt(x, y + 1)) / (sampleDy_ * sampleDy_);
        }

        if ((y + 2 < rows_) && sample(index(x, y + 1)).valid
            && sample(index(x, y + 2)).valid)
        {
            return (heightAt(x, y) - 2.0 * heightAt(x, y + 1)
                    + heightAt(x, y + 2)) / (sampleDy_ * sampleDy_);
        }

        if ((y - 2 >= 0) && sample(index(x, y - 1)).valid
            && sample(index(x, y - 2)).valid)
        {
            return (heightAt(x, y) - 2.0 * heightAt(x, y - 1)
                    + heightAt(x, y - 2)) / (sampleDy_ * sampleDy_);
        }

        return 0.0;
    }

    double mixedDerivative(int x, int y) const
    {
        if ((x <= 0) || (x + 1 >= cols_) || (y <= 0) || (y + 1 >= rows_)) {
            return 0.0;
        }

        if (!(sample(index(x - 1, y - 1)).valid
              && sample(index(x + 1, y - 1)).valid
              && sample(index(x - 1, y + 1)).valid
              && sample(index(x + 1, y + 1)).valid))
        {
            return 0.0;
        }

        return (heightAt(x + 1, y + 1) - heightAt(x + 1, y - 1)
                - heightAt(x - 1, y + 1) + heightAt(x - 1, y - 1))
            / (4.0 * sampleDx_ * sampleDy_);
    }

    const DemTinInput &input_;
    const DemTinOptions &options_;
    const int rows_;
    const int cols_;
    const double width_;
    const double height_;
    const double tileScale_;
    const double sampleDx_;
    const double sampleDy_;
    const double sampleScale2_;
    bool fullyCovered_;
    const math::Matrix4 g2l_;

    std::vector<SampleInfo> samples_;
    std::vector<TriangleState> triangles_;
    std::priority_queue<HeapItem> queue_;
    std::unordered_map<EdgeKey, EdgeOwner, EdgeKeyHash> edgeOwners_;
    std::unordered_map<int, int> vertexIndices_;
    int leafCount_ = 0;
};

} // namespace

namespace demtin {

std::pair<double, double> eigenvalues(double hxx, double hxy, double hyy)
{
    const double trace(hxx + hyy);
    const double diff(hxx - hyy);
    const double root(std::sqrt(std::max(0.0, diff * diff + 4.0 * hxy * hxy)));
    return { 0.5 * (trace - root), 0.5 * (trace + root) };
}

double curvatureSaliency(double hxx, double hxy, double hyy, double alpha)
{
    const auto ev(eigenvalues(hxx, hxy, hyy));
    return std::max(0.0, -ev.first) + alpha * std::max(0.0, -ev.second);
}

double normalizedError(double error, double tileScale)
{
    if (tileScale <= 0.0) { return error; }
    return error / tileScale;
}

} // namespace demtin

AugmentedMesh demTinMesh(const DemTinInput &input, const DemTinOptions &options)
{
    return DemTinBuilder(input, options).build();
}
