#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include <Windows.h>

namespace CSC::detail
{
    struct CachedCompile
    {
        HRESULT result;
        std::vector<std::byte> code;
        bool hasErrorMessages;
        std::vector<std::byte> errorMessages;
        std::chrono::nanoseconds compileTime;
    };

    // Returns nullopt when no intact entry exists at path. A damaged entry is deleted, so the next
    // successful compile rewrites it; handing damaged bytecode to Community Shaders would make its
    // shader creation throw.
    [[nodiscard]] std::optional<CachedCompile> ReadCompileEntry(const std::filesystem::path& path);

    // Replaces the entry at path in one rename, so readers never see a partial entry, including after a
    // crash mid-write. Returns false when it could not be written.
    bool WriteCompileEntry(const std::filesystem::path& path, const CachedCompile& compile);
}
