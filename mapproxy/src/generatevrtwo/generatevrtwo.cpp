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

#include <cstdlib>
#include <utility>
#include <functional>
#include <map>
#include <numeric>
#include <algorithm>

#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/thread.hpp>
#include <boost/format.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include <gdal/vrtdataset.h>

#include "cpl_minixml.h"

#include "utility/streams.hpp"
#include "utility/buildsys.hpp"
#include "utility/openmp.hpp"
#include "utility/raise.hpp"
#include "utility/duration.hpp"
#include "utility/time.hpp"
#include "service/cmdline.hpp"
#include "utility/enum-io.hpp"
#include "utility/path.hpp"

#include "geo/geodataset.hpp"
#include "geo/gdal.hpp"

#include "gdal-drivers/solid.hpp"

#include "./generatevrtwo.hpp"
#include "./io.hpp"

namespace fs = boost::filesystem;
namespace ba = boost::algorithm;

namespace vrtwo {

namespace {

class NodeIterator {
public:
    NodeIterator(::CPLXMLNode *node, const char *name = nullptr)
        : node_(node->psChild), name_(name)
    {
        // go till node with given name is hit
        while (node_ && !matches()) {
            node_ = node_->psNext;
        }
    }

    operator bool() const { return node_; }
    ::CPLXMLNode* operator*() { return node_; }
    ::CPLXMLNode* operator->() { return node_; }

    NodeIterator& operator++() {
        if (!node_) { return *this; }
        // skip current node and find new with the same name
        do {
            node_ = node_->psNext;
        } while (node_ && !matches());
        return *this;
    }

private:
    bool matches() const {
        return !name_ || !std::strcmp(name_, node_->pszValue);
    }

    ::CPLXMLNode *node_;
    const char *name_;
};

typedef std::shared_ptr< ::CPLXMLNode> XmlNode;
XmlNode xmlNode(const fs::path &path)
{
    auto n(::CPLParseXMLFile(path.c_str()));
    if (!n) {
        LOGTHROW(err1, std::runtime_error)
            << "Cannot parse XML from " << path
            << ": <" << ::CPLGetLastErrorMsg() << ">.";
    }
    return XmlNode(n, [](::CPLXMLNode *n) { ::CPLDestroyXMLNode(n); });
}

typedef std::shared_ptr< ::CPLXMLNode> XmlNode;
XmlNode xmlNodeFromString(const std::string &data)
{
    auto n(::CPLParseXMLString(data.c_str()));
    if (!n) {
        LOGTHROW(err1, std::runtime_error)
            << "Cannot parse XML from a string \"" << data
            << "\": <" << ::CPLGetLastErrorMsg() << ">.";
    }
    return XmlNode(n, [](::CPLXMLNode *n) { ::CPLDestroyXMLNode(n); });
}

typedef std::vector<math::Size2> Sizes;

// dataset mask type
UTILITY_GENERATE_ENUM(MaskType,
    ((none))
    ((nodata))
    ((band))
)

struct Setup {
    math::Size2 size;
    math::Extents2 extents;
    Sizes ovrSizes;
    Sizes ovrTiled;
    int xPlus;
    MaskType maskType;
    fs::path outputDataset;

    Setup() : xPlus(), maskType() {}
};

Setup makeSetup(const geo::GeoDataset::Descriptor &ds
                , const Config &config)
{
    auto size(ds.size);
    auto extents(ds.extents);

    auto halve([&]()
    {
        size.width = int(std::round(size.width / 2.0));
        size.height = int(std::round(size.height / 2.0));
    });

    Setup setup;
    setup.extents = extents;
    setup.size = size;

    // determine mask type
    if (ds.maskType & GMF_ALL_VALID) {
        setup.maskType = MaskType::none;
    } else if (ds.maskType & GMF_NODATA) {
        setup.maskType = MaskType::nodata;
    } else {
        setup.maskType = MaskType::band;
    }

    halve();
    while ((size.width >= config.minOvrSize.width)
           || (size.height >= config.minOvrSize.height))
    {
        setup.ovrSizes.push_back(size);

        if ((size.width == config.minOvrSize.width)
            || (size.height == config.minOvrSize.height))
        {
            // special case
            break;
        }

        halve();
    }

    auto makeTiled([&]()
    {
        const auto &ts(config.tileSize);
        for (const auto &size : setup.ovrSizes) {
            setup.ovrTiled.emplace_back
                ((size.width + ts.width - 1) / ts.width
                 , (size.height + ts.height - 1) / ts.height);
        }
    });

    if (!config.wrapx) {
        makeTiled();
        return setup;
    }

    // add 3 pixel to each side at bottom level and double on the way up
    // 3 pixels because of worst scenario (lanczos filter)
    int add(6);
    for (auto &s : boost::adaptors::reverse(setup.ovrSizes)) {
        s.width += add;
        add *= 2;
    }

    // set x plus component
    setup.xPlus = add / 2;

    // calculate pixel width
    auto es(math::size(setup.extents));
    auto pw(es.width / setup.size.width);

    // calculate addition
    auto eadd(setup.xPlus * pw);

    // apply addition in both dimensions
    setup.extents.ll(0) -= eadd;
    setup.extents.ur(0) += eadd;

    // and finally update size
    setup.size.width += add;

    makeTiled();
    return setup;
}

geo::GeoDataset::Format asVrt(geo::GeoDataset::Format f)
{
    f.storageType = geo::GeoDataset::Format::Storage::vrt;
    return f;
}

struct Rect {
    math::Point2i origin;
    math::Size2 size;

    Rect(const math::Point2i &origin = math::Point2i()
         , const math::Size2 &size = math::Size2())
        : origin(origin), size(size)
    {}

    Rect(const math::Size2 &size) : origin(), size(size) {}
};

typedef boost::optional<Rect> OptionalRect;

struct BandDescriptor {
    fs::path filename;
    int srcBand;
    Rect src;
    Rect dst;
    geo::GeoDataset::BandProperties bp;

    BandDescriptor(const fs::path &filename
                   , const geo::GeoDataset &ds, int srcBand
                   , const OptionalRect &srcRect
                   , const OptionalRect &dstRect)
        : filename(filename), srcBand(srcBand)
        , src(srcRect ? *srcRect : Rect(ds.size()))
        , dst(dstRect ? *dstRect : src)
        , bp(ds.bandProperties(srcBand))
    {}

    void serialize(std::ostream &os, bool mask = false) const;

    typedef std::vector<BandDescriptor> list;
};

class VrtDs {
public:
    VrtDs(const fs::path &path, const geo::SrsDefinition &srs
          , const math::Extents2 &extents, const math::Size2 &size
          , const geo::GeoDataset::Format &format
          , const geo::GeoDataset::NodataValue &nodata
          , MaskType maskType)
        : path_(path.string())
        , ds_(geo::GeoDataset::create
              (path, srs, extents, size, asVrt(format), nodata))
        , bandCount_(format.channels.size())
        , maskType_(maskType), maskBand_()
    {
        if (maskType == MaskType::band) {
            maskBand_ = ds_.createPerDatasetMask<VRTSourcedRasterBand>();
        }
    }

    void flush() {
        // destroy dataset
        ds_ = geo::GeoDataset::placeholder();
    }

    /** NB: band and srcBand are zero-based!
     */
    void addSimpleSource(int band, const fs::path &filename
                         , const geo::GeoDataset &ds
                         , int srcBand
                         , const OptionalRect &srcRect = boost::none
                         , const OptionalRect &dstRect = boost::none);


    void addBackground(const fs::path &path, const Color::optional &color
                       , const boost::optional<fs::path> &localTo
                       = boost::none);

    const geo::GeoDataset& dataset() const { return ds_; }

    std::size_t bandCount() const { return bandCount_; };

private:
    // need to use std::string becase fs::path is non-moveable
    std::string path_;
    geo::GeoDataset ds_;
    std::size_t bandCount_;
    MaskType maskType_;
    VRTSourcedRasterBand *maskBand_;
};

void writeSourceFilename(std::ostream &os, const fs::path &filename
                         , bool shared)
{
    os << "<SourceFilename relativeToVRT=\""
       << int(!filename.is_absolute())
       << " shared=" << int(shared)
       << "\">" << filename.string() << "</SourceFilename>\n"
        ;
}

void writeSourceBand(std::ostream &os, int srcBand, bool mask)
{
    os << "<SourceBand>";
    if (mask) { os << "mask,"; }
    os << (srcBand + 1) << "</SourceBand>\n";
}

void writeRect(std::ostream &os, const char *name, const Rect &r)
{
    os <<  "<" << name << " xOff=\"" << r.origin(0)
       << "\" yOff=\"" << r.origin(1)
       << "\" xSize=\"" << r.size.width
       << "\" ySize=\"" << r.size.height << "\" />";
}

void BandDescriptor::serialize(std::ostream &os, bool mask) const
{
    os << "<SimpleSource>\n";

    writeSourceFilename(os, filename, true);
    writeSourceBand(os, srcBand, mask);

    writeRect(os, "SrcRect", src);
    writeRect(os, "DstRect", dst);

    os << "<SourceProperties RasterXSize=\""<< bp.size.width
       << "\" RasterYSize=\"" << bp.size.height
       << "\" DataType=\"" << bp.dataType
       << "\" BlockXSize=\"" << bp.blockSize.width
       << "\" BlockYSize=\"" << bp.blockSize.height
       << "\" />\n"
        ;

    os << "</SimpleSource>\n"
        ;
}

void VrtDs::addSimpleSource(int band, const fs::path &filename
                            , const geo::GeoDataset &ds
                            , int srcBand
                            , const OptionalRect &srcRect
                            , const OptionalRect &dstRect)
{
    BandDescriptor bd(filename, ds, srcBand, srcRect, dstRect);

    // set source
    {
        std::ostringstream os;
        bd.serialize(os);
        ds_.setMetadata(band + 1, geo::GeoDataset::Metadata("source", os.str())
                        , "new_vrt_sources");
    }

    // only if mask is being used
    if (band || !maskBand_) { return; }

    // add mask simple source
    // serialize as a XML
    std::ostringstream os;
    bd.serialize(os, true);

    // try to create simple source from parsed string
    std::unique_ptr< ::VRTSimpleSource> src(new ::VRTSimpleSource());
#if GDAL_VERSION_NUM >= 3040000
    std::map<CPLString, GDALDataset*> dsMap;
    if (src->XMLInit(xmlNodeFromString(os.str()).get(), nullptr
                     , dsMap) != CE_None)
#elif GDAL_VERSION_NUM >= 3000000
    std::map<CPLString, GDALDataset*> dsMap;
    if (src->XMLInit(xmlNodeFromString(os.str()).get(), nullptr, nullptr
                     , dsMap) != CE_None)
#elif GDAL_VERSION_NUM >= 2040000
    if (src->XMLInit(xmlNodeFromString(os.str()).get(), nullptr, nullptr)
        != CE_None)
#else
    if (src->XMLInit(xmlNodeFromString(os.str()).get(), nullptr) != CE_None)
#endif
    {
        LOGTHROW(err2, std::runtime_error)
            << "Cannot parse VRT source from XML: <"
            << ::CPLGetLastErrorNo() << ", "
            << ::CPLGetLastErrorMsg() << ">.";
    }

    // add source to mask band
    maskBand_->AddSource(src.release());
}

void VrtDs::addBackground(const fs::path &path
                          , const Color::optional &color
                          , const boost::optional<fs::path> &localTo)
{
    if (!color) { return; }

    auto background(*color);
    background.resize(bandCount_);
    const fs::path fname("bg.solid");
    const fs::path bgPath(path / fname);
    const fs::path storePath(localTo ? (*localTo / fname) : bgPath);

    gdal_drivers::SolidDataset::Config cfg;
    cfg.srs = ds_.srs();
    cfg.size = ds_.size();
    cfg.geoTransform(ds_.geoTransform());
    for (std::size_t i(0); i != bandCount_; ++i) {
        const auto bp(ds_.bandProperties(i));

        gdal_drivers::SolidDataset::Config::Band band;
        band.value = background[i];
        band.colorInterpretation = bp.colorInterpretation;
        band.dataType = bp.dataType;
        cfg.bands.push_back(band);
    }

    // create background dataset
    auto bg(geo::GeoDataset::use
            (gdal_drivers::SolidDataset::create(bgPath, cfg)));

    // map layers
    for (std::size_t i(0); i != bandCount_; ++i) {
        addSimpleSource(i, storePath, bg, i);
    }
}

void addOverview(const fs::path &vrtPath, const fs::path &ovrPath)
{

    auto root(xmlNode(vrtPath));

    for (NodeIterator ni(root.get(), "VRTRasterBand"); ni; ++ni) {
        NodeIterator bandNode(*ni, "band");
        if (!bandNode) {
            LOG(warn3) << "Cannot find band attribute in VRTRasterBand.";
            continue;
        }

        // get band number
        auto band(bandNode->psChild->pszValue);

        auto overview(::CPLCreateXMLNode(*ni, ::CXT_Element, "Overview"));
        auto sourceFilename(::CPLCreateXMLNode
                            (overview, ::CXT_Element, "SourceFilename"));
        auto relativeToVRT(::CPLCreateXMLNode
                           (sourceFilename, CXT_Attribute, "relativeToVRT"));
        ::CPLCreateXMLNode(relativeToVRT, CXT_Text
                           , (ovrPath.is_absolute() ? "0" : "1"));
        ::CPLCreateXMLNode(sourceFilename, ::CXT_Text, ovrPath.c_str());
        auto sourceBand(::CPLCreateXMLNode
                        (overview, ::CXT_Element, "SourceBand"));
        ::CPLCreateXMLNode(sourceBand, ::CXT_Text, band);
    }

    auto res(::CPLSerializeXMLTreeToFile(root.get(), vrtPath.c_str()));
    if (!res) {
        LOGTHROW(err3, std::runtime_error)
            << "Cannot save updated VRT file into " << vrtPath << ".";
    }
}

fs::path symlinkSource(const Config &config, const fs::path &path
                       , const fs::path &base)
{
    if (config.pathToOriginalDataset
        == PathToOriginalDataset::absoluteSymlink)
    {
        return fs::absolute(path);
    }

    return utility::lexically_relative(fs::absolute(path)
                                       , fs::absolute(base));
}

Setup buildDatasetBase(const Config &config
                       , const fs::path &input
                       , const fs::path &output)
{
    if (config.pathToOriginalDataset == PathToOriginalDataset::copy) {
        LOGTHROW(err2, std::runtime_error)
            << "Support for dataset copy not implemented yet.";
        // TODO: use dataset->driver->CopyFiles to copy files
    }

    const auto outputDataset(output / "dataset");

    fs::path inputDataset("./original");
    {
        // use original file name for datasets that insist of special name
        const auto des(geo::GeoDataset::open(input).descriptor());
        if (des.driverName == "SRTMHGT") {
            inputDataset = input.filename();
        }
    }

    fs::path inputDatasetSymlink(output / inputDataset);

    LOG(info3) << "Creating dataset base in " << outputDataset
               << " from " << inputDatasetSymlink << ".";

    // make a symlink, remove newpath beforehand
    auto symlink([](const fs::path &oldpath, const fs::path &newpath)
    {
        LOG(info1) << "Linking " << oldpath << " <- " << newpath << ".";
        fs::remove(newpath);
        fs::create_symlink(oldpath, newpath);
    });

    // make symlink to input dataset
    symlink(symlinkSource(config, input, output), inputDatasetSymlink);

    // make symlinks to "sidecar" files
    // FIXME: update for symlink source!
    {
        const auto dir(input.parent_path());
        const auto basename(input.filename().string());
        const auto prefix(basename + ".");

        // temporarily open dataset and grab list of datasets files
        const auto in(geo::GeoDataset::open(input));
        for (const auto &file : in.files()) {
            // get file name
            const auto name(file.filename().string());
            if (ba::starts_with(name, prefix)) {
                const auto ext(name.substr(basename.size()));
                symlink(symlinkSource(config, dir / name, output)
                        , utility::addExtension(inputDatasetSymlink, ext));
            }
        }
    }

    auto in(geo::GeoDataset::open(inputDatasetSymlink));

    const auto ds(in.descriptor());
    auto setup(makeSetup(ds, config));
    setup.outputDataset = outputDataset;

    // remove anything lying in the way of the dataset
    boost::system::error_code ec;
    fs::remove(outputDataset, ec);

    // create virtual output dataset
    VrtDs out(outputDataset, in.srs(), setup.extents
              , setup.size, in.getFormat()
              , (config.nodata ? *config.nodata
                 : in.rawNodataValue())
              , setup.maskType);

    // add input bands
    auto inSize(in.size());
    for (std::size_t i(0); i != in.bandCount(); ++i) {
        if (config.wrapx) {
            // wrapping in x

            // get shift based on pixel overlap
            const auto shift(*config.wrapx);

            // add center section
            Rect centerDst(math::Point2i(setup.xPlus, 0), inSize);
            out.addSimpleSource(i, inputDataset, in, i
                                , boost::none, centerDst);
            math::Size2 strip(math::Size2(setup.xPlus, inSize.height));

            Rect rightSrc
                (math::Point2i(inSize.width - setup.xPlus - shift, 0)
                 , strip);
            Rect leftDst(math::Size2(setup.xPlus, inSize.height));
            out.addSimpleSource(i, inputDataset, in, i, rightSrc, leftDst);

            Rect leftSrc(math::Point2i(shift, 0)
                         , math::Size2(setup.xPlus, inSize.height));
            Rect rightDst(math::Point2i(inSize.width + setup.xPlus, 0)
                              , strip);
            out.addSimpleSource(i, inputDataset, in, i, leftSrc, rightDst);
        } else {
            out.addSimpleSource(i, inputDataset, in, i);
        }
    }

    // done
    out.flush();

    return setup;
}

struct TIDGuard {
    TIDGuard(const std::string &id)
        : old(dbglog::thread_id())
    {
        dbglog::thread_id(id);
    }
    ~TIDGuard() { dbglog::thread_id(old); }

    const std::string old;
};

class Dataset {
public:
    Dataset(const std::string &path)
        : path_(path), ds_(geo::GeoDataset::placeholder())
    {}

    Dataset(const Dataset &d)
        : path_(d.path_), ds_(geo::GeoDataset::open(path_))
    {}

    ~Dataset() {}

    geo::GeoDataset& ds() { return ds_; }

private:
    std::string path_;
    geo::GeoDataset ds_;
};

template <typename T>
bool compareValue(const cv::Mat_<T> &block
                  , const math::Size2 &size
                  , T value)
{
    for (int j(0); j != size.height; ++j) {
        for (int i(0); i != size.width; ++i) {
            if (block(j, i) != value) { return false; }
        }
    }
    return true;
}

bool compare(const geo::GeoDataset::Block &block, const math::Size2 &size
             , ::GDALDataType type, double value)
{
    switch (type) {
    case ::GDT_Byte:
        return compareValue<std::uint8_t>(block.data, size, value);

    case ::GDT_UInt16:
        return compareValue<std::uint16_t>(block.data, size, value);

    case ::GDT_Int16:
        return compareValue<std::int16_t>(block.data, size, value);

    case ::GDT_UInt32:
    case ::GDT_Int32:
        // use signed comparison for unsigned int since OpenCV 4 has no
        // specialization for unsigned int
        return compareValue<std::int32_t>(block.data, size, value);

    case ::GDT_Float32:
        return compareValue<float>(block.data, size, value);

    case ::GDT_Float64:
        return compareValue<double>(block.data, size, value);

    default:
        utility::raise<std::runtime_error>
            ("Unsupported data type <%s>.", type);
    };
    throw;
}

bool emptyTile(const Config &config, const geo::GeoDataset &ds)
{
    if (config.background) {
        // TODO: we are using a background color: need to check content for
        // exact color

        // get background
        int bands(ds.bandCount());
        auto background(*config.background);
        background.resize(bands);

        auto bps(ds.bandProperties());

        // process all blocks
        for (const auto &bi : ds.getBlocking()) {
            for (int i(0); i != bands; ++i) {
                // load block in native format
                auto block(ds.readBlock(bi.offset, i, true));
                if (!compare(block, bi.size, bps[i].dataType, background[i])) {
                    // not single color
                    return false;
                }
            }
        }

        return true;
    }

    // no background -> do not store if mask is empty

    // fetch optimized mask
    auto mask(ds.fetchMask(true));
    // no data -> full area is valid
    if (!mask.data) { return false; }

    // no non-zero count -> empty mask
    return !cv::countNonZero(mask);
}

geo::GeoDataset createTmpDataset(const geo::GeoDataset &src
                                 , const math::Extents2 &extents
                                 , const math::Size2 &size
                                 , MaskType maskType)
{
    // data format
    auto format(src.getFormat());
    format.storageType = geo::GeoDataset::Format::Storage::memory;

    auto nodata(src.rawNodataValue());

    if (maskType == MaskType::band) {
        // internal mask type, derive bigger data type and nodata value
        const auto ds(src.descriptor());

        switch (ds.dataType) {
        // 8 bit -> 16 bits
        case ::GDT_Byte:
            format.channelType = ::GDT_Int16;
            nodata = std::numeric_limits<std::int16_t>::lowest();
            break;

        // 16 bits -> 32 bits
        case ::GDT_UInt16:
        case ::GDT_Int16:
            format.channelType = ::GDT_Int32;
            nodata = std::numeric_limits<std::int32_t>::lowest();
            break;

        // 32 bits -> 64 bits
        case ::GDT_UInt32:
        case ::GDT_Int32:
        case ::GDT_Float32:
            format.channelType = ::GDT_Float64;
            nodata = std::numeric_limits<double>::lowest();
            break;

         // 64 bits -> well, 64 bits + nodata value
        case ::GDT_Float64:
            nodata = std::numeric_limits<double>::lowest();
            break;

        default:
            utility::raise<std::runtime_error>
                ("Unsupported data type <%s>.", ds.dataType);
        }
    }

    // create in-memory temporary dataset dataset
    return geo::GeoDataset::create
        ("MEM", src.srs(), extents, size, format, nodata);
}

void copyWithMask(const geo::GeoDataset &src, geo::GeoDataset &dst)
{
    for (const auto &bi : src.getBlocking()) {
        // copy all data bands
        dst.writeBlock(bi.offset, src.readBlock(bi.offset, true).data);
        // copy mask band
        dst.writeMaskBlock(bi.offset, src.readBlock(bi.offset, -1, true).data);
    }
}

void createOutputDataset(const geo::GeoDataset &original
                         , const geo::GeoDataset &src
                         , const fs::path &path
                         , const geo::Options &createOptions
                         , MaskType maskType)
{
    if (maskType != MaskType::band) {
        // we can copy as is
        UTILITY_OMP(critical(createOutputDataset))
            src.copy(path, "GTiff", createOptions);
        return;
    }

    // we need to create output dataset manually
    auto format(original.getFormat());
    // use custom format to prevent .tfw and .prj creation...
    format.storageType = geo::GeoDataset::Format::Storage::custom;
    format.driver = "GTiff";

    auto dst(geo::GeoDataset::placeholder());

    UTILITY_OMP(critical(createOutputDataset))
        dst = geo::GeoDataset::create(path, src.srs(), src.extents()
                                      , src.size(), format, boost::none
                                      , createOptions);

    copyWithMask(src, dst);
    dst.flush();
}

fs::path createOverview(const Config &config
                        , const boost::filesystem::path &output
                        , int ovrIndex
                        , const fs::path &srcPath
                        , const fs::path &dir
                        , const math::Size2 &size
                        , const math::Size2 &tiled
                        , std::atomic<int> &progress, int total
                        , MaskType maskType)
{
    auto ovrName(dir / "ovr.vrt");
    auto ovrPath(output / ovrName);
    const auto &ts(config.tileSize);

    LOG(info3)
        << "Creating overview #" << ovrIndex
        << " of " << math::area(tiled) << " tiles in "
        << ovrPath << " from " << srcPath << ".";

    // copy options so that the PREDICTOR can be possibly modified
    geo::Options createOptions(config.createOptions);

    VrtDs ovr([&]() -> VrtDs
    {
        auto src(geo::GeoDataset::open(srcPath));

        // If create options contain PREDICTOR, check/set its value based on
        // original dataset type.
        auto &opts(createOptions.options);
        auto it(std::find_if( opts.begin(), opts.end()
                             , [](const geo::Options::Option &op)
                               {
                                    return op.first == "PREDICTOR";
                               }));

        if (it != opts.end()) {
            // find out what the value of predictor should be
            auto predictor([&]() -> std::string {
                switch (src.descriptor().dataType) {
                case ::GDT_Float32:
                case ::GDT_Float64:
                    return "3";
                default:
                    break;
                }
                return "2";
            }());

            // set predictor to optimal
            if (it->second.empty()) {
                it->second = predictor;

            // leave it if predictor is turned off
            } else if (it->second == "1") {

            // if predictor is set, check if the value is right
            } else if (it->second != predictor) {
                LOGTHROW(err2, std::runtime_error)
                    << "PREDICTOR value and bandtype mismatch. Use 2 for "
                    << "integer and 3 for floating point or leave without "
                    << "value to be determined automatically.";
            }
        }

        return VrtDs(ovrPath, src.srs(), src.extents()
                     , size, src.getFormat(), src.rawNodataValue()
                     , maskType);
    }());

    (void) maskType;

    auto extents(ovr.dataset().extents());
    ovr.addBackground(output / dir, config.background, fs::path());


    // compute tile size in real extents
    auto tileSize([&]() -> math::Size2f
    {
        auto es(math::size(extents));
        return math::Size2f((es.width * ts.width) / size.width
                            , (es.height * ts.height) / size.height);
    }());
    // extent's upper-left corner is origin for tile calculations
    math::Point2 origin(ul(extents));

    auto tc(math::area(tiled));

    // last tile size
    math::Size2 lts(size.width - (tiled.width - 1) * ts.width
                    , size.height - (tiled.height - 1) * ts.height);

    // Dataset dataset(srcPath.string());

    // use full dataset and disable safe-chunking
    geo::GeoDataset::WarpOptions warpOptions;
    warpOptions.overview = geo::GeoDataset::Overview();
    warpOptions.safeChunks = false;

    // UTILITY_OMP(parallel for firstprivate(dataset) schedule(dynamic))
    UTILITY_OMP(parallel for schedule(dynamic))
        for (int i = 0; i < tc; ++i) {
            utility::DurationMeter timer;
            math::Point2i tile(i % tiled.width, i / tiled.width);

            bool lastX(tile(0) == (tiled.width - 1));
            bool lastY(tile(1) == (tiled.height - 1));

            math::Size2 pxSize(lastX ? lts.width : ts.width
                               , lastY ? lts.height : ts.height);

            // calculate extents
            math::Point2 ul(origin(0) + tileSize.width * tile(0)
                            , origin(1) - tileSize.height * tile(1));
            math::Point2 lr(lastX ? extents.ur(0) : ul(0) + tileSize.width
                            , lastY ? extents.ll(1): ul(1) - tileSize.height);

            math::Extents2 te(ul(0), lr(1), lr(0), ul(1));
            TIDGuard tg(str(boost::format("tile:%d-%d-%d")
                            % ovrIndex % tile(0) % tile(1)));

            LOG(info2)
                << std::fixed
                << "Processing tile " << ovrIndex
                << '-' << tile(0) << '-' << tile(1) << " (size: " << pxSize
                << ", extents: " << te << ").";

            // try warp
            auto src(geo::GeoDataset::open(srcPath));

            // auto &src(dataset.ds());

            // strore result to file
            fs::path tileName(str(boost::format("%d-%d.tif")
                                  % tile(0) % tile(1)));
            fs::path tilePath(output / dir / tileName);

            auto tmp(createTmpDataset(src, te, pxSize, maskType));

            src.warpInto(tmp, config.resampling, warpOptions);

            // check result and skip if no need to store
            if (emptyTile(config, tmp)) {
                auto id(++progress);
                LOG(info3)
                    << std::fixed
                    << "Processed tile #" << id << '/' << total << ' '
                    << ovrIndex
                    << '-' << tile(0) << '-' << tile(1) << " (size: " << pxSize
                    << ", extents: " << te << ") [empty]"
                    << "; duration: "
                    << utility::formatDuration(timer.duration()) << ".";
                continue;
            }

            // make room for output file
            fs::remove(tilePath);

            createOutputDataset(src, tmp, tilePath
                                , createOptions // use modified options
                                , maskType);

            // store result
            Rect drect(math::Point2i(tile(0) * ts.width, tile(1) * ts.height)
                       , pxSize);

            UTILITY_OMP(critical(createOverwiew_addSimpleSource))
                for (std::size_t b(0), eb(ovr.bandCount()); b != eb; ++b) {
                    ovr.addSimpleSource(b, tileName, tmp, b
                                        , boost::none, drect);
                }

            auto id(++progress);
            LOG(info3)
                << std::fixed
                << "Processed tile #" << id << '/' << total << ' ' << ovrIndex
                << '-' << tile(0) << '-' << tile(1) << " (size: " << pxSize
                << ", extents: " << te << ") [valid]"
                << "; duration: "
                << utility::formatDuration(timer.duration()) << ".";
        }

    ovr.flush();

    return ovrName;
}

} // namespace

/** Generate virtual geodataset with overviews
 */
void generate(const boost::filesystem::path &input
              , const boost::filesystem::path &output
              , const Config &config)
{
    if (!fs::create_directories(output) && !config.overwrite) {
        LOGTHROW(err3, std::runtime_error)
            << "Destination directory already exits. Use --overwrite "
            "to force existing output overwrite.";
    }

    auto setup(buildDatasetBase(config, input, output));

    auto total(std::accumulate(setup.ovrTiled.begin(), setup.ovrTiled.end()
                               , 0, [&](int t, const math::Size2 &tiled)
                               {
                                   return t + math::area(tiled);
                               }));

    LOG(info3) << "About to generate " << setup.ovrSizes.size()
               << " overviews with " << total << " tiles of size "
               << config.tileSize << ".";

    std::atomic<int> progress(0);

    // generate overviews
    fs::path inputPath(setup.outputDataset);
    for (std::size_t i(0); i != setup.ovrSizes.size(); ++i) {
        auto dir(str(boost::format("%d") % i));
        fs::create_directories(output / dir);

        auto path(createOverview
                  (config, output, i, inputPath, dir, setup.ovrSizes[i]
                   , setup.ovrTiled[i], progress, total
                   , setup.maskType));

        // add overview (manually by manipulating the XML)
        addOverview(setup.outputDataset, path);

        // use previous level in the next round
        inputPath = output / path;
    }
}

} // namespace vrtwo
