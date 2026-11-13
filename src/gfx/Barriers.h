#pragma once

#include "gfx/D3D12Common.h"

#include <array>
#include <cassert>
#include <cstdint>

// Enhanced Barriers with named states. A barrier says: wait for this earlier work (sync before) and make its writes
// (access before) visible to this later work (sync after, access after), changing the texture's layout on the way.
// Each call site keeps the comment on why it needs the barrier it records.
namespace walnut
{
    // In D3D12_BARRIER_SUBRESOURCE_RANGE, an index of 0xffffffff (with NumMipLevels = 0) means every subresource.
    inline constexpr UINT kAllSubresources = 0xffffffff;

    // One side of a texture barrier: which work uses the texture, how, and in which layout.
    struct TextureAccess
    {
        D3D12_BARRIER_SYNC sync;
        D3D12_BARRIER_ACCESS access;
        D3D12_BARRIER_LAYOUT layout;
    };

    // One side of a buffer barrier (buffers have no layout).
    struct BufferAccess
    {
        D3D12_BARRIER_SYNC sync;
        D3D12_BARRIER_ACCESS access;
    };

    namespace access
    {
        // Nothing in this command list to wait for, and the contents don't matter (layout UNDEFINED is legal from
        // whatever layout the texture was left in). As the "before" of a texture about to be overwritten.
        inline constexpr TextureAccess Undefined{ D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                                                  D3D12_BARRIER_LAYOUT_UNDEFINED };
        // The swap chain's layout for Present (= COMMON). Present happens outside the command list, so there's no
        // work in it to wait for or to block.
        inline constexpr TextureAccess Present{ D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                                                D3D12_BARRIER_LAYOUT_PRESENT };
        inline constexpr TextureAccess RenderTarget{ D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                                                     D3D12_BARRIER_LAYOUT_RENDER_TARGET };
        // Depth tests and writes.
        inline constexpr TextureAccess DepthWrite{ D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                                                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE };
        // Depth tests only (no writes), still in the writable layout.
        inline constexpr TextureAccess DepthTest{ D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ,
                                                  D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE };
        // Sampled by pixel shaders on the direct queue. DIRECT_QUEUE_SHADER_RESOURCE: only the direct queue reads it,
        // which can be faster than the generic SHADER_RESOURCE layout.
        inline constexpr TextureAccess PixelShaderRead{ D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                                        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE };
        inline constexpr TextureAccess CopySource{ D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                                   D3D12_BARRIER_LAYOUT_COPY_SOURCE };
        inline constexpr TextureAccess CopyDest{ D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
                                                 D3D12_BARRIER_LAYOUT_COPY_DEST };

        // Buffers.
        inline constexpr BufferAccess ComputeReadWrite{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
        inline constexpr BufferAccess PixelShaderReadBuffer{ D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE };
        inline constexpr BufferAccess CopySourceBuffer{ D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE };
    }

    // Collects barriers and records them with one Barrier() call, so the driver can batch them.
    class BarrierBatch
    {
    public:
        // discard: the old contents may be thrown away (only with 'before' = access::Undefined); lets the driver
        // skip preserving them and just (re)initialize the compression metadata.
        BarrierBatch& Texture(ID3D12Resource* texture, const TextureAccess& before, const TextureAccess& after,
                              bool discard = false)
        {
            assert(m_textureCount < kCapacity);
            m_textures[m_textureCount++] = CD3DX12_TEXTURE_BARRIER(
                before.sync, after.sync, before.access, after.access, before.layout, after.layout, texture,
                CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources),
                discard ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE);
            return *this;
        }

        BarrierBatch& Buffer(ID3D12Resource* buffer, const BufferAccess& before, const BufferAccess& after)
        {
            assert(m_bufferCount < kCapacity);
            m_buffers[m_bufferCount++] = CD3DX12_BUFFER_BARRIER(before.sync, after.sync, before.access, after.access, buffer);
            return *this;
        }

        void Submit(ID3D12GraphicsCommandList7* commandList)
        {
            std::array<D3D12_BARRIER_GROUP, 2> groups;
            UINT groupCount = 0;
            if (m_bufferCount > 0)
                groups[groupCount++] = CD3DX12_BARRIER_GROUP(m_bufferCount, m_buffers.data());
            if (m_textureCount > 0)
                groups[groupCount++] = CD3DX12_BARRIER_GROUP(m_textureCount, m_textures.data());
            if (groupCount > 0)
                commandList->Barrier(groupCount, groups.data());
            m_textureCount = 0;
            m_bufferCount = 0;
        }

    private:
        static constexpr UINT32 kCapacity = 8;
        std::array<D3D12_TEXTURE_BARRIER, kCapacity> m_textures{};
        std::array<D3D12_BUFFER_BARRIER, kCapacity> m_buffers{};
        UINT32 m_textureCount = 0;
        UINT32 m_bufferCount = 0;
    };

    // A texture that remembers the state its last barrier left it in. Passes ask for the state they need and get
    // the barrier from wherever it is now, instead of every pass knowing what the one before it did.
    class TrackedTexture
    {
    public:
        // A new resource, or a new frame: 'state' is what the next barrier starts from.
        void Reset(ID3D12Resource* resource, const TextureAccess& state)
        {
            m_resource = resource;
            m_state = state;
        }

        // Adds the barrier from the current state to 'to' to the batch, unless the current state already serves it.
        // discard: the contents may be thrown away (see BarrierBatch::Texture).
        void Transition(BarrierBatch& batch, const TextureAccess& to, bool discard = false)
        {
            if (Serves(m_state, to))
                return;
            batch.Texture(m_resource, m_state, to, discard);
            m_state = to;
        }

        ID3D12Resource* Get() const { return m_resource; }
        const TextureAccess& GetState() const { return m_state; }

    private:
        // Same layout, and the access allows what's asked: the same access, or depth writes when only depth tests
        // are needed (a depth target that's written can also be tested against).
        static bool Serves(const TextureAccess& current, const TextureAccess& wanted)
        {
            if (current.layout != wanted.layout || current.sync != wanted.sync)
                return false;
            return current.access == wanted.access ||
                   (current.access == D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE &&
                    wanted.access == D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
        }

        ID3D12Resource* m_resource = nullptr;
        TextureAccess m_state = access::Undefined;
    };
}
