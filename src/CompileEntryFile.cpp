#include "CompileEntryFile.h"

#include "Sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <type_traits>

namespace CSC::detail
{
    namespace
    {
        constexpr std::array<char, 4> kMagic{ 'C', 'S', 'C', '1' };

        struct EntryHeader
        {
            std::int64_t compileTimeNanoseconds;
            std::array<char, 4> magic;
            HRESULT result;
            std::uint32_t codeSize;
            std::uint32_t errorMessagesSize;
            std::uint32_t hasErrorMessages;
            std::uint32_t reserved;
            // Of the code followed by the error messages.
            Sha256Digest payloadDigest;
        };
        static_assert(sizeof(EntryHeader) == 64);
        // No padding, so the bytes written are fully determined by the fields.
        static_assert(std::has_unique_object_representations_v<EntryHeader>);

        std::vector<std::byte> PayloadOf(const CachedCompile& compile)
        {
            std::vector<std::byte> payload = compile.code;
            payload.insert(payload.end(), compile.errorMessages.begin(), compile.errorMessages.end());
            return payload;
        }

        std::optional<CachedCompile> ParseEntry(const std::span<const std::byte> bytes)
        {
            std::array<std::byte, sizeof(EntryHeader)> headerBytes;
            if (bytes.size() < headerBytes.size()) {
                return std::nullopt;
            }
            std::ranges::copy(bytes.first(headerBytes.size()), headerBytes.begin());
            const auto header = std::bit_cast<EntryHeader>(headerBytes);
            const auto payload = bytes.subspan(headerBytes.size());
            if (header.magic != kMagic ||
                payload.size() != std::size_t{ header.codeSize } + header.errorMessagesSize ||
                Sha256(payload) != header.payloadDigest) {
                return std::nullopt;
            }
            const auto code = payload.first(header.codeSize);
            const auto errorMessages = payload.subspan(header.codeSize);
            return CachedCompile{
                .result = header.result,
                .code = { code.begin(), code.end() },
                .hasErrorMessages = header.hasErrorMessages != 0,
                .errorMessages = { errorMessages.begin(), errorMessages.end() },
                .compileTime = std::chrono::nanoseconds(header.compileTimeNanoseconds),
            };
        }
    }

    std::optional<CachedCompile> ReadCompileEntry(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return std::nullopt;
        }
        file.close();

        auto entry = ParseEntry(bytes);
        if (!entry) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        return entry;
    }

    bool WriteCompileEntry(const std::filesystem::path& path, const CachedCompile& compile)
    {
        const std::vector<std::byte> payload = PayloadOf(compile);
        const auto digest = Sha256(payload);
        if (!digest) {
            return false;
        }
        const EntryHeader header{
            .compileTimeNanoseconds = compile.compileTime.count(),
            .magic = kMagic,
            .result = compile.result,
            .codeSize = static_cast<std::uint32_t>(compile.code.size()),
            .errorMessagesSize = static_cast<std::uint32_t>(compile.errorMessages.size()),
            .hasErrorMessages = compile.hasErrorMessages,
            .reserved = 0,
            .payloadDigest = *digest,
        };

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
        // Writers of the same entry, in this process or another game instance, never share a file.
        const std::filesystem::path temporary =
            path.wstring() + std::format(L".{}.{}.tmp", GetCurrentProcessId(), GetCurrentThreadId());
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        file.close();
        if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }
}
