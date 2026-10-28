#include "ShaderCompileCache.h"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <wrl/client.h>

namespace CSC
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using Microsoft::WRL::ComPtr;

        struct CompileCall
        {
            LPCWSTR fileName;
            const D3D_SHADER_MACRO* defines;
            ID3DInclude* include;
            LPCSTR entrypoint;
            LPCSTR target;
            UINT flags1;
            UINT flags2;
            ID3DBlob** code;
            ID3DBlob** errorMessages;

            HRESULT InvokeWith(const CompileFromFileFn compile) const
            {
                return compile(fileName, defines, include, entrypoint, target, flags1, flags2, code, errorMessages);
            }
        };

        // Every field is length-prefixed, so different field sequences never share material.
        class FingerprintBuilder
        {
        public:
            void Append(const std::span<const std::byte> bytes)
            {
                AppendRaw(bytes.size());
                _material.insert(_material.end(), bytes.begin(), bytes.end());
            }

            void Append(const std::string_view text) { Append(std::as_bytes(std::span(text))); }

            void Append(const std::wstring_view text) { Append(std::as_bytes(std::span(text))); }

            // Distinguishes a null string from an empty one.
            void AppendOptional(const char* text)
            {
                AppendRaw(text != nullptr);
                if (text) {
                    Append(std::string_view(text));
                }
            }

            template <class T>
                requires std::is_trivially_copyable_v<T>
            void AppendRaw(const T value)
            {
                const auto bytes = std::as_bytes(std::span(&value, 1));
                _material.insert(_material.end(), bytes.begin(), bytes.end());
            }

            [[nodiscard]] std::optional<detail::Fingerprint> Finish() const noexcept { return Sha256(_material); }

        private:
            std::vector<std::byte> _material;
        };

        std::string ToUtf8(const std::wstring_view text)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        // Bytecode persisted by another compiler build may differ from what this one produces.
        std::optional<std::string> LoadedCompilerVersion()
        {
            const HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
            std::array<wchar_t, 32'768> path{};
            const DWORD length = compiler ? GetModuleFileNameW(compiler, path.data(), static_cast<DWORD>(path.size())) : 0;
            if (length == 0 || length >= path.size()) {
                return std::nullopt;
            }
            DWORD ignored = 0;
            std::vector<std::byte> info(GetFileVersionInfoSizeW(path.data(), &ignored));
            void* versionData = nullptr;
            UINT versionSize = 0;
            if (info.empty() || !GetFileVersionInfoW(path.data(), 0, static_cast<DWORD>(info.size()), info.data()) ||
                !VerQueryValueW(info.data(), L"\\", &versionData, &versionSize) ||
                versionSize < sizeof(VS_FIXEDFILEINFO)) {
                return std::nullopt;
            }
            const auto* version = static_cast<const VS_FIXEDFILEINFO*>(versionData);
            return std::format("{}.{}.{}.{}", HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
                HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
        }

        // Returns nullopt when the source cannot be read or preprocessed, so the compile must not be cached.
        //
        // Preprocessing through the caller's handler expands exactly the includes and defines the compiler
        // sees, with one gap: the compiler also predefines target-specific macros, so text guarded by them
        // is expanded differently here. The target is part of the fingerprint, so that text can only
        // differ if an include behind such a guard changed on disk.
        std::optional<detail::Fingerprint> FingerprintOf(const CompileCall& call, const std::string_view compilerVersion)
        {
            std::ifstream file(std::filesystem::path(call.fileName), std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            const std::vector<char> source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            if (file.bad()) {
                return std::nullopt;
            }
            // The standard handler resolves relative includes against the source name.
            const std::string sourceName = ToUtf8(call.fileName);
            ComPtr<ID3DBlob> text;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3DPreprocess(source.data(), source.size(), sourceName.c_str(), call.defines, call.include,
                    text.GetAddressOf(), errors.GetAddressOf()))) {
                return std::nullopt;
            }

            FingerprintBuilder builder;
            builder.Append(std::wstring_view(call.fileName));
            builder.Append(std::span(static_cast<const std::byte*>(text->GetBufferPointer()), text->GetBufferSize()));
            // Unused defines leave the text unchanged but can still reach debug information.
            for (const D3D_SHADER_MACRO* define = call.defines; define && define->Name; ++define) {
                builder.AppendOptional(define->Name);
                builder.AppendOptional(define->Definition);
            }
            builder.AppendOptional(nullptr);
            builder.AppendOptional(call.entrypoint);
            builder.AppendOptional(call.target);
            builder.AppendRaw(call.flags1);
            builder.AppendRaw(call.flags2);
            builder.Append(compilerVersion);
            return builder.Finish();
        }

        std::vector<std::byte> BytesOf(ID3DBlob& blob)
        {
            const auto* data = static_cast<const std::byte*>(blob.GetBufferPointer());
            return { data, data + blob.GetBufferSize() };
        }

        HRESULT CreateBlob(const std::span<const std::byte> bytes, ComPtr<ID3DBlob>& blob)
        {
            const HRESULT result = D3DCreateBlob(bytes.size(), &blob);
            if (SUCCEEDED(result)) {
                std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
            }
            return result;
        }

        // Leaves both outputs untouched on failure.
        HRESULT CopyToBlobs(const detail::CachedCompile& cached, ID3DBlob** const code, ID3DBlob** const errorMessages)
        {
            ComPtr<ID3DBlob> codeBlob;
            if (const HRESULT result = CreateBlob(cached.code, codeBlob); FAILED(result)) {
                return result;
            }
            ComPtr<ID3DBlob> errorBlob;
            if (errorMessages && cached.hasErrorMessages) {
                if (const HRESULT result = CreateBlob(cached.errorMessages, errorBlob); FAILED(result)) {
                    return result;
                }
            }
            *code = codeBlob.Detach();
            if (errorMessages) {
                *errorMessages = errorBlob.Detach();
            }
            return S_OK;
        }

        void LogCompile(const std::string_view outcome, const CompileCall& call, const std::chrono::nanoseconds time)
        {
            spdlog::debug("{} {} {} {} in {:.1f} ms", outcome, ToUtf8(call.fileName),
                call.entrypoint ? call.entrypoint : "", call.target ? call.target : "",
                std::chrono::duration<double, std::milli>(time).count());
        }
    }

    std::size_t detail::FingerprintHash::operator()(const Fingerprint& fingerprint) const noexcept
    {
        std::size_t hash;
        std::memcpy(&hash, fingerprint.data(), sizeof(hash));
        return hash;
    }

    ShaderCompileCache::ShaderCompileCache(const CompileFromFileFn compile, std::filesystem::path directory, const bool isValidating) :
        _compile(compile), _directory(std::move(directory)), _isValidating(isValidating), _compilerVersion(LoadedCompilerVersion())
    {
        if (!_compilerVersion) {
            spdlog::warn("cannot read the d3dcompiler_47.dll version; compiles are cached for this session only");
        }
    }

    HRESULT ShaderCompileCache::CompileFromFile(const LPCWSTR fileName, const D3D_SHADER_MACRO* const defines,
        ID3DInclude* const include, const LPCSTR entrypoint, const LPCSTR target, const UINT flags1, const UINT flags2,
        ID3DBlob** const code, ID3DBlob** const errorMessages)
    {
        const CompileCall call{ fileName, defines, include, entrypoint, target, flags1, flags2, code, errorMessages };
        // Without an output there is nothing to serve; the compiler reports the invalid call itself.
        if (!fileName || !code) {
            return call.InvokeWith(_compile);
        }

        const auto fingerprintStart = Clock::now();
        const auto fingerprint = FingerprintOf(call, _compilerVersion.value_or(""));
        const auto lookup = fingerprint ? Find(*fingerprint) : std::nullopt;
        {
            std::scoped_lock lock(_mutex);
            _statistics.fingerprintTime += Clock::now() - fingerprintStart;
        }
        // Requires _mutex.
        const auto countHit = [this, &lookup] {
            ++_statistics.hits;
            if (lookup->isFromDisk) {
                ++_statistics.diskHits;
            }
            _statistics.avoidedCompileTime += lookup->compile.compileTime;
        };
        if (lookup && !_isValidating && SUCCEEDED(CopyToBlobs(lookup->compile, code, errorMessages))) {
            std::scoped_lock lock(_mutex);
            countHit();
            LogCompile(lookup->isFromDisk ? "disk hit" : "hit", call, lookup->compile.compileTime);
            return lookup->compile.result;
        }

        // Diagnostics are captured even when this caller discards them, for later callers that do not.
        ComPtr<ID3DBlob> discardedErrorMessages;
        CompileCall capturingCall = call;
        if (!capturingCall.errorMessages) {
            capturingCall.errorMessages = discardedErrorMessages.GetAddressOf();
        }
        const auto compileStart = Clock::now();
        const HRESULT result = capturingCall.InvokeWith(_compile);
        const std::chrono::nanoseconds compileTime = Clock::now() - compileStart;

        {
            std::scoped_lock lock(_mutex);
            _statistics.compileTime += compileTime;
            if (!fingerprint) {
                ++_statistics.bypasses;
                LogCompile("bypass", call, compileTime);
                return result;
            }
            if (lookup) {
                // Validating: count the hit that would have been served, then check it against the fresh compile.
                countHit();
                const bool isMatch = result == lookup->compile.result && *code && BytesOf(**code) == lookup->compile.code;
                if (!isMatch) {
                    ++_statistics.mismatches;
                    spdlog::warn("cached bytecode differs from a fresh compile of {} {} {}", ToUtf8(fileName),
                        entrypoint ? entrypoint : "", target ? target : "");
                }
                LogCompile(isMatch ? "validated" : "mismatch", call, compileTime);
                return result;
            }
            ++_statistics.misses;
            LogCompile("miss", call, compileTime);
        }
        if (FAILED(result) || !*code) {
            return result;
        }

        ID3DBlob* const capturedErrorMessages = *capturingCall.errorMessages;
        detail::CachedCompile compiled{
            .result = result,
            .code = BytesOf(**code),
            .hasErrorMessages = capturedErrorMessages != nullptr,
            .errorMessages = capturedErrorMessages ? BytesOf(*capturedErrorMessages) : std::vector<std::byte>{},
            .compileTime = compileTime,
        };
        const auto [stored, isInserted] = [&] {
            std::scoped_lock lock(_mutex);
            const auto [entry, isNew] = _compiles.try_emplace(*fingerprint, std::move(compiled));
            return std::pair{ &entry->second, isNew };
        }();
        // When another thread stored the same compile first, it also writes the entry.
        if (isInserted && _compilerVersion && !detail::WriteCompileEntry(EntryPathOf(*fingerprint), *stored)) {
            std::scoped_lock lock(_mutex);
            ++_statistics.failedWrites;
        }
        return result;
    }

    std::optional<ShaderCompileCache::Lookup> ShaderCompileCache::Find(const detail::Fingerprint& fingerprint)
    {
        {
            std::scoped_lock lock(_mutex);
            if (const auto it = _compiles.find(fingerprint); it != _compiles.end()) {
                return Lookup{ it->second, false };
            }
        }
        if (!_compilerVersion) {
            return std::nullopt;
        }
        auto loaded = detail::ReadCompileEntry(EntryPathOf(fingerprint));
        if (!loaded) {
            return std::nullopt;
        }
        std::scoped_lock lock(_mutex);
        return Lookup{ _compiles.try_emplace(fingerprint, std::move(*loaded)).first->second, true };
    }

    std::filesystem::path ShaderCompileCache::EntryPathOf(const detail::Fingerprint& fingerprint) const
    {
        return _directory / (ToHex(fingerprint) + ".cse");
    }

    CompileStatistics ShaderCompileCache::GetStatistics() const
    {
        std::scoped_lock lock(_mutex);
        return _statistics;
    }
}
