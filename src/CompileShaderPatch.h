#pragma once

#include <stdexcept>

namespace CSC
{
    // Thrown when CommunityShaders.dll is absent, is not the analysed build, or was already patched
    // where this patch writes. Community Shaders is left untouched.
    class InstallRefusal : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // Routes Community Shaders' Util::CompileShader compiles through a ShaderCompileCache.
    // Call once, after CommunityShaders.dll has loaded and before it compiles its first shader.
    void Install(bool isValidating);

    // Logs what the cache did so far; does nothing unless Install succeeded.
    void LogStatistics();
}
