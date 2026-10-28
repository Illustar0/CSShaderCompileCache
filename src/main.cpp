#include "CompileShaderPatch.h"

namespace
{
    bool isValidating = false;

    bool ReadIsValidating()
    {
        // GetPrivateProfile* resolves a relative path against the Windows directory, not the game's.
        const auto path = std::filesystem::absolute(L"Data/SKSE/Plugins/CSShaderCompileCache.ini");
        return GetPrivateProfileIntW(L"General", L"bValidate", 0, path.c_str()) != 0;
    }

    // SKSE's EventCallback takes a non-const Message*.
    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    void OnSkseMessage(SKSE::MessagingInterface::Message* const message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            // Every plugin, CommunityShaders.dll included, is loaded now; CS compiles only once the
            // renderer starts, which is later.
            try {
                CSC::Install(isValidating);
            } catch (const CSC::InstallRefusal& refusal) {
                SKSE::log::warn("not installed; Community Shaders compiles every shader itself: {}", refusal.what());
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            CSC::LogStatistics();
            break;
        default:
            break;
        }
    }
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* skse)
{
    isValidating = ReadIsValidating();
    // Validation exists to inspect every compile, which is logged at debug level.
    SKSE::Init(skse, { .logLevel = isValidating ? spdlog::level::debug : spdlog::level::info });
    if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE", OnSkseMessage)) {
        SKSE::log::warn("cannot register for SKSE messages; not installed");
    }
    return true;
}
