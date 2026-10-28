#pragma once

#include "CompileEntryFile.h"
#include "Sha256.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <Windows.h>
#include <d3dcompiler.h>

namespace CSC
{
    using CompileFromFileFn = HRESULT(WINAPI*)(LPCWSTR fileName, const D3D_SHADER_MACRO* defines,
        ID3DInclude* include, LPCSTR entrypoint, LPCSTR target, UINT flags1, UINT flags2,
        ID3DBlob** code, ID3DBlob** errorMessages);

    struct CompileStatistics
    {
        std::uint32_t hits = 0;
        // Hits that had to read their entry from disk first; included in hits.
        std::uint32_t diskHits = 0;
        std::uint32_t misses = 0;
        // Compiles whose inputs could not be fingerprinted and went straight to the compiler.
        std::uint32_t bypasses = 0;
        // Validating hits whose fresh bytecode differed from the cached bytecode.
        std::uint32_t mismatches = 0;
        std::uint32_t failedWrites = 0;
        std::chrono::nanoseconds compileTime{};
        std::chrono::nanoseconds avoidedCompileTime{};
        std::chrono::nanoseconds fingerprintTime{};
    };

    namespace detail
    {
        // SHA-256 of every compile input; see ShaderCompileCache.
        using Fingerprint = Sha256Digest;

        struct FingerprintHash
        {
            std::size_t operator()(const Fingerprint& fingerprint) const noexcept;
        };
    }

    // Serves repeated D3DCompileFromFile calls from bytecode compiled earlier, in this process or in an
    // earlier one that used the same directory.
    //
    // A call is a repeat when its file name, its source preprocessed through the caller's include
    // handler, its defines in order, entrypoint, target, both flag words and the loaded
    // d3dcompiler_47.dll version all match an earlier successful compile. The include handler is
    // therefore called once more per compile, so it must be safe to Open and Close the same files
    // repeatedly. Entries are named by that fingerprint and never go stale, but nothing removes old
    // ones; deleting the directory clears the cache.
    //
    // Only successful compiles are cached. Every hit returns new blobs holding copies of the cached
    // bytecode and diagnostics, so callers keep sole ownership of what they receive. When validating,
    // hits are compiled anyway, compared with the cache and the fresh result is returned.
    // Thread-safe; compiles and file I/O run outside the lock.
    class ShaderCompileCache
    {
    public:
        ShaderCompileCache(CompileFromFileFn compile, std::filesystem::path directory, bool isValidating);

        // Same contract as D3DCompileFromFile.
        HRESULT CompileFromFile(LPCWSTR fileName, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
            LPCSTR entrypoint, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code,
            ID3DBlob** errorMessages);

        [[nodiscard]] CompileStatistics GetStatistics() const;

    private:
        struct Lookup
        {
            const detail::CachedCompile& compile;
            bool isFromDisk;
        };

        // Looks in memory, then on disk; an entry read from disk is kept in memory for later hits.
        std::optional<Lookup> Find(const detail::Fingerprint& fingerprint);

        [[nodiscard]] std::filesystem::path EntryPathOf(const detail::Fingerprint& fingerprint) const;

        CompileFromFileFn _compile;
        std::filesystem::path _directory;
        bool _isValidating;
        // Nullopt when the version cannot be read; nothing is persisted then.
        std::optional<std::string> _compilerVersion;
        mutable std::mutex _mutex;
        // Entries are never erased, so references to them stay valid outside the lock.
        std::unordered_map<detail::Fingerprint, detail::CachedCompile, detail::FingerprintHash> _compiles;
        CompileStatistics _statistics;
    };
}
