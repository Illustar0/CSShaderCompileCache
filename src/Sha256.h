#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>

#include <Windows.h>
#include <bcrypt.h>

namespace CSC
{
    using Sha256Digest = std::array<std::byte, 32>;

    // Returns nullopt when BCrypt fails.
    [[nodiscard]] inline std::optional<Sha256Digest> Sha256(const std::span<const std::byte> bytes) noexcept
    {
        Sha256Digest digest;
        const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
            reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()));
        if (!BCRYPT_SUCCESS(status)) {
            return std::nullopt;
        }
        return digest;
    }

    // Lowercase, as sha256sum prints it.
    [[nodiscard]] inline std::string ToHex(const Sha256Digest& digest)
    {
        std::string hex;
        hex.reserve(digest.size() * 2);
        for (const std::byte value : digest) {
            std::format_to(std::back_inserter(hex), "{:02x}", std::to_integer<unsigned>(value));
        }
        return hex;
    }
}
