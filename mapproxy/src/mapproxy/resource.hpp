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

#ifndef mapproxy_resource_hpp_included_
#define mapproxy_resource_hpp_included_

#include <memory>
#include <iostream>
#include <tuple>

#include <boost/filesystem/path.hpp>
#include <boost/any.hpp>
#include <boost/optional.hpp>

#include "dbglog/dbglog.hpp"

#include "utility/enum-io.hpp"

#include "vts-libs/registry.hpp"
#include "vts-libs/vts/basetypes.hpp"

#include "error.hpp"

#include "support/fileclass.hpp"

// fwd
namespace Json { class Value; }

enum class Changed { yes, no, safely, withRevisionBump };

// fwd
class DefinitionBase;

namespace vr = vtslibs::registry;
namespace vts = vtslibs::vts;

struct DualId {
    std::string id;
    int numId;

    DualId(const std::string &id = "", int numId = 0)
        : id(id), numId(numId)
    {}

    operator std::string() const { return id; }
    operator int() const { return numId; }

    bool operator<(const DualId &o) const {
        return (id < o.id);
    }

    typedef std::set<DualId> set;
};

inline vr::StringIdSet asStringSet(const DualId::set &set)
{
    return vr::StringIdSet(set.begin(), set.end());
}

inline vr::Credits asCredits(const DualId::set &set)
{
    vr::Credits credits;
    for (auto &id : set) {
        credits.set(id, boost::none);
    }
    return credits;
}

inline vr::IdSet asIntSet(const DualId::set &set)
{
    return vr::IdSet(set.begin(), set.end());
}

struct Resource {
    struct Id {
        std::string referenceFrame;
        std::string group;
        std::string id;

        std::string fullId() const { return group + "-" + id; }

        Id() {}
        Id(const std::string &referenceFrame, const std::string &group
           , const std::string &id)
            : referenceFrame(referenceFrame), group(group), id(id) {}

        Id(const std::string &referenceFrame, const Id &id)
            : referenceFrame(referenceFrame), group(id.group), id(id.id) {}
        bool operator<(const Id &o) const;
        bool operator==(const Id &o) const;
        bool operator!=(const Id &o) const;

        typedef std::vector<Id> list;
    };

    typedef boost::optional<Id> OptId;
    typedef std::vector<OptId> OptIds;

    struct Generator {
        enum class Type { tms, surface, geodata };
        Type type;
        std::string driver;

        Generator() {}
        Generator(Type type, const std::string &driver)
            : type(type), driver(driver) {}
        bool operator<(const Generator &o) const;
        bool operator==(const Generator &o) const;
        bool operator!=(const Generator &o) const;

        template <typename Definition> static Generator from() {
            return { Definition::type, Definition::driverName };
        }
    };

    Id id;
    Generator generator;
    std::string comment;

    /** Resource revision. Bumped automatically when resorce definition
     *  comparison says revision bump is needed; max(update, stored) is used.
     */
    unsigned int revision;

    DualId::set credits;

    const vr::ReferenceFrame *referenceFrame;
    vr::LodRange lodRange;
    vr::TileRange tileRange;

    vr::RegistryBase registry;

    FileClassSettings fileClassSettings;

    std::shared_ptr<DefinitionBase> definition() const { return definition_; }

    template <typename T> const T& definition() const;

    void definition(const std::shared_ptr<DefinitionBase> &definition) {
        definition_ = definition;
    }

    typedef std::map<Id, Resource> map;
    typedef std::vector<Resource> list;

    Resource(const FileClassSettings &fileClassSettings)
        : revision(), fileClassSettings(fileClassSettings)
    {}

    Changed changed(const Resource &o) const;

    Id::list needsResources() const;

private:
    /** Definition: based on type and driver, created by resource
     *  parser/generator and interpreted by driver.
     */
    std::shared_ptr<DefinitionBase> definition_;
};

/** Base of all resource definitions.
 */
class DefinitionBase {
public:
    typedef std::shared_ptr<DefinitionBase> pointer;
    virtual ~DefinitionBase() {}

    void from(const Json::Value &value) { from_impl(value); }
    void to(Json::Value &value) const { to_impl(value); }
    Changed changed(const DefinitionBase &other) const {
        return changed_impl(other);
    }

    /** Are credits frozen in the resources's dataset?
     */
    bool frozenCredits() const { return frozenCredits_impl(); }

    /** Returns true if resource needs lod and tile ranges.
     */
    bool needsRanges() const { return needsRanges_impl(); }

    /** Returns list of resources this resource depends on.
     */
    Resource::Id::list needsResources() const { return needsResources_impl(); }

    template <typename T> const T& as() const {
        if (const auto *value = dynamic_cast<const T*>(this)) {
            return *value;
        }
        LOGTHROW(err1, Error)
            << "Incompatible resource definitions: cannot convert <"
            << typeid(*this).name() << "> into <" << typeid(T).name() << ">.";
        throw;
    }

private:
    /** Fills in this definition from given input value.
     */
    virtual void from_impl(const Json::Value &value) = 0;

    /** Fills in this definition into given output value.
     */
    virtual void to_impl(Json::Value &value) const = 0;

    /** Compares this resource definition the other one and check whether they
     *  are basically the same. The definitions can differ but the difference
     *  must not affect resource generation.
     */
    virtual Changed changed_impl(const DefinitionBase &other) const = 0;

    /** If generated dataset freezes credit info into its published data then
     *  credits cannot be changed.
     */
    virtual bool frozenCredits_impl() const { return true; }

    /** Returns true if resource needs lod and tile ranges.
     */
    virtual bool needsRanges_impl() const { return true; }

    /** Returns list of resources this resource depends on.
     */
    virtual Resource::Id::list needsResources_impl() const {
        return {};
    }
};

struct GeneratorInterface {
    typedef Resource::Generator::Type Type;
    enum class Interface : std::uint8_t { vts = 1, terrain = 2, wmts = 3 };

    Type type;
    Interface interface;

    GeneratorInterface(Type type = Type()
                       , Interface interface = Interface::vts)
        : type(type), interface(interface)
    {}

    operator Type() const { return type; }

    GeneratorInterface as(Interface other) const { return { type, other }; }

    bool operator==(const GeneratorInterface &o) const;
    bool operator!=(const GeneratorInterface &o) const;
};

vr::Credits asInlineCredits(const Resource &res);

UTILITY_GENERATE_ENUM_IO(Resource::Generator::Type,
    ((tms))
    ((surface))
    ((geodata))
)

UTILITY_GENERATE_ENUM(RasterFormat,
    ((jpg))
    ((png))
    ((webp))
)

UTILITY_GENERATE_ENUM_IO(GeneratorInterface::Interface,
    ((vts))
    ((terrain))
    ((wmts))
)

std::ostream& operator<<(std::ostream &os, const GeneratorInterface &gi);
std::istream& operator>>(std::istream &is, GeneratorInterface &gi);

constexpr RasterFormat MaskFormat = RasterFormat::png;
constexpr RasterFormat RasterMetatileFormat = RasterFormat::png;
constexpr RasterFormat RasterNormalMapFormat = RasterFormat::webp;

/** Resource root: used to build relative paths from given root.
 */
struct ResourceRoot {
    /** Depth in the virtual filesystem tree.
     *
     * NB: no enum class to allow usage Resource::xxx (as it is used throughout
     * the code.
     */
    enum Depth : int {
        referenceFrame = 0, interface = 1, group = 2, id = 3, none = 4
    };

    /** Root location.
     */
    Depth depth;

    /** How many times to go up the directory tree before adding current root
     *  directory. Each level is one ".."
     */
    int backup;

    ResourceRoot(Depth depth = none, int backup = 0)
        : depth(depth), backup(backup)
    {}

    operator Depth() const { return depth; }

    int depthDifference(ResourceRoot::Depth other) const {
        return depth - other;
    }
};

/** Computes path from this resource to that resource.
 */
ResourceRoot resolveRoot(const Resource::Id &thisResource
                         , const GeneratorInterface &thisGeneratorIface
                         , const Resource::Id &thatResource
                         , const GeneratorInterface &thatGeneratorIface
                         , ResourceRoot::Depth thisDepth = ResourceRoot::none);

/** Computes path from this resource to that resource.
 */
ResourceRoot resolveRoot(const Resource &thisResource
                         , const Resource &thatResource
                         , ResourceRoot::Depth thisDepth = ResourceRoot::none);

/** What directory is resource root:
 */
UTILITY_GENERATE_ENUM_IO(ResourceRoot::Depth,
    ((referenceFrame))
    ((interface))
    ((group))
    ((id))
    ((none))
)

typedef boost::function<void(const Resource::Id&, const std::string&)>
ResourceLoadErrorCallback;

/** Load resources from given path.
 */
Resource::map loadResources(const boost::filesystem::path &path
                            , ResourceLoadErrorCallback error
                            , const FileClassSettings &fileClassSettings
                            = FileClassSettings());

/** Load resources from given path.
 */
Resource::map loadResources(const Json::Value &json
                            , const boost::filesystem::path &path
                            , ResourceLoadErrorCallback error
                            , const FileClassSettings &fileClassSettings
                            = FileClassSettings());

/** Load single resource from given path.
 */
Resource::list loadResource(const boost::filesystem::path &path
                            , const FileClassSettings &fileClassSettings
                            = FileClassSettings());

/** Save single resource to given path.
 */
void save(const boost::filesystem::path &path, const Resource &resource);

/** Writes helper config with include snippets, one per entry in include vector.
 */
void saveIncludeConfig(const boost::filesystem::path &path
                       , const std::vector<std::string> &includes);

boost::filesystem::path prependRoot(const boost::filesystem::path &path
                                    , const Resource &resource
                                    , const ResourceRoot &root);

std::string prependRoot(const std::string &path, const Resource &resource
                        , const ResourceRoot &root);

boost::filesystem::path
prependRoot(const boost::filesystem::path &path
            , const Resource::Id &resource
            , const GeneratorInterface &generatorInterface
            , const ResourceRoot &root);

std::string prependRoot(const std::string &path, const Resource::Id &resource
                        , const GeneratorInterface &generatorIface
                        , const ResourceRoot &root);

std::string contentType(RasterFormat format);

enum class RangeType { lod, tileId };

bool checkRanges(const Resource &resource, const vts::TileId &tileId
                 , RangeType rangeType = RangeType::tileId);

/** Combine resource with given reference frame.
 */
Resource::Id addReferenceFrame(Resource::Id rid, std::string referenceFrame);

// inlines + IO

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
operator<<(std::basic_ostream<CharT, Traits> &os, const Resource::Id &rid)
{
    return os << rid.referenceFrame << '/' << rid.group << '/' << rid.id;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
operator<<(std::basic_ostream<CharT, Traits> &os
           , const Resource::Generator &g)
{
    return os << g.type << '/' << g.driver;
}

inline bool Resource::Id::operator<(const Id &o) const {
    if (referenceFrame < o.referenceFrame) { return true; }
    else if (o.referenceFrame < referenceFrame) { return false; }

    if (group < o.group) { return true; }
    else if (o.group < group) { return false; }

    return id < o.id;
}

inline bool Resource::Id::operator==(const Id &o) const {
    return ((referenceFrame == o.referenceFrame)
            && (group == o.group)
            && (id == o.id));
}

inline bool Resource::Id::operator!=(const Id &o) const {
    return !operator==(o);
}

inline bool Resource::Generator::operator<(const Generator &o) const {
    return std::tie(type, driver) < std::tie(o.type, o.driver);
}

inline bool Resource::Generator::operator==(const Generator &o) const {
    return ((type == o.type)
            && (driver == o.driver));
}

inline bool Resource::Generator::operator!=(const Generator &o) const {
    return !operator==(o);
}

inline Resource::Id addReferenceFrame(Resource::Id rid
                                      , std::string referenceFrame)
{
    rid.referenceFrame = std::move(referenceFrame);
    return rid;
}

inline boost::filesystem::path prependRoot(const boost::filesystem::path &path
                                           , const Resource &resource
                                           , const ResourceRoot &root)
{
    return prependRoot(path, resource.id, resource.generator.type, root);
}

inline std::string prependRoot(const std::string &path
                               , const Resource &resource
                               , const ResourceRoot &root)
{
    return prependRoot(path, resource.id, resource.generator.type, root);
}

inline boost::filesystem::path prependRoot(const boost::filesystem::path &path
                                           , const Resource &resource
                                           , const GeneratorInterface &iface
                                           , const ResourceRoot &root)
{
    return prependRoot(path, resource.id, iface, root);
}

inline std::string prependRoot(const std::string &path
                               , const Resource &resource
                               , const GeneratorInterface &iface
                               , const ResourceRoot &root)
{
    return prependRoot(path, resource.id, iface, root);
}

inline ResourceRoot resolveRoot(const Resource &thisResource
                                , const Resource &thatResource
                                , ResourceRoot::Depth thisDepth)
{
    return resolveRoot(thisResource.id, thisResource.generator.type
                       , thatResource.id, thatResource.generator.type
                       , thisDepth);
}

inline bool GeneratorInterface::operator==(const GeneratorInterface &o) const {
    return (type == o.type) && (interface == o.interface);
}

inline bool GeneratorInterface::operator!=(const GeneratorInterface &o) const {
    return !(*this == o);
}

inline GeneratorInterface::Interface
operator|(GeneratorInterface::Interface l, GeneratorInterface::Interface r)
{
    return static_cast<GeneratorInterface::Interface>
        (static_cast<std::uint8_t>(l) | static_cast<std::uint8_t>(r));
}

inline GeneratorInterface::Interface&
operator|=(GeneratorInterface::Interface &l, GeneratorInterface::Interface r)
{
    return (l = l | r);
}

inline GeneratorInterface::Interface
operator&(GeneratorInterface::Interface l, GeneratorInterface::Interface r)
{
    return static_cast<GeneratorInterface::Interface>
        (static_cast<std::uint8_t>(l)
         & static_cast<std::uint8_t>(r));
}

template <typename T> const T& Resource::definition() const {
    return definition_->as<T>();
}

#endif // mapproxy_resource_hpp_included_
