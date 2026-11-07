#include "gfx/RootSignature.h"

#include "core/Log.h"
#include "gfx/GpuUtils.h"

namespace walnut
{
    namespace
    {
        D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(UINT shaderRegister, D3D12_FILTER filter,
                                                    D3D12_TEXTURE_ADDRESS_MODE addressMode)
        {
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = filter;
            sampler.AddressU = addressMode;
            sampler.AddressV = addressMode;
            sampler.AddressW = addressMode;
            sampler.MaxAnisotropy = (filter == D3D12_FILTER_ANISOTROPIC) ? 16 : 1;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.MinLOD = 0.0f;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = shaderRegister;
            sampler.RegisterSpace = 0;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            return sampler;
        }
    }

    ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device)
    {
        CD3DX12_ROOT_PARAMETER1 parameters[RootParam::Count];

        // Root constants: the values themselves are stored in the root signature (copied into the command list),
        // so they cost no memory lookup. The whole root signature is limited to 64 DWORDs, so keep this small.
        parameters[RootParam::DrawConstants].InitAsConstants(kNumDrawConstants, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        // Root CBVs: a 64-bit GPU address each (2 DWORDs). DATA_STATIC_WHILE_SET_AT_EXECUTE promises the data
        // won't change while the GPU may read it, which the FrameAllocator guarantees.
        parameters[RootParam::DrawData].InitAsConstantBufferView(
            1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_SHADER_VISIBILITY_ALL);
        parameters[RootParam::FrameData].InitAsConstantBufferView(
            2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_SHADER_VISIBILITY_ALL);

        // Static samplers cost no descriptor heap space and no root signature space. Most engines only ever
        // need a handful of sampler states, so baking them in avoids a sampler heap entirely.
        const D3D12_STATIC_SAMPLER_DESC samplers[] = {
            MakeStaticSampler(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP),            // s0
            MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),    // s1
            MakeStaticSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),     // s2
        };

        // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is what makes ResourceDescriptorHeap[] legal in shaders. There is no
        // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT: vertex shaders fetch their own vertices, so no PSO has an input layout.
        const D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(_countof(parameters), parameters, _countof(samplers), samplers, flags);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &serialized, &errors);
        if (FAILED(hr) && errors)
            log::Error("Root signature serialization failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
        WR_CHECK(hr);

        ComPtr<ID3D12RootSignature> rootSignature;
        WR_CHECK(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSignature)));
        SetDebugName(rootSignature.Get(), L"BindlessRootSignature");
        return rootSignature;
    }
}
