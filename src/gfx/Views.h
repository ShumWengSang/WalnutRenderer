#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>

// View descriptions for the common cases, so call sites say what they view instead of filling in structs.
namespace walnut
{
    // How a view's four channels are filled from the texel ("component mapping", applied by the texture unit when it
    // reads, so no shader has to know).
    enum class Swizzle
    {
        Default,  // as stored: (r, g, b, a), missing channels 0 (alpha 1)
        RedAsGray, // (r, r, r, 1): a one-channel texture (e.g. depth) shows as gray instead of red, for the UI
    };

    inline UINT ComponentMapping(Swizzle swizzle)
    {
        if (swizzle == Swizzle::RedAsGray)
            return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }

    // StructuredBuffer<T>: no format, elements of 'stride' bytes. firstElement counts elements, not bytes.
    inline D3D12_SHADER_RESOURCE_VIEW_DESC StructuredBufferSrv(uint32_t count, uint32_t stride, uint64_t firstElement = 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = firstElement;
        desc.Buffer.NumElements = count;
        desc.Buffer.StructureByteStride = stride;
        return desc;
    }

    // RWStructuredBuffer<T>. An unordered access view: read AND write from shaders, in any order ("unordered": the
    // GPU doesn't sort out conflicting writes for you, unlike render targets).
    inline D3D12_UNORDERED_ACCESS_VIEW_DESC StructuredBufferUav(uint32_t count, uint32_t stride)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = count;
        desc.Buffer.StructureByteStride = stride;
        return desc;
    }

    // Texture2D: all of its mips (mipLevels of them, from the most detailed).
    inline D3D12_SHADER_RESOURCE_VIEW_DESC Texture2DSrv(DXGI_FORMAT format, uint32_t mipLevels = 1,
                                                        Swizzle swizzle = Swizzle::Default)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = ComponentMapping(swizzle);
        desc.Texture2D.MipLevels = mipLevels;
        return desc;
    }

    // Texture2DArray over sliceCount slices starting at firstSlice (one mip).
    inline D3D12_SHADER_RESOURCE_VIEW_DESC Texture2DArraySrv(DXGI_FORMAT format, uint32_t firstSlice, uint32_t sliceCount,
                                                             Swizzle swizzle = Swizzle::Default)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Shader4ComponentMapping = ComponentMapping(swizzle);
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = firstSlice;
        desc.Texture2DArray.ArraySize = sliceCount;
        return desc;
    }

    // TextureCubeArray: every 6 slices of a 2D array are one cube (+X, -X, +Y, -Y, +Z, -Z), one mip.
    inline D3D12_SHADER_RESOURCE_VIEW_DESC TextureCubeArraySrv(DXGI_FORMAT format, uint32_t cubeCount)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.TextureCubeArray.MipLevels = 1;
        desc.TextureCubeArray.NumCubes = cubeCount;
        return desc;
    }

    // RaytracingAccelerationStructure. It has no resource (pass nullptr to CreateShaderResourceView): just the GPU
    // address of the TLAS.
    inline D3D12_SHADER_RESOURCE_VIEW_DESC AccelerationStructureSrv(D3D12_GPU_VIRTUAL_ADDRESS tlas)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.RaytracingAccelerationStructure.Location = tlas;
        return desc;
    }

    inline D3D12_DEPTH_STENCIL_VIEW_DESC Texture2DDsv(DXGI_FORMAT format)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        return desc;
    }

    // One slice of a 2D array as a depth target.
    inline D3D12_DEPTH_STENCIL_VIEW_DESC Texture2DArraySliceDsv(DXGI_FORMAT format, uint32_t slice)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = slice;
        desc.Texture2DArray.ArraySize = 1;
        return desc;
    }
}
