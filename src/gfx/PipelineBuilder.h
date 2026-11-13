#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/ShaderCompiler.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace walnut
{
    using ShaderDefines = std::vector<std::pair<std::wstring, std::wstring>>;

    // Builds the pipelines of one pass from one shader file: compiles entry points, creates PSOs, names them, and
    // collects every error on the way. After the first failure it creates nothing more, so a pass can build all its
    // PSOs and check Failed() once at the end. It keeps the compiled shaders alive while it exists.
    class PipelineBuilder
    {
    public:
        PipelineBuilder(ID3D12Device10* device, const ShaderCompiler& compiler, ID3D12RootSignature* rootSignature,
                        std::filesystem::path shaderPath);

        // Compiles one entry point of the file. On failure: empty bytecode, and the errors are kept.
        D3D12_SHADER_BYTECODE Shader(const wchar_t* entryPoint, ShaderStage stage, ShaderDefines defines = {});

        // Creates a PSO and gives it a debug name (shown by PIX and the debug layer). The root signature is filled
        // in when the desc doesn't set one. Returns null once anything has failed.
        ComPtr<ID3D12PipelineState> Graphics(D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, const std::wstring& debugName);
        ComPtr<ID3D12PipelineState> Compute(D3D12_SHADER_BYTECODE shader, const std::wstring& debugName);

        bool Failed() const { return m_failed; }
        // Compiler errors and warnings, and PSO creation failures.
        const std::string& GetErrors() const { return m_errors; }

    private:
        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        ID3D12RootSignature* m_rootSignature = nullptr;
        std::filesystem::path m_shaderPath;
        std::vector<ComPtr<IDxcBlob>> m_shaders;
        std::string m_errors;
        bool m_failed = false;
    };

    // A fullscreen pass (tonemap, deferred lighting): one triangle over the screen, no culling, no blending, no depth
    // buffer, one render target. The shaders are left for the caller.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullscreenPassDesc(DXGI_FORMAT renderTargetFormat);

    // A pass whose shaders F5 recompiles. ReloadShaders keeps the previous PSOs when anything fails, and
    // GetLastError says why; the UI shows it.
    class ShaderReloadable
    {
    public:
        virtual ~ShaderReloadable() = default;

        // Recompiles the shaders and rebuilds the PSOs. On failure the previous PSOs are kept.
        // The old PSOs are released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

    protected:
        ShaderReloadable(const char* name, ID3D12Device10* device, const ShaderCompiler& compiler,
                         ID3D12RootSignature* rootSignature, std::filesystem::path shaderPath);

        // For the derived constructor: the first build, which must succeed (throws otherwise).
        void BuildPipelinesOrThrow();

        // Builds every PSO with the builder into locals, and moves them into the members only if
        // !builder.Failed() at the end (otherwise the previous ones stay).
        virtual void BuildPipelines(PipelineBuilder& builder) = 0;

    private:
        const char* m_name; // for the error message of a failed first build
        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        std::filesystem::path m_shaderPath;
        std::string m_lastError;
    };
}
