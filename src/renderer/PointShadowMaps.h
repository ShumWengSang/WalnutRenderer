#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "renderer/ShadowMap.h"
#include "shared/Constants.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace walnut
{
    class Device;

    // Shadow maps for point lights (M1). A point light shines in every direction, so one flat shadow map can't hold
    // what it sees. Instead it gets a CUBE: six square depth images, one per axis direction (+X, -X, +Y, -Y, +Z, -Z),
    // each rendered with a 90-degree camera at the light, so together they cover every direction exactly once.
    //
    // All cubes live in ONE texture: a 2D array of 6 x kMaxShadowedLights slices (cube 'slot' uses slices
    // 6 * slot .. 6 * slot + 5). Views:
    //   DSV per slice  (D32_FLOAT)  each face is rendered on its own
    //   SRV, cube array (R32_FLOAT) the shading samples it with a direction: the hardware picks the face (M2)
    //   SRV per slice  (R32_FLOAT, red into green/blue) to show a face in the UI as a gray image
    class PointShadowMaps
    {
    public:
        static constexpr uint32_t kFaceSize = kPointShadowFaceSize; // shared with the shaders (shared/Constants.h)
        static constexpr uint32_t kMaxShadowedLights = 32;
        static constexpr uint32_t kFaceCount = 6;
        // The same formats as the sun's shadow map.
        static constexpr DXGI_FORMAT kDepthFormat = ShadowMap::kDepthFormat;
        // Each face camera sees from nearScale * range to range. Closer than that, nothing casts a shadow (the light
        // would have to be inside an object). A larger near plane gives the depth buffer more precision.
        static constexpr float kNearScale = 0.05f;

        PointShadowMaps(Device& device, DescriptorHeap& srvHeap);
        ~PointShadowMaps();

        PointShadowMaps(const PointShadowMaps&) = delete;
        PointShadowMaps& operator=(const PointShadowMaps&) = delete;

        // The camera of one face for a light at 'position' with 'range': a 90-degree square perspective looking down
        // the face's axis, reversed-Z (1 at the near plane, 0 at the range). Matches the D3D cube map layout, so that
        // sampling the cube with a direction finds the texel this camera rendered.
        static DirectX::XMMATRIX GetFaceViewProjection(const DirectX::XMFLOAT3& position, float range, uint32_t face);

        // All faces become depth targets (their old contents discarded) ...
        void Begin(ID3D12GraphicsCommandList7* commandList) const;
        // ... one at a time: binds and clears face 'face' of cube 'slot', with a matching viewport ...
        void BeginFace(ID3D12GraphicsCommandList7* commandList, uint32_t slot, uint32_t face) const;
        // ... and then all become readable by pixel shaders.
        void End(ID3D12GraphicsCommandList7* commandList) const;

        uint32_t GetCubeArraySrvIndex() const { return m_cubeArraySrv.index; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetFaceDebugSrv(uint32_t slot, uint32_t face) const
        {
            return m_faceDebugSrvs[slot * kFaceCount + face].gpu;
        }
        uint64_t GetBytes() const { return m_bytes; }

    private:
        DescriptorHeap& m_srvHeap;
        std::unique_ptr<DescriptorHeap> m_dsvHeap;
        ComPtr<ID3D12Resource> m_texture;
        std::vector<OwnedDescriptor> m_dsvs;          // one per slice
        std::vector<OwnedDescriptor> m_faceDebugSrvs; // one per slice
        OwnedDescriptor m_cubeArraySrv;
        uint64_t m_bytes = 0;
    };
}
