#include "CompileShaderPatch.h"

#include "Sha256.h"
#include "ShaderCompileCache.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CSC
{
    namespace
    {
        consteval Sha256Digest DigestFromHex(const std::string_view hex)
        {
            const auto nibble = [](const char digit) {
                return digit <= '9' ? digit - '0' : digit - 'a' + 10;
            };
            Sha256Digest digest{};
            for (std::size_t i = 0; i < digest.size(); ++i) {
                digest[i] = static_cast<std::byte>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
            }
            return digest;
        }

        // The CommunityShaders.dll 1.8.0.0 custom build every RVA below was taken from.
        constexpr Sha256Digest kModuleSha256 = DigestFromHex("c6e5c51ecdc808b9b2cecf093be276d30d415769333f2ba80adbb9878c46e178");

        // Util::CompileShader calls D3DCompileFromFile through an import thunk, so redirecting this one
        // call covers every shader CS compiles there without touching its C++ or STL types.
        constexpr std::uintptr_t kCompileCallRva = 0x3FAA36;
        constexpr std::array<std::uint8_t, 5> kCompileCallBytes{ 0xE8, 0x8C, 0x47, 0x15, 0x00 };  // call +0x54F1C7
        constexpr std::uintptr_t kCompileThunkRva = 0x54F1C7;
        constexpr std::array<std::uint8_t, 6> kCompileThunkBytes{ 0xFF, 0x25, 0x4B, 0xAE, 0x05, 0x00 };  // jmp [D3DCompileFromFile]

        // One branch: jmp [rip] followed by its 8-byte target.
        constexpr std::size_t kTrampolineSize = 14;

        // Global because the redirected call lands in a plain function.
        std::optional<ShaderCompileCache> cache;

        HRESULT WINAPI CompileThroughCache(const LPCWSTR fileName, const D3D_SHADER_MACRO* const defines,
            ID3DInclude* const include, const LPCSTR entrypoint, const LPCSTR target, const UINT flags1,
            const UINT flags2, ID3DBlob** const code, ID3DBlob** const errorMessages)
        {
            return cache->CompileFromFile(fileName, defines, include, entrypoint, target, flags1, flags2, code, errorMessages);
        }

        bool HasBytes(const std::uintptr_t address, const std::span<const std::uint8_t> expected) noexcept
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) == 0;
        }

        std::vector<std::byte> ReadModuleFile(const HMODULE communityShaders)
        {
            std::array<wchar_t, 32'768> path{};
            const DWORD length = GetModuleFileNameW(communityShaders, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size()) {
                throw InstallRefusal(std::format("cannot locate CommunityShaders.dll: Win32 error {}", GetLastError()));
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> file(CreateFileW(path.data(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), &CloseHandle);
            LARGE_INTEGER size{};
            if (file.get() == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.get(), &size) ||
                size.QuadPart > (std::numeric_limits<DWORD>::max)()) {
                throw InstallRefusal(std::format("cannot open CommunityShaders.dll: Win32 error {}", GetLastError()));
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
            DWORD bytesRead = 0;
            if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr) ||
                bytesRead != bytes.size()) {
                throw InstallRefusal(std::format("cannot read CommunityShaders.dll: Win32 error {}", GetLastError()));
            }
            return bytes;
        }

        // The loaded image differs from its file through relocations, import binding and other plugins'
        // patches, so the file is hashed. Windows refuses writes to a file mapped as an image, so the
        // file still holds what was loaded.
        void VerifyModuleFile(const HMODULE communityShaders)
        {
            const auto digest = Sha256(ReadModuleFile(communityShaders));
            if (!digest) {
                throw InstallRefusal("cannot hash CommunityShaders.dll to verify its build");
            }
            if (*digest != kModuleSha256) {
                throw InstallRefusal(std::format("unsupported CommunityShaders.dll build (SHA-256 {})", ToHex(*digest)));
            }
        }

        // Returns the call site to redirect; throws InstallRefusal.
        std::uintptr_t LocateCompileCall()
        {
            const HMODULE communityShaders = GetModuleHandleW(L"CommunityShaders.dll");
            if (!communityShaders) {
                throw InstallRefusal("CommunityShaders.dll is not loaded");
            }
            VerifyModuleFile(communityShaders);
            const auto base = reinterpret_cast<std::uintptr_t>(communityShaders);
            if (!HasBytes(base + kCompileCallRva, kCompileCallBytes)) {
                throw InstallRefusal("Util::CompileShader's compiler call was already patched by another plugin");
            }
            if (!HasBytes(base + kCompileThunkRva, kCompileThunkBytes)) {
                throw InstallRefusal("the D3DCompileFromFile import thunk was already patched by another plugin");
            }
            return base + kCompileCallRva;
        }
    }

    void Install(const bool isValidating)
    {
        const std::uintptr_t callSite = LocateCompileCall();
        const auto thunk = reinterpret_cast<CompileFromFileFn>(callSite - kCompileCallRva + kCompileThunkRva);
        // Inside Community Shaders' own disk cache, so its Clear Shader Cache, and the clearing it does
        // when its version changes, remove these entries too.
        cache.emplace(thunk, std::filesystem::absolute(L"Data/ShaderCache/CSShaderCompileCache"), isValidating);

        // SKSE's own trampoline sits near SkyrimSE.exe; rel32 needs one within 2 GiB of CommunityShaders.dll.
        static SKSE::Trampoline trampoline("CSShaderCompileCache");
        trampoline.create(kTrampolineSize, reinterpret_cast<void*>(callSite));
        trampoline.write_call<5>(callSite, &CompileThroughCache);
        SKSE::log::info("redirected Util::CompileShader's compiler call at CommunityShaders.dll+{:X}{}",
            kCompileCallRva, isValidating ? "; validating every cache hit" : "");
    }

    void LogStatistics()
    {
        if (!cache) {
            return;
        }
        const CompileStatistics statistics = cache->GetStatistics();
        using Seconds = std::chrono::duration<double>;
        SKSE::log::info("{} hits ({} from disk), {} misses, {} bypasses, {} mismatches, {} failed writes; "
                        "compiled for {:.1f} s, hits stood for {:.1f} s of compiling, fingerprinted for {:.1f} s",
            statistics.hits, statistics.diskHits, statistics.misses, statistics.bypasses, statistics.mismatches,
            statistics.failedWrites, Seconds(statistics.compileTime).count(),
            Seconds(statistics.avoidedCompileTime).count(), Seconds(statistics.fingerprintTime).count());
    }
}
