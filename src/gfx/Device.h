#pragma once

#include "gfx/D3D12Common.h"

#include <string>

namespace walnut
{
    struct DeviceDesc
    {
        bool enableDebugLayer = false;
        bool enableGpuValidation = false; // much slower; implies enableDebugLayer
    };

    // Minimum requirements, checked per adapter in SelectAdapterAndCreateDevice.
    inline constexpr D3D_SHADER_MODEL kMinShaderModel = D3D_SHADER_MODEL_6_6;
    inline constexpr D3D12_RESOURCE_BINDING_TIER kMinResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;

    // "6.6" etc.
    std::string ToString(D3D_SHADER_MODEL model);

    // Owns the DXGI factory, the chosen adapter and the D3D12 device.
    class Device
    {
    public:
        explicit Device(const DeviceDesc& desc);
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        // ID3D12Device10 adds the Enhanced Barriers resource creation APIs (CreateCommittedResource3,
        // InitialLayout). It converts implicitly to ID3D12Device* for everything else.
        ID3D12Device10* Get() const { return m_device.Get(); }
        IDXGIFactory6* GetFactory() const { return m_factory.Get(); }
        const std::wstring& GetAdapterName() const { return m_adapterName; }
        bool IsTearingSupported() const { return m_tearingSupported; }
        bool IsDebugLayerEnabled() const { return m_debugLayerEnabled; }
        D3D_SHADER_MODEL GetShaderModel() const { return m_shaderModel; }
        D3D12_RESOURCE_BINDING_TIER GetResourceBindingTier() const { return m_resourceBindingTier; }
        // Inline ray tracing (RayQuery in any shader) needs tier 1.1. Optional: ray-traced features check this.
        D3D12_RAYTRACING_TIER GetRaytracingTier() const { return m_raytracingTier; }
        bool SupportsInlineRaytracing() const { return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_1; }

        // Logs the device-removed reason and DRED breadcrumbs/page-fault info, if available.
        void LogDeviceRemoved() const;

    private:
        void EnableDebugLayer(bool gpuValidation);
        void CreateFactory();
        void SelectAdapterAndCreateDevice();
        void ConfigureInfoQueue();

        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter4> m_adapter;
        ComPtr<ID3D12Device10> m_device;
        std::wstring m_adapterName;
        D3D_SHADER_MODEL m_shaderModel = D3D_SHADER_MODEL_5_1;
        D3D12_RESOURCE_BINDING_TIER m_resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        bool m_tearingSupported = false;
        bool m_debugLayerEnabled = false;
        DWORD m_infoQueueCallbackCookie = 0;
    };

    // Reports leaked DXGI/D3D12 objects to the debug output (requires Graphics Tools).
    void ReportLiveObjects();
}
