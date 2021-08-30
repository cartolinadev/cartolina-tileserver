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

#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/thread.hpp>

#include "utility/streams.hpp"
#include "utility/tcpendpoint-io.hpp"
#include "utility/buildsys.hpp"
#include "utility/format.hpp"
#include "service/service.hpp"

#include "gdal-drivers/register.hpp"

#include "vts-libs/registry/po.hpp"
#include "vts-libs/vts/support.hpp"

#include "http/http.hpp"

#include "support/wmts.hpp"

#include "error.hpp"
#include "resourcebackend.hpp"
#include "generator.hpp"
#include "core.hpp"
#include "gdalsupport.hpp"

namespace po = boost::program_options;
namespace fs = boost::filesystem;
namespace ba = boost::algorithm;

namespace vr = vtslibs::registry;
namespace vts = vtslibs::vts;

class Daemon : public service::Service {
public:
    Daemon()
        : service::Service("mapproxy", BUILD_TARGET_VERSION
                           , service::ENABLE_CONFIG_UNRECOGNIZED_OPTIONS
                           | service::ENABLE_UNRECOGNIZED_OPTIONS)
        , httpListen_(3070)
        , httpThreadCount_(boost::thread::hardware_concurrency())
        , httpClientThreadCount_(1)
        , coreThreadCount_(boost::thread::hardware_concurrency())
        , httpEnableBrowser_(false)
    {
        generatorsConfig_.root
            = utility::buildsys::installPath("var/mapproxy/store");
        generatorsConfig_.resourceRoot
            = utility::buildsys::installPath("var/mapproxy/datasets");
        gdalWarperOptions_.processCount
            = boost::thread::hardware_concurrency();
        gdalWarperOptions_.tmpRoot
            = utility::buildsys::installPath("var/mapproxy/tmp");
        generatorsConfig_.resourceUpdatePeriod = 300;

        generatorsConfig_.variables = &variables_;

        variables_ = vts::defaultSupportVars;

        // some file class defaults
        auto &fcs(resourceBackendGenericConfig_.fileClassSettings);
        fcs.setMaxAge(FileClass::config, 60);
        fcs.setMaxAge(FileClass::support, 3600);
        fcs.setMaxAge(FileClass::registry, 3600);
        fcs.setMaxAge(FileClass::data, 604800);
        fcs.setMaxAge(FileClass::unknown, -1);
    }

private:
    void configuration(po::options_description &cmdline
                       , po::options_description &config
                       , po::positional_options_description &pd);

    service::UnrecognizedParser::optional
    configure(const po::variables_map &vars
              , const service::UnrecognizedOptions &unrecognized);

    void configure(const po::variables_map &vars);

    std::vector<std::string> listHelps() const;

    bool help(std::ostream &out, const std::string &what) const;

    bool prePersonaSwitch();

    Service::Cleanup start();

    int run();

    void cleanup();

    virtual bool ctrl(const CtrlCommand &cmd, std::ostream &os);

    virtual void stat(std::ostream &os);

    virtual void monitor(std::ostream &os);

    struct Stopper {
        Stopper(Daemon &d) : d(d) { }
        ~Stopper() { d.cleanup(); }
        Daemon &d;
    };
    friend struct Stopper;

    fs::path resourceRoot_;

    utility::TcpEndpoint httpListen_;
    unsigned int httpThreadCount_;
    unsigned int httpClientThreadCount_;
    unsigned int coreThreadCount_;
    bool httpEnableBrowser_;
    ResourceBackend::GenericConfig resourceBackendGenericConfig_;
    ResourceBackend::TypedConfig resourceBackendConfig_;
    vs::SupportFile::Vars variables_;
    Generators::Config generatorsConfig_;
    GdalWarper::Options gdalWarperOptions_;

    ResourceBackend::pointer resourceBackend_;
    boost::optional<GdalWarper> gdalWarper_;
    boost::optional<Generators> generators_;
    boost::optional<Core> core_;
    boost::optional<http::Http> http_;
};

void Daemon::configuration(po::options_description &cmdline
                            , po::options_description &config
                            , po::positional_options_description &pd)
{
    vr::registryConfiguration(config, vr::defaultPath());

    config.add_options()
        ("store.path", po::value(&generatorsConfig_.root)
         ->default_value(generatorsConfig_.root)->required()
         , "Path to internal store.")

        ("http.listen", po::value(&httpListen_)
         ->default_value(httpListen_)->required()
         , "TCP endpoint where to listen at.")
        ("http.threadCount", po::value(&httpThreadCount_)
         ->default_value(httpThreadCount_)->required()
         , "Number of server HTTP threads.")
        ("http.client.threadCount", po::value(&httpClientThreadCount_)
         ->default_value(httpClientThreadCount_)->required()
         , "Number of client HTTP threads.")
        ("http.enableBrowser", po::value(&httpEnableBrowser_)
         ->default_value(httpEnableBrowser_)->required()
         , "Enables resource browsering functionaly if set to true.")

        ("http.externalUrl", po::value(&generatorsConfig_.externalUrl)
         , "External URL of root of this mapproxy instance. Used only "
         "by services that cannot cope with relive paths (WMTS).")

        ("core.threadCount", po::value(&coreThreadCount_)
         ->default_value(coreThreadCount_)->required()
         , "Number of processing threads.")

        ("gdal.processCount"
         , po::value(&gdalWarperOptions_.processCount)
         ->default_value(gdalWarperOptions_.processCount)->required()
         , "Number of GDAL processes.")
        ("gdal.tmpRoot"
         , po::value(&gdalWarperOptions_.tmpRoot)
         ->default_value(gdalWarperOptions_.tmpRoot)->required()
         , "Root for GDAL temporary stuff (WMTS cache etc.).")
        ("gdal.rssLimit"
         , po::value(&gdalWarperOptions_.rssLimit)
         ->default_value(gdalWarperOptions_.rssLimit)->required()
         , "Real memory limit of all GDAL processes (in MB).")
        ("gdal.rssCheckPeriod"
         , po::value(&gdalWarperOptions_.rssCheckPeriod)
         ->default_value(gdalWarperOptions_.rssCheckPeriod)->required()
         , "Memory check period (in seconds)")

        ("resource-backend.type"
         , po::value(&resourceBackendConfig_.type)->required()
         , ("Resource backend type, possible values: "
            + boost::lexical_cast<std::string>
            (utility::join(ResourceBackend::listTypes(), ", "))
            + ".").c_str())
        ("resource-backend.updatePeriod"
         , po::value(&generatorsConfig_.resourceUpdatePeriod)
         ->default_value(generatorsConfig_.resourceUpdatePeriod)->required()
         , "Update period between resource list update (in seconds).")
        ("resource-backend.root"
         , po::value(&generatorsConfig_.resourceRoot)
         ->default_value(generatorsConfig_.resourceRoot)->required()
         , "Root of datasets defined as relative path.")
        ("resource-backend.freeze"
         , po::value<std::string>()
         ->default_value(utility::concat
                         (utility::join
                          (generatorsConfig_.freezeResourceTypes, ",")))
         ->required()
         , utility::concat
         ("List of resource types that should be immutable once "
          "successfully configured for the first time. Comma-separated list "
          "of resource types (available types: "
          , enumerationString(Resource::Generator::Type{})
          , ").").c_str())
        ("resource-backend.purgeRemoved"
         , po::value(&generatorsConfig_.purgeRemovedResources)
         ->default_value(generatorsConfig_.purgeRemovedResources)->required()
         , "Removed resources are purged from store if true. Use with care.")

        ("vts.builtinBrowserUrl"
         , po::value(&variables_["VTS_BUILTIN_BROWSER_URL"])
         ->default_value(variables_["VTS_BUILTIN_BROWSER_URL"])
         , "URL of built in browser.")

        ("introspection.defaultFov"
         , po::value(&generatorsConfig_.defaultFov)
         ->default_value(generatorsConfig_.defaultFov)->required()
         , "Camera FOV used when no introspection position is provided.")
        ;

        resourceBackendGenericConfig_.fileClassSettings.configuration(config);

    (void) cmdline;
    (void) pd;
}

const std::string RBPrefix("resource-backend");
const std::string RBPrefixDotted(RBPrefix + ".");

service::UnrecognizedParser::optional
Daemon::configure(const po::variables_map &vars
                  , const service::UnrecognizedOptions &unrecognized)
{
    // configure resource backend
    const auto RBType(RBPrefixDotted + "type");
    if (!vars.count(RBType)) { return {}; }
    try {
        // fetch backend type
        resourceBackendConfig_.type = vars[RBType].as<std::string>();
        // and configure
        return ResourceBackend::configure
            (RBPrefixDotted, resourceBackendConfig_, unrecognized);
    } catch (const UnknownResourceBackend&) {
        throw po::validation_error
            (po::validation_error::invalid_option_value, RBType);
    }
}

void Daemon::configure(const po::variables_map &vars)
{
    vr::registryConfigure(vars);

    // prepare generators' configuration
    generatorsConfig_.root = absolute(generatorsConfig_.root);
    generatorsConfig_.resourceRoot = absolute(generatorsConfig_.resourceRoot);
    if (httpEnableBrowser_) {
        generatorsConfig_.fileFlags |= FileFlags::browserEnabled;
    }

    gdalWarperOptions_.tmpRoot = fs::absolute(gdalWarperOptions_.tmpRoot);

    {
        const auto &value(vars["resource-backend.freeze"].as<std::string>());
        std::vector<std::string> parts;
        ba::split(parts, value, ba::is_any_of(", "), ba::token_compress_on);

        generatorsConfig_.freezeResourceTypes.clear();
        for (const auto &part : parts) {
            if (part.empty()) { continue; }
            try {
                generatorsConfig_.freezeResourceTypes.insert
                    (boost::lexical_cast<Resource::Generator::Type>(part));
            } catch (const boost::bad_lexical_cast&) {
                throw po::validation_error
                    (po::validation_error::invalid_option_value, value);
            }
        }
    }

    if (!vars.count("http.externalUrl")) {
        generatorsConfig_.externalUrl
            = utility::format("http://%s/"
                              , utility::TcpEndpointPrettyPrint(httpListen_));
    }

    LOG(info3, log_)
        << "Config:"
        << "\n\tstore.path = " << generatorsConfig_.root
        << "\n\thttp.listen = " << httpListen_
        << "\n\thttp.threadCount = " << httpThreadCount_
        << "\n\thttp.client.threadCount = " << httpClientThreadCount_
        << "\n\thttp.enableBrowser = " << std::boolalpha << httpEnableBrowser_
        << "\n\tcore.threadCount = " << coreThreadCount_
        << "\n\tgdal.processCount = " << gdalWarperOptions_.processCount
        << "\n\tgdal.tmpRoot = " << gdalWarperOptions_.tmpRoot
        << "\n\tresource-backend.updatePeriod = "
        << generatorsConfig_.resourceUpdatePeriod
        << "\n\tresource-backend.root = "
        << generatorsConfig_.resourceRoot
        << "\n\tresource-backend.freeze = ["
        << utility::join(generatorsConfig_.freezeResourceTypes, ",")
        << "]\n"
        << "\n\tresource-backend.purgeRemoved = "
        << generatorsConfig_.purgeRemovedResources << '\n'
        << "\thttp.externalUrl = " << generatorsConfig_.externalUrl << '\n'
        << utility::LManip([&](std::ostream &os) {
                ResourceBackend::printConfig(os, "\t" + RBPrefixDotted
                                             , resourceBackendConfig_);
            })
        ;

    // share same root
    generatorsConfig_.tmpRoot = gdalWarperOptions_.tmpRoot / "generators";
}

const std::string RBHelpPrefix(RBPrefix + "-");

std::vector<std::string> Daemon::listHelps() const
{
    return ResourceBackend::listTypes(RBHelpPrefix);
}

bool Daemon::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        // program help
        out << ("mapproxy daemon\n"
                "\n"
                );

        return true;
    }

    // check for resource backend snippet help
    if (ba::starts_with(what, RBHelpPrefix)) {
        ResourceBackend::TypedConfig config(what.substr(RBHelpPrefix.size()));
        auto parser(ResourceBackend::configure(RBPrefixDotted, config
                                               , {}));
        if (parser) {
            out << parser->options;
            return true;
        }
    }

    return false;
}

bool Daemon::prePersonaSwitch()
{
    return false; // no need to keep saved persona
}

service::Service::Cleanup Daemon::start()
{
    // wmts support
    wmts::prepareTileMatrixSets();

    auto guard(std::make_shared<Stopper>(*this));

    // warper must be first since it uses processes
    gdalWarper_ = boost::in_place(gdalWarperOptions_, std::ref(*this));

    resourceBackend_ = ResourceBackend::create
        (resourceBackendGenericConfig_, resourceBackendConfig_);
    generators_ = boost::in_place(generatorsConfig_, resourceBackend_);

    http_ = boost::in_place();
    http_->serverHeader(utility::format
                        ("%s/%s", utility::buildsys::TargetName
                         , utility::buildsys::TargetVersion));

    http_->startClient(httpClientThreadCount_);

    // starts core + generators
    core_ = boost::in_place(std::ref(*generators_), std::ref(*gdalWarper_)
                            , coreThreadCount_
                            , std::ref(http_->fetcher()));

    http_->listen(httpListen_, std::ref(*core_));
    http_->startServer(httpThreadCount_);

    return guard;
}

void Daemon::cleanup()
{
    // TODO: stop machinery
    // destroy, in reverse order
    http_ = boost::none;
    core_ = boost::none;
    generators_.reset();
    resourceBackend_.reset();

    gdalWarper_ = boost::none;
}

void Daemon::stat(std::ostream &os)
{
    http_->stat(os);
    gdalWarper_->stat(os);
}

void Daemon::monitor(std::ostream &os)
{
    (void) os;
}

inline void sendBoolean(std::ostream &os, bool value)
{
    os << (value ? "true\n" : "false\n");
}

inline bool parseBoolean(const std::string &value)
{
    if (value == "true") {
        return true;
    } else if (value == "false") {
        return false;
    }

    LOGTHROW(err2, std::runtime_error)
        << "Invalid boolean value: <" << value << ">.";
    throw;
}

bool Daemon::ctrl(const CtrlCommand &cmd, std::ostream &os)
{
    if (cmd.cmd == "list-resources") {
        generators_->listResources(os);
        return true;
    } else if (cmd.cmd == "update-resources") {
        auto token(generators_->update());
        os << "resource updater notified\n"
           << token << "\n"
            ;
        return true;

    } else if (cmd.cmd == "updated-since") {
        try {
            switch (cmd.args.size()) {
            case 1:
                sendBoolean(os, generators_->updatedSince
                            (boost::lexical_cast<std::uint64_t>(cmd.args[0])));
                return true;

            case 4:
                sendBoolean
                    (os, generators_->updatedSince
                     (Resource::Id(cmd.args[1], cmd.args[2], cmd.args[3])
                      , boost::lexical_cast<std::uint64_t>(cmd.args[0])));
                return true;

            case 5:
                sendBoolean
                    (os, generators_->updatedSince
                     (Resource::Id(cmd.args[1], cmd.args[2], cmd.args[3])
                      , boost::lexical_cast<std::uint64_t>(cmd.args[0])
                      , parseBoolean(cmd.args[4])));
                return true;

            default:
                os << "error: updated-since expects 1, 4 or 5 argument\n";
                return true;
            }

        } catch (const boost::bad_lexical_cast&) {
            os << "error: argument is not a number\n";
        }
        return true;

    } else if (cmd.cmd == "supports-reference-frame") {
        sendBoolean(os, bool(vr::system.referenceFrames
                             (cmd.args[0], std::nothrow)));
        return true;

    } else if (cmd.cmd == "has-resource") {
        if (cmd.args.size() != 3) {
            os << "error: has-resource expects 3 arguments\n";
            return true;
        }

        sendBoolean(os, generators_->has
                    (Resource::Id(cmd.args[0], cmd.args[1], cmd.args[2])));
        return true;

    } else if (cmd.cmd == "is-resource-ready") {
        if (cmd.args.size() != 3) {
            os << "error: is-resource-ready expects 3 arguments\n";
            return true;
        }

        sendBoolean(os, generators_->isReady
                    (Resource::Id(cmd.args[0], cmd.args[1], cmd.args[2])));
        return true;

    } else if (cmd.cmd == "resource-url") {
        if (cmd.args.size() != 3) {
            os << "error: resource-url expects 3 arguments\n";
            return true;
        }

        os << generators_->url
            (Resource::Id(cmd.args[0], cmd.args[1], cmd.args[2]))
           << '\n';
        return true;

    } else if (cmd.cmd == "help") {
        os << "update-resources  schedule immediate update of resources;\n"
           << "                  returns timestamp (usec from Epoch)\n"
           << "                  that can be used to check for update \n"
           << "                  completion\n"
           << "updated-since timestamp\n"
           << "                  check whether resources have been updated\n"
           << "                  since given timestamp (usec since Epoch)\n"
           << "has-resource referenceFrame group id\n"
           << "                  returns boolean (true/false) indicating\n"
           << "                  resource presence in the delivery table\n"
           << "is-resource-ready referenceFrame group id\n"
           << "                  returns boolean (true/false) indicating\n"
           << "                  resource readiness\n"
           << "resource-url referenceFrame group id\n"
           << "                  returns local resource URL\n"
            ;
        return true;

    }
    return false;
}

int Daemon::run()
{
    try {
        while (Service::isRunning()) {
            gdalWarper_->housekeeping();
            ::usleep(100000);
        }
    } catch (const AbandonAll&) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    ::GDALAllRegister();
    ::OGRRegisterAll();
    gdal_drivers::registerAll();

    return Daemon()(argc, argv);
}
