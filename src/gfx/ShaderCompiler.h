#pragma once

#include "gfx/D3D12Common.h"

#include <dxcapi.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace walnut
{
    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Compute,
    };

    // Every shader is compiled for this model ("vs_6_6", "ps_6_6", ...). It must not exceed what the device
    // supports (Device checks kMinShaderModel), or creating a pipeline from the bytecode fails.
    // 6.6 adds ResourceDescriptorHeap[] / SamplerDescriptorHeap[] (bindless).
    inline constexpr wchar_t kShaderModelSuffix[] = L"6_6";

    struct ShaderCompileDesc
    {
        std::filesystem::path path;
        std::wstring entryPoint = L"main";
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<std::pair<std::wstring, std::wstring>> defines;
    };

    struct ShaderCompileResult
    {
        ComPtr<IDxcBlob> bytecode;
        std::string errors; // compiler errors and warnings (may be non-empty on success)

        bool Succeeded() const { return bytecode != nullptr; }
        D3D12_SHADER_BYTECODE GetBytecode() const;
    };

    // Runtime HLSL -> DXIL compiler built on DXC (dxcompiler.dll + dxil.dll).
    //
    // #include "x.hlsli" is searched in the including file's folder first, then in 'shaderRoot'
    // (the shaders/ folder next to the exe), so any shader can write #include "common/Foo.hlsli".
    class ShaderCompiler
    {
    public:
        ShaderCompiler(bool debugInfo, std::filesystem::path shaderRoot);
        ~ShaderCompiler();

        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;

        ShaderCompileResult Compile(const ShaderCompileDesc& desc) const;

        const std::filesystem::path& GetShaderRoot() const { return m_shaderRoot; }

    private:
        ComPtr<IDxcUtils> m_utils;
        ComPtr<IDxcCompiler3> m_compiler;
        ComPtr<IDxcIncludeHandler> m_includeHandler;
        bool m_debugInfo = false;
        std::filesystem::path m_shaderRoot;
    };
}
