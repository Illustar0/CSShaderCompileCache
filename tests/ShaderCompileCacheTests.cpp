#include "ShaderCompileCache.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;

    std::atomic<int> compileCount = 0;

    HRESULT WINAPI CountingCompile(const LPCWSTR fileName, const D3D_SHADER_MACRO* const defines,
        ID3DInclude* const include, const LPCSTR entrypoint, const LPCSTR target, const UINT flags1,
        const UINT flags2, ID3DBlob** const code, ID3DBlob** const errorMessages)
    {
        ++compileCount;
        return D3DCompileFromFile(fileName, defines, include, entrypoint, target, flags1, flags2, code, errorMessages);
    }

    // Resolves every include inside one directory, like Community Shaders' Util::CustomInclude.
    class DirectoryInclude final : public ID3DInclude
    {
    public:
        explicit DirectoryInclude(std::filesystem::path directory) :
            _directory(std::move(directory)) {}

        HRESULT __stdcall Open(D3D_INCLUDE_TYPE, const LPCSTR fileName, LPCVOID, LPCVOID* const data, UINT* const bytes) override
        {
            std::ifstream file(_directory / fileName, std::ios::binary);
            if (!file) {
                return E_FAIL;
            }
            const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            auto* buffer = new char[text.size()];
            std::ranges::copy(text, buffer);
            *data = buffer;
            *bytes = static_cast<UINT>(text.size());
            return S_OK;
        }

        HRESULT __stdcall Close(const LPCVOID data) override
        {
            delete[] static_cast<const char*>(data);
            return S_OK;
        }

    private:
        std::filesystem::path _directory;
    };

    struct CompileOutput
    {
        HRESULT result;
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errorMessages;

        [[nodiscard]] std::string_view CodeBytes() const
        {
            return { static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize() };
        }
    };

    constexpr std::string_view kShader = R"(#include "scale.hlsli"
RWBuffer<float> output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    output[id.x] = VALUE * SCALE;
}
)";

    std::filesystem::path CurrentTestDirectory()
    {
        const auto* test = testing::UnitTest::GetInstance()->current_test_info();
        return std::filesystem::temp_directory_path() / "CSShaderCompileCacheTests" / test->name();
    }

    class ShaderCompileCacheTest : public testing::Test
    {
    protected:
        ShaderCompileCacheTest() :
            _directory(CurrentTestDirectory()), _include(_directory) {}

        void SetUp() override
        {
            compileCount = 0;
            std::filesystem::remove_all(_directory);
            std::filesystem::create_directories(_directory);
            WriteFile("main.hlsl", kShader);
            WriteFile("scale.hlsli", "#define SCALE 2.0\n");
        }

        void TearDown() override { std::filesystem::remove_all(_directory); }

        void WriteFile(const std::string_view name, const std::string_view text) const
        {
            std::ofstream(_directory / name, std::ios::binary | std::ios::trunc) << text;
        }

        struct CompileOptions
        {
            const char* value = "1.0";
            bool isDiscardingErrorMessages = false;
            bool isUsingStandardInclude = false;
        };

        CompileOutput Compile(CSC::ShaderCompileCache& cache, const CompileOptions& options)
        {
            const D3D_SHADER_MACRO defines[] = { { "VALUE", options.value }, { nullptr, nullptr } };
            ID3DInclude* include = options.isUsingStandardInclude ? D3D_COMPILE_STANDARD_FILE_INCLUDE : &_include;
            const std::wstring fileName = (_directory / "main.hlsl").wstring();
            CompileOutput output{};
            output.result = cache.CompileFromFile(fileName.c_str(), defines, include, "main", "cs_5_0", 0, 0,
                output.code.GetAddressOf(),
                options.isDiscardingErrorMessages ? nullptr : output.errorMessages.GetAddressOf());
            return output;
        }

        CompileOutput Compile(CSC::ShaderCompileCache& cache) { return Compile(cache, {}); }

        [[nodiscard]] std::filesystem::path CacheDirectory() const { return _directory / "cache"; }

        [[nodiscard]] std::vector<std::filesystem::path> CacheEntries() const
        {
            std::vector<std::filesystem::path> entries;
            for (const auto& entry : std::filesystem::directory_iterator(CacheDirectory())) {
                entries.push_back(entry.path());
            }
            return entries;
        }

        std::filesystem::path _directory;
        DirectoryInclude _include;
    };
}

TEST_F(ShaderCompileCacheTest, RepeatedCompileIsServedFromCache)
{
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache);
    const CompileOutput second = Compile(cache);

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 1);
    EXPECT_EQ(second.CodeBytes(), first.CodeBytes());
    EXPECT_NE(second.code.Get(), first.code.Get());
    const CSC::CompileStatistics statistics = cache.GetStatistics();
    EXPECT_EQ(statistics.misses, 1u);
    EXPECT_EQ(statistics.hits, 1u);
}

TEST_F(ShaderCompileCacheTest, ChangedIncludeIsCompiledAgain)
{
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache);
    WriteFile("scale.hlsli", "#define SCALE 3.0\n");
    const CompileOutput second = Compile(cache);

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 2);
    EXPECT_NE(second.CodeBytes(), first.CodeBytes());
}

TEST_F(ShaderCompileCacheTest, ChangedDefineIsCompiledAgain)
{
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache, { .value = "1.0" });
    const CompileOutput second = Compile(cache, { .value = "4.0" });

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 2);
    EXPECT_NE(second.CodeBytes(), first.CodeBytes());
}

TEST_F(ShaderCompileCacheTest, FailedCompileIsNotCached)
{
    WriteFile("main.hlsl", "this is not HLSL");
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache);
    const CompileOutput second = Compile(cache);

    EXPECT_TRUE(FAILED(first.result));
    EXPECT_TRUE(FAILED(second.result));
    EXPECT_NE(second.errorMessages, nullptr);
    EXPECT_EQ(compileCount, 2);
}

TEST_F(ShaderCompileCacheTest, HitReturnsWarningsTheFirstCallerDiscarded)
{
    // Implicit truncation of a vector type is a warning, not an error.
    WriteFile("scale.hlsli", "static const float2 kScale = float4(2, 2, 2, 2);\n#define SCALE kScale.x\n");
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache, { .isDiscardingErrorMessages = true });
    const CompileOutput second = Compile(cache);

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 1);
    ASSERT_NE(second.errorMessages, nullptr);
    const std::string_view warnings(static_cast<const char*>(second.errorMessages->GetBufferPointer()),
        second.errorMessages->GetBufferSize());
    EXPECT_NE(warnings.find("warning"), std::string_view::npos);
}

TEST_F(ShaderCompileCacheTest, ValidatingCompilesEveryHitAndFindsItExact)
{
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), true);

    const CompileOutput first = Compile(cache);
    const CompileOutput second = Compile(cache);

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 2);
    const CSC::CompileStatistics statistics = cache.GetStatistics();
    EXPECT_EQ(statistics.hits, 1u);
    EXPECT_EQ(statistics.mismatches, 0u);
}

TEST_F(ShaderCompileCacheTest, StandardIncludeIsCachedUntilTheIncludeChanges)
{
    CSC::ShaderCompileCache cache(&CountingCompile, CacheDirectory(), false);

    const CompileOutput first = Compile(cache, { .isUsingStandardInclude = true });
    const CompileOutput repeated = Compile(cache, { .isUsingStandardInclude = true });
    WriteFile("scale.hlsli", "#define SCALE 3.0\n");
    const CompileOutput changed = Compile(cache, { .isUsingStandardInclude = true });

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(repeated.result));
    ASSERT_TRUE(SUCCEEDED(changed.result));
    EXPECT_EQ(compileCount, 2);
    EXPECT_EQ(repeated.CodeBytes(), first.CodeBytes());
    EXPECT_NE(changed.CodeBytes(), first.CodeBytes());
}

TEST_F(ShaderCompileCacheTest, LaterCacheReusesPersistedCompile)
{
    CompileOutput first;
    {
        CSC::ShaderCompileCache earlier(&CountingCompile, CacheDirectory(), false);
        first = Compile(earlier);
    }
    CSC::ShaderCompileCache later(&CountingCompile, CacheDirectory(), false);
    const CompileOutput second = Compile(later);

    ASSERT_TRUE(SUCCEEDED(first.result));
    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 1);
    EXPECT_EQ(second.CodeBytes(), first.CodeBytes());
    EXPECT_EQ(later.GetStatistics().diskHits, 1u);
}

TEST_F(ShaderCompileCacheTest, DamagedEntryIsCompiledAgainAndRewritten)
{
    CompileOutput first;
    {
        CSC::ShaderCompileCache earlier(&CountingCompile, CacheDirectory(), false);
        first = Compile(earlier);
    }
    const auto entries = CacheEntries();
    ASSERT_EQ(entries.size(), 1u);
    std::filesystem::resize_file(entries.front(), std::filesystem::file_size(entries.front()) - 1);

    CSC::ShaderCompileCache later(&CountingCompile, CacheDirectory(), false);
    const CompileOutput second = Compile(later);

    ASSERT_TRUE(SUCCEEDED(second.result));
    EXPECT_EQ(compileCount, 2);
    EXPECT_EQ(second.CodeBytes(), first.CodeBytes());
    EXPECT_EQ(later.GetStatistics().diskHits, 0u);
    CSC::ShaderCompileCache afterRepair(&CountingCompile, CacheDirectory(), false);
    Compile(afterRepair);
    EXPECT_EQ(afterRepair.GetStatistics().diskHits, 1u);
}
