#pragma once

#include "gfx/D3D12Common.h"

#include <dxcapi.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace walnut
{
    struct ShaderCompileDesc
    {
        std::filesystem::path path;
        std::wstring entryPoint = L"main";
        std::wstring target; // e.g. L"vs_6_0", L"ps_6_0"
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
    class ShaderCompiler
    {
    public:
        explicit ShaderCompiler(bool debugInfo);
        ~ShaderCompiler();

        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;

        ShaderCompileResult Compile(const ShaderCompileDesc& desc) const;

    private:
        ComPtr<IDxcUtils> m_utils;
        ComPtr<IDxcCompiler3> m_compiler;
        ComPtr<IDxcIncludeHandler> m_includeHandler;
        bool m_debugInfo = false;
    };
}
