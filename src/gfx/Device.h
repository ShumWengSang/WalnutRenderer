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

    // Owns the DXGI factory, the chosen adapter and the D3D12 device.
    class Device
    {
    public:
        explicit Device(const DeviceDesc& desc);
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        ID3D12Device* Get() const { return m_device.Get(); }
        IDXGIFactory6* GetFactory() const { return m_factory.Get(); }
        const std::wstring& GetAdapterName() const { return m_adapterName; }
        bool IsTearingSupported() const { return m_tearingSupported; }
        bool IsDebugLayerEnabled() const { return m_debugLayerEnabled; }

        // Logs the device-removed reason and DRED breadcrumbs/page-fault info, if available.
        void LogDeviceRemoved() const;

    private:
        void EnableDebugLayer(bool gpuValidation);
        void CreateFactory();
        void SelectAdapterAndCreateDevice();
        void ConfigureInfoQueue();

        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter4> m_adapter;
        ComPtr<ID3D12Device> m_device;
        std::wstring m_adapterName;
        bool m_tearingSupported = false;
        bool m_debugLayerEnabled = false;
        DWORD m_infoQueueCallbackCookie = 0;
    };

    // Reports leaked DXGI/D3D12 objects to the debug output (requires Graphics Tools).
    void ReportLiveObjects();
}
