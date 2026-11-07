#include "gfx/ShaderCompiler.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "gfx/GpuUtils.h"

namespace walnut
{
    D3D12_SHADER_BYTECODE ShaderCompileResult::GetBytecode() const
    {
        if (!bytecode)
            return {};
        return { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
    }

    namespace
    {
        std::wstring MakeTarget(ShaderStage stage)
        {
            const wchar_t* prefix = L"vs_";
            switch (stage)
            {
            case ShaderStage::Vertex: prefix = L"vs_"; break;
            case ShaderStage::Pixel: prefix = L"ps_"; break;
            case ShaderStage::Compute: prefix = L"cs_"; break;
            }
            return std::wstring(prefix) + kShaderModelSuffix;
        }
    }

    ShaderCompiler::ShaderCompiler(bool debugInfo, std::filesystem::path shaderRoot)
        : m_debugInfo(debugInfo), m_shaderRoot(std::move(shaderRoot))
    {
        WR_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)));
        WR_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)));
        WR_CHECK(m_utils->CreateDefaultIncludeHandler(&m_includeHandler));
    }

    ShaderCompiler::~ShaderCompiler() = default;

    ShaderCompileResult ShaderCompiler::Compile(const ShaderCompileDesc& desc) const
    {
        ShaderCompileResult result;
        const std::wstring pathString = desc.path.wstring();

        ComPtr<IDxcBlobEncoding> source;
        if (FAILED(m_utils->LoadFile(pathString.c_str(), nullptr, &source)))
        {
            result.errors = "Failed to read shader file: " + ToUtf8(pathString);
            return result;
        }

        const std::wstring includeDir = desc.path.parent_path().wstring();
        const std::wstring rootIncludeDir = m_shaderRoot.wstring();
        const std::wstring target = MakeTarget(desc.stage);
        std::vector<std::wstring> defineStrings;
        defineStrings.reserve(desc.defines.size());
        for (const auto& [name, value] : desc.defines)
            defineStrings.push_back(value.empty() ? name : name + L"=" + value);

        std::vector<LPCWSTR> args = {
            pathString.c_str(), // file name, used in error messages and PIX
            L"-E", desc.entryPoint.c_str(),
            L"-T", target.c_str(),
            L"-I", includeDir.c_str(),
            L"-I", rootIncludeDir.c_str(),
            L"-HV", L"2021",
            DXC_ARG_WARNINGS_ARE_ERRORS,
        };
        if (m_debugInfo)
        {
            args.push_back(DXC_ARG_DEBUG);              // -Zi
            args.push_back(L"-Qembed_debug");           // keep PDB data in the blob for PIX
            args.push_back(DXC_ARG_SKIP_OPTIMIZATIONS); // -Od
        }
        else
        {
            args.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);
        }
        for (const std::wstring& define : defineStrings)
        {
            args.push_back(L"-D");
            args.push_back(define.c_str());
        }

        DxcBuffer buffer{};
        buffer.Ptr = source->GetBufferPointer();
        buffer.Size = source->GetBufferSize();
        buffer.Encoding = DXC_CP_ACP;

        ComPtr<IDxcResult> compileResult;
        HRESULT hr = m_compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()),
                                         m_includeHandler.Get(), IID_PPV_ARGS(&compileResult));
        if (SUCCEEDED(hr))
            compileResult->GetStatus(&hr);

        if (compileResult)
        {
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors && errors->GetStringLength() > 0)
                result.errors = errors->GetStringPointer();
        }

        if (FAILED(hr))
        {
            if (result.errors.empty())
                result.errors = std::format("DXC failed with HRESULT 0x{:08X}", static_cast<uint32_t>(hr));
            log::Error("Shader compile failed: {} ({})\n{}", ToUtf8(pathString), ToUtf8(desc.entryPoint), result.errors);
            return result;
        }

        WR_CHECK(compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result.bytecode), nullptr));
        log::Info("Compiled shader {} [{} {}]", desc.path.filename().string(), ToUtf8(desc.entryPoint), ToUtf8(target));
        return result;
    }
}
