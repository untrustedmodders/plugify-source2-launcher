#include <iostream>
#include <filesystem>

#include <plugify/assembly.hpp>
#include <plugify/mem_protector.hpp>
#include <plugify/compat_format.hpp>
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

#include <tier0/logging.h>
#include <appframework/iappsystem.h>

#if S2PLUGIFY_PLATFORM_WINDOWS
#include <windows.h>
#else
std::string GenerateCmdLine(int argc, char* argv[]) {
    std::string cmdLine;
    for (int i = 1; i < argc; ++i) {
        std::format_to(std::back_inserter(cmdLine), "{} ", argv[i]);
    }
    return cmdLine;
}
#endif // S2PLUGIFY_PLATFORM_WINDOWS

using namespace plugify;

template<typename TFunc> requires(std::is_pointer_v<TFunc> && std::is_function_v<std::remove_pointer_t<TFunc>>)
TFunc HookMethod(void* lpVirtualTable, TFunc pHookMethod, ptrdiff_t dwOffset) {
    uintptr_t dwVTable	= *((uintptr_t*)lpVirtualTable);
    uintptr_t dwEntry	= dwVTable + dwOffset;
    uintptr_t dwOrig	= *((uintptr_t*)dwEntry);
    MemProtector memProtector(dwEntry, sizeof(dwEntry), ProtFlag::RWX);
    *((uintptr_t*)dwEntry) = (uintptr_t)pHookMethod;
    return (TFunc) dwOrig;
}

using OnAppSystemLoadedFn = void (*)(IAppSystem*);
using Source2MainFn = int (*)(void* hInstance, void* hPrevInstance, const char *pszCmdLine, int nShowCmd, const char *pszBaseDir, const char *pszGame);

class S2Logger final : public plugify::ILogger {
public:
    explicit S2Logger(const char *name, int flags = 0, LoggingVerbosity_t verbosity = LV_DEFAULT, Color color = UNSPECIFIED_LOGGING_COLOR) {
        m_channelID = LoggingSystem_RegisterLoggingChannel(name, nullptr, flags, verbosity, color);
    }
    ~S2Logger() override = default;

    void Log(std::string_view message, plugify::Severity severity) override {
        if (severity <= m_severity) {
            std::string sMessage = std::format("{}\n", message);

            switch (severity) {
                case plugify::Severity::Fatal:
                    LoggingSystem_Log(m_channelID, LS_ERROR, Color(255, 0, 255, 255), sMessage.c_str());
                    break;
                case plugify::Severity::Error:
                    LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 0, 0, 255), sMessage.c_str());
                    break;
                case plugify::Severity::Warning:
                    LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 127, 0, 255), sMessage.c_str());
                    break;
                case plugify::Severity::Info:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 0, 255), sMessage.c_str());
                    break;
                case plugify::Severity::Debug:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(0, 255, 0, 255), sMessage.c_str());
                    break;
                case plugify::Severity::Verbose:
                    LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 255, 255), sMessage.c_str());
                    break;
                case plugify::Severity::None:
                    break;
            }
        }
    }

    void SetSeverity(plugify::Severity severity) {
        m_severity = severity;
    }

private:
    plugify::Severity m_severity{ plugify::Severity::None };
    LoggingChannelID_t m_channelID;
};

std::shared_ptr<S2Logger> s_logger;
std::shared_ptr<plugify::IPlugify> s_context;

OnAppSystemLoadedFn OnAppSystemLoadedOrig;
void OnAppSystemLoaded(IAppSystem* self) {
    OnAppSystemLoadedOrig(self);

    if (s_context)
        return;

    s_context = plugify::MakePlugify();

    s_logger = std::make_shared<S2Logger>("plugify");
    s_logger->SetSeverity(plugify::Severity::Info);
    s_context->SetLogger(s_logger);

    std::filesystem::path rootDir(Plat_GetGameDirectory(0));
    auto result = s_context->Initialize(rootDir / "csgo");
    if (result) {
        s_logger->SetSeverity(s_context->GetConfig().logSeverity);

        if (auto packageManager = s_context->GetPackageManager().lock()) {
            packageManager->Initialize();

            if (packageManager->HasMissedPackages()) {
                s_logger->Log("Plugin manager has missing packages, run 'update --missing' to resolve issues.", plugify::Severity::Warning);
                return;
            }
            if (packageManager->HasConflictedPackages()) {
                s_logger->Log("Plugin manager has conflicted packages, run 'remove --conflict' to resolve issues.", plugify::Severity::Warning);
                return;
            }
        }

        if (auto pluginManager = s_context->GetPluginManager().lock()) {
            pluginManager->Initialize();
        }
    }
}

#if S2PLUGIFY_PLATFORM_WINDOWS
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow) {
#else
int main(int argc, char *argv[])
#endif
    auto exec_path = std::filesystem::current_path();
    auto engine_path = exec_path / S2PLUGIFY_LIBRARY_PREFIX "engine2" S2PLUGIFY_LIBRARY_SUFFIX;
    auto parent_path = exec_path.generic_string();

    Assembly engine(engine_path, LoadFlag::AlteredSearchPath, {}, true);
    if (!engine) {
        std::cerr << "launcher error" << std::endl;
        return 1;
    }

    auto Source2Main = engine.GetFunctionByName("Source2Main").RCast<Source2MainFn>();

    auto table = engine.GetVirtualTableByName("CMaterialSystem2AppSystemDict");
    OnAppSystemLoadedOrig = HookMethod(&table, &OnAppSystemLoaded, 5 * sizeof(void*));

    int res = 0;
    try {
#if S2PLUGIFY_PLATFORM_WINDOWS
        res = Source2Main(hInstance, hPrevInstance, lpCmdLine, nCmdShow, parent_path.c_str(), S2PLUGIFY_GAME_NAME);
#else
        auto lpCmdLine = GenerateCmdLine(argc, argv);
        res = Source2Main(reinterpret_cast<void*>(1), reinterpret_cast<void*>(0), lpCmdLine.c_str(), 0, parent_path.c_str(), S2PLUGIFY_GAME_NAME);
#endif
    } catch (...) {
        std::exception_ptr p = std::current_exception();
        if (p) {
            try {
                std::rethrow_exception(p);
            } catch (const std::exception& e) {
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
