#include "gfx/Device.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "gfx/GpuUtils.h"

namespace walnut
{
    namespace
    {
        void __stdcall OnDebugLayerMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                           D3D12_MESSAGE_ID id, LPCSTR description, void*)
        {
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                log::Error("D3D12 [{}]: {}", static_cast<int>(id), description);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                log::Warn("D3D12 [{}]: {}", static_cast<int>(id), description);
                break;
            default:
                break;
            }
        }
    }

    Device::Device(const DeviceDesc& desc)
    {
        if (desc.enableDebugLayer || desc.enableGpuValidation)
            EnableDebugLayer(desc.enableGpuValidation);

        CreateFactory();
        SelectAdapterAndCreateDevice();
        if (m_debugLayerEnabled)
            ConfigureInfoQueue();
    }

    Device::~Device()
    {
        if (m_infoQueueCallbackCookie != 0)
        {
            ComPtr<ID3D12InfoQueue1> infoQueue1;
            if (SUCCEEDED(m_device.As(&infoQueue1)))
                infoQueue1->UnregisterMessageCallback(m_infoQueueCallbackCookie);
        }
    }

    void Device::EnableDebugLayer(bool gpuValidation)
    {
        ComPtr<ID3D12Debug1> debug;
        if (const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug)); FAILED(hr))
        {
            log::Warn("D3D12 debug layer unavailable (HRESULT 0x{:08X})", static_cast<uint32_t>(hr));
            return;
        }
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(gpuValidation ? TRUE : FALSE);
        m_debugLayerEnabled = true;
        log::Info("D3D12 debug layer enabled (GPU-based validation: {})", gpuValidation ? "on" : "off");

        // Device Removed Extended Data: records breadcrumbs and page faults so TDRs are debuggable.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    void Device::CreateFactory()
    {
        UINT flags = m_debugLayerEnabled ? DXGI_CREATE_FACTORY_DEBUG : 0;
        HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
        if (FAILED(hr) && flags != 0)
        {
            // DXGI debug needs the "Graphics Tools" optional Windows feature.
            log::Warn("DXGI debug factory unavailable (install 'Graphics Tools'); continuing without it");
            hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
        }
        WR_CHECK(hr);

        BOOL allowTearing = FALSE;
        if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
            m_tearingSupported = allowTearing == TRUE;
    }

    void Device::SelectAdapterAndCreateDevice()
    {
        ComPtr<IDXGIAdapter4> adapter;
        for (UINT i = 0;
             m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            DXGI_ADAPTER_DESC3 adapterDesc{};
            adapter->GetDesc3(&adapterDesc);
            if (adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)
                continue;

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
            {
                m_adapter = adapter;
                m_adapterName = adapterDesc.Description;
                log::Info("Using adapter: {} ({} MB VRAM)", ToUtf8(m_adapterName),
                          adapterDesc.DedicatedVideoMemory / (1024 * 1024));
                break;
            }
        }

        if (!m_device)
            throw std::runtime_error("No D3D12 (feature level 12_0) hardware adapter found");

        SetDebugName(m_device.Get(), L"WalnutDevice");

        // Confirms whether the Agility SDK runtime (exe-local D3D12Core.dll) was picked up.
        if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll"))
        {
            wchar_t corePath[MAX_PATH]{};
            GetModuleFileNameW(core, corePath, MAX_PATH);
            log::Info("D3D12Core loaded from: {}", ToUtf8(corePath));
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
            log::Info("Enhanced barriers supported: {}", options12.EnhancedBarriersSupported ? "yes" : "no");
    }

    void Device::ConfigureInfoQueue()
    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(m_device.As(&infoQueue)))
            return;

        if (IsDebuggerPresent())
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }

        // Route debug-layer messages into our log (ID3D12InfoQueue1 ships with the Agility SDK).
        ComPtr<ID3D12InfoQueue1> infoQueue1;
        if (SUCCEEDED(infoQueue.As(&infoQueue1)))
        {
            infoQueue1->RegisterMessageCallback(&OnDebugLayerMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                                nullptr, &m_infoQueueCallbackCookie);
        }
    }

    void Device::LogDeviceRemoved() const
    {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        log::Error("Device removed, reason 0x{:08X}", static_cast<uint32_t>(reason));

        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (FAILED(m_device.As(&dred)))
            return;

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
        {
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
            {
                const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                log::Error("  DRED list '{}' on queue '{}': {}/{} ops completed",
                           node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                           node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?",
                           completed, node->BreadcrumbCount);
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
            log::Error("  DRED page fault at GPU VA 0x{:016X}", pageFault.PageFaultVA);
    }

    void ReportLiveObjects()
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL,
                                         static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
}
