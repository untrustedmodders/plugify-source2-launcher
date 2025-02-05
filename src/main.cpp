#include <iostream>
#include <filesystem>
#include <unordered_set>

#include <plugify/assembly.hpp>
#include <plugify/compat_format.hpp>
#include <plugify/mem_hook.hpp>
#include <plugify/plugify.hpp>
#include <plugify/plugin.hpp>
#include <plugify/module.hpp>
#include <plugify/plugin_descriptor.hpp>
#include <plugify/plugin_reference_descriptor.hpp>
#include <plugify/language_module_descriptor.hpp>
#include <plugify/package.hpp>
#include <plugify/plugin_manager.hpp>
#include <plugify/package_manager.hpp>
#include <plugify/log.hpp>

#include <eiface.h>
#include <convar.h>
#include <igamesystem.h>
#include <tier0/logging.h>
#include <appframework/iappsystem.h>

#if PLUGIFY_PLATFORM_WINDOWS

#include <windows.h>

#else
std::string GenerateCmdLine(int argc, char* argv[]) {
    std::string cmdLine;
    for (int i = 1; i < argc; ++i) {
        std::format_to(std::back_inserter(cmdLine), "{} ", argv[i]);
    }
    return cmdLine;
}
#endif // PLUGIFY_PLATFORM_WINDOWS

using namespace plugify;

class S2Logger final : public ILogger {
public:
    explicit S2Logger(const char *name, int flags = 0, LoggingVerbosity_t verbosity = LV_DEFAULT, Color color = UNSPECIFIED_LOGGING_COLOR) {
        m_channelID = LoggingSystem_RegisterLoggingChannel(name, nullptr, flags, verbosity, color);
    }

    ~S2Logger() override = default;

    void Log(const char *message, LoggingSeverity_t severity, Color color = UNSPECIFIED_LOGGING_COLOR) const {
        LoggingSystem_Log(m_channelID, severity, color, message);
    }

    void Log(std::string_view message, Severity severity) override {
        if (severity <= m_severity) {
            std::string sMessage = std::format("{}\n", message);

            switch (severity) {
                case Severity::None:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 255, 255), sMessage.c_str());
                    break;
                case Severity::Fatal:
                    LoggingSystem_Log(m_channelID, LS_ERROR, Color(255, 0, 255, 255), sMessage.c_str());
                    break;
                case Severity::Error:
                    LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 0, 0, 255), sMessage.c_str());
                    break;
                case Severity::Warning:
                    LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 127, 0, 255), sMessage.c_str());
                    break;
                case Severity::Info:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 0, 255), sMessage.c_str());
                    break;
                case Severity::Debug:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(0, 255, 0, 255), sMessage.c_str());
                    break;
                case Severity::Verbose:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 255, 255), sMessage.c_str());
                    break;
                default:
                    break;
            }
        }
    }

    void SetSeverity(Severity severity) {
        m_severity = severity;
    }

private:
    Severity m_severity{Severity::None};
    LoggingChannelID_t m_channelID;
};

enum class PlugifyState {
    Wait,
    Load,
    Unload,
    Reload
};

std::shared_ptr<S2Logger> s_logger;
std::shared_ptr<IPlugify> s_context;
PlugifyState s_state;

#define CONPRINT(x) s_logger->Log(x, LS_MESSAGE, Color(255, 255, 0, 255))
#define CONPRINTE(x) s_logger->Log(x, LS_WARNING, Color(255, 0, 0, 255))

namespace {
    template<typename S, typename T, typename F>
    requires(std::is_function_v<F>)
    void Print(std::string &out, const T &t, F &f, std::string_view tab = "  ") {
        out += tab;
        if (t.GetState() != S::Loaded) {
            std::format_to(std::back_inserter(out), "[{:02d}] <{}> {}", t.GetId(), f(t.GetState()),
                           t.GetFriendlyName());
        } else {
            std::format_to(std::back_inserter(out), "[{:02d}] {}", t.GetId(), t.GetFriendlyName());
        }
        auto descriptor = t.GetDescriptor();
        const auto &versionName = descriptor.GetVersionName();
        if (!versionName.empty()) {
            std::format_to(std::back_inserter(out), " ({})", versionName);
        } else {
            std::format_to(std::back_inserter(out), " ({})", descriptor.GetVersion());
        }
        const auto &createdBy = descriptor.GetCreatedBy();
        if (!createdBy.empty()) {
            std::format_to(std::back_inserter(out), " by {}", createdBy);
        }
        out += '\n';
    }

    template<typename S, typename T, typename F>
    requires(std::is_function_v<F>)
    void Print(std::string &out, const char *name, const T &t, F &f) {
        if (t.GetState() == S::Error) {
            std::format_to(std::back_inserter(out), "{} has error: {}.\n", name, t.GetError());
        } else {
            std::format_to(std::back_inserter(out), "{} {} is {}.\n", name, t.GetId(), f(t.GetState()));
        }
        auto descriptor = t.GetDescriptor();
        const auto &getCreatedBy = descriptor.GetCreatedBy();
        if (!getCreatedBy.empty()) {
            std::format_to(std::back_inserter(out), "  Name: \"{}\" by {}\n", t.GetFriendlyName(), getCreatedBy);
        } else {
            std::format_to(std::back_inserter(out), "  Name: \"{}\"\n", t.GetFriendlyName());
        }
        const auto &versionName = descriptor.GetVersionName();
        if (!versionName.empty()) {
            std::format_to(std::back_inserter(out), "  Version: {}\n", versionName);
        } else {
            std::format_to(std::back_inserter(out), "  Version: {}\n", descriptor.GetVersion());
        }
        const auto &description = descriptor.GetDescription();
        if (!description.empty()) {
            std::format_to(std::back_inserter(out), "  Description: {}\n", description);
        }
        const auto &createdByURL = descriptor.GetCreatedByURL();
        if (!createdByURL.empty()) {
            std::format_to(std::back_inserter(out), "  URL: {}\n", createdByURL);
        }
        const auto &docsURL = descriptor.GetDocsURL();
        if (!docsURL.empty()) {
            std::format_to(std::back_inserter(out), "  Docs: {}\n", docsURL);
        }
        const auto &downloadURL = descriptor.GetDownloadURL();
        if (!downloadURL.empty()) {
            std::format_to(std::back_inserter(out), "  Download: {}\n", downloadURL);
        }
        const auto &updateURL = descriptor.GetUpdateURL();
        if (!updateURL.empty()) {
            std::format_to(std::back_inserter(out), "  Update: {}\n", updateURL);
        }
    }

    ptrdiff_t FormatInt(const std::string &str) {
        try {
            size_t pos;
            ptrdiff_t result = std::stoul(str, &pos);
            if (pos != str.length()) {
                throw std::invalid_argument("Trailing characters after the valid part");
            }
            return result;
        }
        catch (const std::invalid_argument &e) {
            CONPRINTE(std::format("Invalid argument: {}\n", e.what()).c_str());
        }
        catch (const std::out_of_range &e) {
            CONPRINTE(std::format("Out of range: {}\n", e.what()).c_str());
        }
        catch (const std::exception &e) {
            CONPRINTE(std::format("Conversion error: {}\n", e.what()).c_str());
        }

        return ptrdiff_t(-1);
    }
}

CON_COMMAND_F(plugify, "Plugify control options", FCVAR_NONE) {
    std::vector<std::string> arguments;
    std::unordered_set<std::string> options;
    std::span view(args.ArgV(), args.ArgC());
    arguments.reserve(view.size());
    for (size_t i = 0; i < view.size(); i++) {
        std::string str(view[i]);
        if (i > 1 && str.starts_with("-")) {
            options.emplace(std::move(str));
        } else {
            arguments.emplace_back(std::move(str));
        }
    }

    auto& plugify = s_context;
    if (!plugify)
        return;// Should not trigger!

    auto packageManager = plugify->GetPackageManager().lock();
    auto pluginManager = plugify->GetPluginManager().lock();
    if (!packageManager || !pluginManager)
        return;// Should not trigger!

    if (arguments.size() > 1) {
        if (arguments[1] == "help" || arguments[1] == "-h") {
            CONPRINT("Plugify Menu\n"
                     "(c) untrustedmodders\n"
                     "https://github.com/untrustedmodders\n"
                     "usage: plg <command> [options] [arguments]\n"
                     "  help           - Show help\n"
                     "  version        - Version information\n"
                     "Plugin Manager commands:\n"
                     "  load           - Load plugin manager\n"
                     "  unload         - Unload plugin manager\n"
                     "  reload         - Reload plugin manager\n"
                     "  modules        - List running modules\n"
                     "  plugins        - List running plugins\n"
                     "  plugin <name>  - Show information about a module\n"
                     "  module <name>  - Show information about a plugin\n"
                     "Plugin Manager options:\n"
                     "  -h, --help     - Show help\n"
                     "  -u, --uuid     - Use index instead of name\n"
                     "Package Manager commands:\n"
                     "  install <name> - Packages to install (space separated)\n"
                     "  remove <name>  - Packages to remove (space separated)\n"
                     "  update <name>  - Packages to update (space separated)\n"
                     "  list           - Print all local packages\n"
                     "  query          - Print all remote packages\n"
                     "  show  <name>   - Show information about local package\n"
                     "  search <name>  - Search information about remote package\n"
                     "  snapshot       - Snapshot packages into manifest\n"
                     "  repo <url>     - Add repository to config\n"
                     "Package Manager options:\n"
                     "  -h, --help     - Show help\n"
                     "  -a, --all      - Install/remove/update all packages\n"
                     "  -f, --file     - Packages to install (from file manifest)\n"
                     "  -l, --link     - Packages to install (from HTTP manifest)\n"
                     "  -m, --missing  - Install missing packages\n"
                     "  -c, --conflict - Remove conflict packages\n"
                     "  -i, --ignore   - Ignore missing or conflict packages\n");
        }

        else if (arguments[1] == "version" || arguments[1] == "-v") {
            CONPRINT(R"(      ____)" "\n"
                     R"( ____|    \         Plugify )" PLUGIFY_PROJECT_VERSION "\n"
                     R"((____|     `._____  )" "Copyright (C) 2023-" PLUGIFY_PROJECT_YEAR " Untrusted Modders Team\n"
                     R"( ____|       _|___)" "\n"
                     R"((____|     .'       This program may be freely redistributed under)" "\n"
                     R"(     |____/         the terms of the GNU General Public License.)" "\n");
        }

        else if (arguments[1] == "load") {
            packageManager->Reload();
            if (!options.contains("--ignore") && !options.contains("-i")) {
                if (packageManager->HasMissedPackages()) {
                    CONPRINTE("Plugin manager has missing packages, run 'install --missing' to resolve issues.\n");
                    return;
                }
                if (packageManager->HasConflictedPackages()) {
                    CONPRINTE("Plugin manager has conflicted packages, run 'remove --conflict' to resolve issues.\n");
                    return;
                }
            }
            if (pluginManager->IsInitialized()) {
                CONPRINTE("Plugin manager already loaded.\n");
            } else {
                s_state = PlugifyState::Load;
            }
        }

        else if (arguments[1] == "unload") {
            if (!pluginManager->IsInitialized()) {
                CONPRINTE("Plugin manager already unloaded.\n");
            } else {
                s_state = PlugifyState::Unload;
            }
        }

        else if (arguments[1] == "reload") {
            if (!pluginManager->IsInitialized()) {
                CONPRINTE("Plugin manager not loaded.");
                packageManager->Reload();
            } else {
                s_state = PlugifyState::Reload;
            }
        }

        else if (arguments[1] == "plugins") {
            if (!pluginManager->IsInitialized()) {
                CONPRINTE("You must load plugin manager before query any information from it.\n");
                return;
            }

            auto count = pluginManager->GetPlugins().size();
            std::string sMessage = count ? std::format("Listing {} plugin{}:\n", count, (count > 1) ? "s" : "") : std::string("No plugins loaded.\n");

            for (const auto& plugin: pluginManager->GetPlugins()) {
                Print<plugify::PluginState>(sMessage, plugin, plugify::PluginUtils::ToString);
            }

            CONPRINT(sMessage.c_str());
        }

        else if (arguments[1] == "modules") {
            if (!pluginManager->IsInitialized()) {
                CONPRINTE("You must load plugin manager before query any information from it.\n");
                return;
            }
            auto count = pluginManager->GetModules().size();
            std::string sMessage = count ? std::format("Listing {} module{}:\n", count, (count > 1) ? "s" : "") : std::string("No modules loaded.\n");
            for (const auto& module: pluginManager->GetModules()) {
                Print<plugify::ModuleState>(sMessage, module, plugify::ModuleUtils::ToString);
            }

            CONPRINT(sMessage.c_str());
        }

        else if (arguments[1] == "plugin") {
            if (arguments.size() > 2) {
                if (!pluginManager->IsInitialized()) {
                    CONPRINTE("You must load plugin manager before query any information from it.\n");
                    return;
                }
                auto plugin = options.contains("--uuid") || options.contains("-u") ? pluginManager->FindPluginFromId(FormatInt(arguments[2])) : pluginManager->FindPlugin(arguments[2]);
                if (plugin) {
                    std::string sMessage;
                    Print<plugify::PluginState>(sMessage, "Plugin", plugin, plugify::PluginUtils::ToString);
                    auto descriptor = plugin.GetDescriptor();
                    std::format_to(std::back_inserter(sMessage), "  Language module: {}\n", descriptor.GetLanguageModule());
                    sMessage += "  Dependencies: \n";
                    for (const auto& reference: descriptor.GetDependencies()) {
                        auto dependency = pluginManager->FindPlugin(reference.GetName());
                        if (dependency) {
                            Print<plugify::PluginState>(sMessage, dependency, plugify::PluginUtils::ToString, "    ");
                        } else {
                            std::format_to(std::back_inserter(sMessage), "    {} <Missing> (v{})", reference.GetName(), reference.GetRequestedVersion().has_value() ? reference.GetRequestedVersion()->to_string() : "[latest]");
                        }
                    }
                    std::format_to(std::back_inserter(sMessage), "  File: {}\n\n", descriptor.GetEntryPoint());

                    CONPRINT(sMessage.c_str());
                } else {
                    CONPRINTE(std::format("Plugin {} not found.\n", arguments[2]).c_str());
                }
            } else {
                CONPRINTE("You must provide name.\n");
            }
        }

        else if (arguments[1] == "module") {
            if (arguments.size() > 2) {
                if (!pluginManager->IsInitialized()) {
                    CONPRINTE("You must load plugin manager before query any information from it.");
                    return;
                }
                auto module = options.contains("--uuid") || options.contains("-u") ? pluginManager->FindModuleFromId(FormatInt(arguments[2])) : pluginManager->FindModule(arguments[2]);
                if (module) {
                    std::string sMessage;

                    Print<plugify::ModuleState>(sMessage, "Module", module, plugify::ModuleUtils::ToString);
                    std::format_to(std::back_inserter(sMessage), "  Language: {}\n", module.GetLanguage());
                    std::format_to(std::back_inserter(sMessage), "  File: {}\n\n", std::filesystem::path(module.GetFilePath()).string());

                    CONPRINT(sMessage.c_str());
                } else {
                    CONPRINTE(std::format("Module {} not found.\n", arguments[2]).c_str());
                }
            } else {
                CONPRINTE("You must provide name.\n");
            }
        }

        else if (arguments[1] == "snapshot") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            packageManager->SnapshotPackages(plugify->GetConfig().baseDir / std::format("snapshot_{}.wpackagemanifest", DateTime::Get("%Y_%m_%d_%H_%M_%S")), true);
        }

        else if (arguments[1] == "repo") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }

            if (arguments.size() > 2) {
                bool success = false;
                for (const auto& repository: std::span(arguments.begin() + 2, arguments.size() - 2)) {
                    success |= plugify->AddRepository(repository);
                }
                if (success) {
                    packageManager->Reload();
                }
            } else {
                CONPRINTE("You must give at least one repository to add.\n");
            }
        }

        else if (arguments[1] == "install") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            if (options.contains("--missing") || options.contains("-m")) {
                if (packageManager->HasMissedPackages()) {
                    packageManager->InstallMissedPackages();
                } else {
                    CONPRINT("No missing packages were found.\n");
                }
            } else {
                if (arguments.size() > 2) {
                    if (options.contains("--link") || options.contains("-l")) {
                        packageManager->InstallAllPackages(arguments[2], arguments.size() > 3);
                    } else if (options.contains("--file") || options.contains("-f")) {
                        packageManager->InstallAllPackages(std::filesystem::path{arguments[2]}, arguments.size() > 3);
                    } else {
                        packageManager->InstallPackages(std::span(arguments.begin() + 2, arguments.size() - 2));
                    }
                } else {
                    CONPRINTE("You must give at least one requirement to install.\n");
                }
            }
        }

        else if (arguments[1] == "remove") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            if (options.contains("--all") || options.contains("-a")) {
                packageManager->UninstallAllPackages();
            } else if (options.contains("--conflict") || options.contains("-c")) {
                if (packageManager->HasConflictedPackages()) {
                    packageManager->UninstallConflictedPackages();
                } else {
                    CONPRINT("No conflicted packages were found.\n");
                }
            } else {
                if (arguments.size() > 2) {
                    packageManager->UninstallPackages(std::span(arguments.begin() + 2, arguments.size() - 2));
                } else {
                    CONPRINTE("You must give at least one requirement to remove.\n");
                }
            }
        }

        else if (arguments[1] == "update") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            if (options.contains("--all") || options.contains("-a")) {
                packageManager->UpdateAllPackages();
            } else {
                if (arguments.size() > 2) {
                    packageManager->UpdatePackages(std::span(arguments.begin() + 2, arguments.size() - 2));
                } else {
                    CONPRINTE("You must give at least one requirement to update.\n");
                }
            }
        }

        else if (arguments[1] == "list") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            auto localPackages = packageManager->GetLocalPackages();
            auto count = localPackages.size();
            if (!count) {
                CONPRINTE("No local packages found.\n");
            } else {
                CONPRINT(std::format("Listing {} local package{}:\n", count, (count > 1) ? "s" : "").c_str());
            }
            for (const auto& localPackage: localPackages) {
                CONPRINT(std::format("  {} [{}] (v{}) at {}\n", localPackage->name, localPackage->type, localPackage->version, localPackage->path.string()).c_str());
            }
        }

        else if (arguments[1] == "query") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            auto remotePackages = packageManager->GetRemotePackages();
            auto count = packageManager->GetRemotePackages().size();
            std::string sMessage = count ? std::format("Listing {} remote package{}:\n", count, (count > 1) ? "s" : "") : std::string("No remote packages found.\n");
            for (const auto& remotePackage: remotePackages) {
                if (remotePackage->author.empty() || remotePackage->description.empty()) {
                    std::format_to(std::back_inserter(sMessage), "  {} [{}]\n", remotePackage->name, remotePackage->type);
                } else {
                    std::format_to(std::back_inserter(sMessage), "  {} [{}] ({}) by {}\n", remotePackage->name, remotePackage->type, remotePackage->description, remotePackage->author);
                }
            }

            CONPRINT(sMessage.c_str());
        }

        else if (arguments[1] == "show") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            if (arguments.size() > 2) {
                auto package = packageManager->FindLocalPackage(arguments[2]);
                if (package) {
                    CONPRINT(std::format("  Name: {}\n"
                                         "  Type: {}\n"
                                         "  Version: {}\n"
                                         "  File: {}\n\n",
                                         package->name, package->type, package->version, package->path.string())
                                     .c_str());
                } else {
                    CONPRINTE(std::format("Package {} not found.\n", arguments[2]).c_str());
                }
            } else {
                CONPRINTE("You must provide name.\n");
            }
        }

        else if (arguments[1] == "search") {
            if (pluginManager->IsInitialized()) {
                CONPRINTE("You must unload plugin manager before bring any change with package manager.\n");
                return;
            }
            if (arguments.size() > 2) {
                auto package = packageManager->FindRemotePackage(arguments[2]);
                if (package) {
                    std::string sMessage;

                    std::format_to(std::back_inserter(sMessage), "  Name: {}\n", package->name);
                    std::format_to(std::back_inserter(sMessage), "  Type: {}\n", package->type);
                    if (!package->author.empty()) {
                        std::format_to(std::back_inserter(sMessage), "  Author: {}\n", package->author);
                    }
                    if (!package->description.empty()) {
                        std::format_to(std::back_inserter(sMessage), "  Description: {}\n", package->description);
                    }
                    const auto& versions = package->versions;
                    if (!versions.empty()) {
                        std::string combined("  Versions: ");
                        std::format_to(std::back_inserter(combined), "{}", versions.begin()->version);
                        for (auto it = std::next(versions.begin()); it != versions.end(); ++it) {
                            std::format_to(std::back_inserter(combined), ", {}", it->version);
                        }
                        std::format_to(std::back_inserter(combined), "\n\n");
                        sMessage += combined;

                        CONPRINT(sMessage.c_str());
                    } else {
                        CONPRINT("\n");
                    }
                } else {
                    CONPRINTE(std::format("Package {} not found.\n", arguments[2]).c_str());
                }
            } else {
                CONPRINTE("You must provide name.\n");
            }
        }

        else {
            std::string sMessage = std::format("unknown option: {}\n", arguments[1]);
            sMessage += "usage: plugify <command> [options] [arguments]\n"
                        "Try plugify help or -h for more information.\n";

            CONPRINTE(sMessage.c_str());
        }
    } else {
        CONPRINTE("usage: plg <command> [options] [arguments]\n"
                  "Try plg help or -h for more information.\n");
    }
}

static ConCommand plg_command("plg", plugify_callback, "Plugify control options", 0);

using ServerGamePostSimulateFn = void (*)(IGameSystem *, const EventServerGamePostSimulate_t &);
using OnAppSystemLoadedFn = void (*)(CAppSystemDict *);
using Source2MainFn = int (*)(void *hInstance, void *hPrevInstance, const char *pszCmdLine, int nShowCmd,
                              const char *pszBaseDir, const char *pszGame);

ServerGamePostSimulateFn _ServerGamePostSimulate;

void ServerGamePostSimulate(IGameSystem *pThis, const EventServerGamePostSimulate_t &msg) {
    _ServerGamePostSimulate(pThis, msg);

    s_context->Update();

    switch (s_state) {
        case PlugifyState::Load: {
            auto pluginManager = s_context->GetPluginManager().lock();
            if (!pluginManager) {
                s_state = PlugifyState::Wait;
                return;
            }

            pluginManager->Initialize();
            CONPRINT("Plugin manager was loaded.\n");
            break;
        }
        case PlugifyState::Unload: {
            auto pluginManager = s_context->GetPluginManager().lock();
            if (!pluginManager) {
                s_state = PlugifyState::Wait;
                return;
            }

            pluginManager->Terminate();
            CONPRINT("Plugin manager was unloaded.\n");

            if (auto packageManager = s_context->GetPackageManager().lock()) {
                packageManager->Reload();
            }
            break;
        }
        case PlugifyState::Reload: {
            auto pluginManager = s_context->GetPluginManager().lock();
            if (!pluginManager) {
                s_state = PlugifyState::Wait;
                return;
            }

            pluginManager->Terminate();

            if (auto packageManager = s_context->GetPackageManager().lock()) {
                packageManager->Reload();
            }

            pluginManager->Initialize();
            CONPRINT("Plugin manager was reloaded.");
            break;
        }
        case PlugifyState::Wait:
            return;
    }

    s_state = PlugifyState::Wait;
}

OnAppSystemLoadedFn _OnAppSystemLoaded;

void OnAppSystemLoaded(CAppSystemDict *pThis) {
    _OnAppSystemLoaded(pThis);

    if (s_context)
        return;

    if (g_pCVar == nullptr) {
        Assembly tier0(PLUGIFY_LIBRARY_PREFIX "tier0" PLUGIFY_LIBRARY_SUFFIX);
        if (tier0) {
            auto CreateInterface = tier0.GetFunctionByName("CreateInterface").RCast<CreateInterfaceFn>();
            g_pCVar = reinterpret_cast<ICvar *>(CreateInterface(CVAR_INTERFACE_VERSION, nullptr));
        } else {
            return;
        }
    }

    if (_ServerGamePostSimulate == nullptr) {
        Assembly server(PLUGIFY_LIBRARY_PREFIX "server" PLUGIFY_LIBRARY_SUFFIX, {}, {}, true);
        if (server) {
            auto table = server.GetVirtualTableByName("CLightQueryGameSystem");
            int offset = GetVirtualTableIndex(&IGameSystem::ServerGamePostSimulate);
            _ServerGamePostSimulate = HookMethod(&table, &ServerGamePostSimulate, offset);
        } else {
            return;
        }
    }

    ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);

    s_context = MakePlugify();

    s_logger = std::make_shared<S2Logger>("plugify");
    s_logger->SetSeverity(Severity::Info);
    s_context->SetLogger(s_logger);

    std::filesystem::path rootDir(Plat_GetGameDirectory());
    auto result = s_context->Initialize(rootDir / "csgo");
    if (result) {
        s_logger->SetSeverity(s_context->GetConfig().logSeverity.value_or(Severity::Debug));

        if (auto packageManager = s_context->GetPackageManager().lock()) {
            packageManager->Initialize();

            if (packageManager->HasMissedPackages()) {
                CONPRINTE("Plugin manager has missing packages, run 'update --missing' to resolve issues.");
                return;
            }
            if (packageManager->HasConflictedPackages()) {
                CONPRINTE("Plugin manager has conflicted packages, run 'remove --conflict' to resolve issues.");
                return;
            }
        }

        if (auto pluginManager = s_context->GetPluginManager().lock()) {
            pluginManager->Initialize();
        }
    }
}

#if PLUGIFY_PLATFORM_WINDOWS

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char *argv[])
#endif
{
    auto exec_path = std::filesystem::current_path();
    auto engine_path = exec_path / PLUGIFY_LIBRARY_PREFIX "engine2" PLUGIFY_LIBRARY_SUFFIX;
    auto parent_path = exec_path.generic_string();

    Assembly engine(engine_path, LoadFlag::AlteredSearchPath, {}, true);
    if (!engine) {
        std::cerr << "launcher error: " << engine.GetError() << std::endl;
        return 1;
    }

    auto table = engine.GetVirtualTableByName("CMaterialSystem2AppSystemDict");
    int offset = GetVirtualTableIndex(&CAppSystemDict::OnAppSystemLoaded);
    _OnAppSystemLoaded = HookMethod(&table, &OnAppSystemLoaded, offset);

    auto Source2Main = engine.GetFunctionByName("Source2Main").RCast<Source2MainFn>();

    int res = 0;
    try {
#if PLUGIFY_PLATFORM_WINDOWS
        res = Source2Main(hInstance, hPrevInstance, lpCmdLine, nCmdShow, parent_path.c_str(), PLUGIFY_GAME_NAME);
#else
        auto lpCmdLine = GenerateCmdLine(argc, argv);
        res = Source2Main(reinterpret_cast<void*>(1), reinterpret_cast<void*>(0), lpCmdLine.c_str(), 0, parent_path.c_str(), PLUGIFY_GAME_NAME);
#endif
    } catch (...) {
        std::exception_ptr p = std::current_exception();
        if (p) {
            try {
                std::rethrow_exception(p);
            } catch (const std::exception &e) {
                std::clog << typeid(e).name() << ": " << e.what() << std::endl;
            } catch (...) {
                std::clog << "unknown exception" << std::endl;
            }
        } else {
            std::clog << "null" << std::endl;
        }
    }

    s_context.reset();
    s_logger.reset();

    return res;
}
