/**
 * Copyright (c) 2019 Melown Technologies SE
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

#include <opencv2/highgui/highgui.hpp>
#include <webp/encode.h>

#include "vts-libs/vts/opencv/atlas.hpp"
#include "utility/raise.hpp"

#include "atlas.hpp"

namespace vts = vtslibs::vts;

namespace {


void encodeToWebP(const cv::Mat& img, std::vector<unsigned char>& buf) {

    int width = img.cols;
    int height = img.rows;
    int stride = img.step;

    uint8_t* output = nullptr;
    size_t outputSize;

    if (img.type() != CV_8UC3) {
        throw utility::makeError<InternalError>("Unsupported image type.");
    }

    // note that we use the BGR (not the RGB) variant.
    // This is meant for normal maps.
    outputSize = WebPEncodeLosslessBGR(
        img.data, width, height, stride, &output);

    if (outputSize == 0) {
        throw utility::makeError<InternalError>("Failed to create WebP data");
    }

    buf.assign(output, output + outputSize);

    // cleanup
    WebPFree(output);
}



} // namespace


void sendImage(const cv::Mat &image, const Sink::FileInfo &sfi
               , RasterFormat format, bool atlas, Sink &sink)
{
    if (atlas) {
        // serialize as a single-image atlas

        // TODO: make quality configurable
        vts::opencv::Atlas a(75);
        a.add(image);

        std::ostringstream os;
        a.serialize(os);
        sink.content(os.str(), sfi);
        return;
    }

    // serialize as a raw image
    std::vector<unsigned char> buf;
    switch (format) {
    case RasterFormat::jpg:
        // TODO: make quality configurable
        cv::imencode(".jpg", image, buf
                     , { cv::IMWRITE_JPEG_QUALITY, 75 });
        break;

    case RasterFormat::png:
        cv::imencode(".png", image, buf
                     , { cv::IMWRITE_PNG_COMPRESSION, 9 });
        break;

    case RasterFormat::webp:
        // meant for normal maps
        encodeToWebP(image, buf);
        break;
    }

    sink.content(buf, sfi);
}
