#include "gfx/Texture.h"

#include "gfx/GpuUtils.h"

namespace walnut
{
    Texture::Texture(ID3D12Device10* device, const TextureDesc& desc)
    {
        m_desc = CD3DX12_RESOURCE_DESC1::Tex2D(desc.format, desc.width, desc.height, 1 /*array size*/, desc.mipLevels,
                                               1 /*samples*/, 0, desc.flags);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        WR_CHECK(device->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &m_desc, desc.initialLayout,
                                                  nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_resource)));
        SetDebugName(m_resource.Get(), desc.name.c_str());
    }

    void Texture::CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE destination) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = m_desc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = m_desc.MipLevels;
        device->CreateShaderResourceView(m_resource.Get(), &srvDesc, destination);
    }
}
