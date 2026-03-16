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

#include <cstdlib>
#include <iterator>
#include <sstream>

#include <boost/filesystem.hpp>

#include "utility/buildsys.hpp"
#include "service/cmdline.hpp"

#include "geometry/meshop.hpp"
#include "vts-libs/vts/mesh.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace vts = vtslibs::vts;

namespace {

geometry::Mesh flatten(const vts::Mesh &mesh)
{
    geometry::Mesh out;

    std::size_t vertexCount(0);
    std::size_t faceCount(0);
    for (const auto &sm : mesh.submeshes) {
        vertexCount += sm.vertices.size();
        faceCount += sm.faces.size();
    }

    out.vertices.reserve(vertexCount);
    out.faces.reserve(faceCount);

    for (const auto &sm : mesh.submeshes) {
        const auto offset(out.vertices.size());
        out.vertices.insert(out.vertices.end()
                            , sm.vertices.begin(), sm.vertices.end());
        for (const auto &face : sm.faces) {
            out.addFace(face[0] + offset, face[1] + offset, face[2] + offset);
        }
    }

    return out;
}

class Rmesh2Ply : public service::Cmdline {
public:
    Rmesh2Ply()
        : service::Cmdline("rmesh2ply", BUILD_TARGET_VERSION)
    {}

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd)
    {
        (void) cmdline;
        (void) config;
        (void) pd;
    }

    void configure(const po::variables_map &vars)
    {
        (void) vars;
    }

    bool help(std::ostream &out, const std::string &what) const
    {
        if (what.empty()) {
            out << ("rmesh to PLY converter\n"
                    "\n"
                    "Reads a raw VTS mesh tile (.rmesh) from stdin and writes\n"
                    "an ASCII PLY mesh to stdout.\n"
                    "\n"
                    "Usage:\n"
                    "    curl .../0-0-0.rmesh | rmesh2ply > tile.ply\n");
            return true;
        }
        return false;
    }

    int run()
    {
        const std::string input((std::istreambuf_iterator<char>(std::cin))
                                , std::istreambuf_iterator<char>());
        if (input.empty()) {
            throw std::runtime_error("No input data on stdin.");
        }

        std::istringstream in(input);
        auto mesh(vts::loadMesh(in, "stdin"));
        geometry::saveAsPly(flatten(mesh), fs::path("/dev/stdout"));
        return EXIT_SUCCESS;
    }
};

} // namespace

int main(int argc, char *argv[])
{
    return Rmesh2Ply()(argc, argv);
}
