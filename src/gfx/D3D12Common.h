#pragma once

// Single include point for D3D12/DXGI. The DirectX-Headers (matching the Agility SDK)
// must be included before anything pulls in the Windows SDK's <d3d12.h>.
#include <Windows.h>

#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <directx/d3dx12.h>

#include <dxgi1_6.h>
#include <dxgidebug.h>

#include <wrl/client.h>

namespace walnut
{
    using Microsoft::WRL::ComPtr;
}
