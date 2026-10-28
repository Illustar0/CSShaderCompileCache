set_xmakever("3.0.0")
set_project("CSShaderCompileCache")
set_version("0.1.0")

set_allowedplats("windows")
set_allowedarchs("x64")
set_languages("c++23")
set_encodings("utf-8")
set_warnings("allextra")
add_rules("mode.debug", "mode.releasedbg")
-- CLion opens the project through this compilation database.
add_rules("plugin.compile_commands.autoupdate", { outputdir = "." })
set_defaultmode("releasedbg")
set_runtimes(is_mode("debug") and "MDd" or "MD")

-- The patch targets CommunityShaders.dll, not the game executable; VR is not analysed.
set_config("skyrim_se", true)
set_config("skyrim_ae", true)
set_config("skyrim_vr", false)
set_config("tests", false)

includes("lib/commonlibsse-ng")

add_requires("gtest 1.17.0", { configs = { main = true, gmock = false } })

target("CSShaderCompileCache")
    set_kind("shared")
    set_default(true)
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "CSShaderCompileCache",
        description = "Reuses Community Shaders bytecode compiled twice during startup"
    })
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00")
    add_syslinks("bcrypt", "d3dcompiler", "version")
    set_pcxxheader("src/PCH.h")
    add_files("src/*.cpp")
    add_installfiles("dist/CSShaderCompileCache.ini", { prefixdir = "SKSE/Plugins" })
    set_installdir("build/package")

-- Engine-independent tests against the real D3DCompiler: `xmake build tests && xmake run tests`.
target("tests")
    set_kind("binary")
    set_default(false)
    add_packages("gtest", "spdlog")
    -- main() comes from gtest_main.lib, and link.exe infers the subsystem only from object files.
    add_ldflags("/subsystem:console")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_WIN32_WINNT=0x0A00")
    add_syslinks("bcrypt", "d3dcompiler", "version")
    add_includedirs("src")
    add_files("src/ShaderCompileCache.cpp", "src/CompileEntryFile.cpp", "tests/*.cpp")
