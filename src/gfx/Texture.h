#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>
#include <string>

namespace walnut
{
    struct TextureDesc
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint16_t mipLevels = 1;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        // Textures, unlike buffers, have a layout. COPY_DEST suits textures filled by UploadContext::UploadTexture.
        D3D12_BARRIER_LAYOUT initialLayout = D3D12_BARRIER_LAYOUT_COPY_DEST;
        std::wstring name;
    };

    // Owns one committed 2D texture in a DEFAULT heap. Move-only, like Buffer.
    //
    // Unlike buffers, textures can't live in UPLOAD heaps (their memory layout is GPU-specific and opaque), so
    // they are always filled through a staging copy (UploadContext::UploadTexture).
    class Texture
    {
    public:
        Texture() = default;
        Texture(ID3D12Device10* device, const TextureDesc& desc);

        Texture(Texture&&) noexcept = default;
        Texture& operator=(Texture&&) noexcept = default;
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        bool IsValid() const { return m_resource != nullptr; }
        ID3D12Resource* Get() const { return m_resource.Get(); }
        const D3D12_RESOURCE_DESC1& GetResourceDesc() const { return m_desc; }
        uint32_t GetWidth() const { return static_cast<uint32_t>(m_desc.Width); }
        uint32_t GetHeight() const { return m_desc.Height; }
        uint16_t GetMipLevels() const { return m_desc.MipLevels; }
        DXGI_FORMAT GetFormat() const { return m_desc.Format; }

        // Writes a Texture2D SRV covering all mips into 'destination' (e.g. a slot of the bindless heap).
        void CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE destination) const;

    private:
        ComPtr<ID3D12Resource> m_resource;
        D3D12_RESOURCE_DESC1 m_desc{};
    };
}
