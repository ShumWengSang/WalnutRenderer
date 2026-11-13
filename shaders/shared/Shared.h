// Headers in shaders/shared/ are compiled by both C++ (src/ includes them as "shared/...") and DXC, so values and
// layouts that the two sides must agree on are written once. They use only what both languages accept: comments,
// the preprocessor, structs of plain fields, and the macros below.
//
//   WALNUT_CONST(name, value);  C++: inline constexpr uint32_t name = value;   HLSL: static const uint name = value;
//   WALNUT_NAMESPACE_BEGIN/END  C++: namespace walnut { ... }                   HLSL: nothing
//   WALNUT_GPU_NAMESPACE_BEGIN/END  C++: namespace walnut::gpu { ... }, where uint, float3, float4x4 etc. name the
//                               C++ types with HLSL's layout (DirectXMath)       HLSL: nothing
//   WALNUT_ROW_MAJOR            HLSL: row_major (matrices are stored as C++ writes them: rows of the row-vector form)
//
// Structs shared this way must keep HLSL's constant buffer packing: no member may straddle a 16-byte register (a
// float3 can be followed by one 4-byte value; arrays are avoided, since each element takes a whole register).
#ifndef WALNUT_SHARED_H
#define WALNUT_SHARED_H

#ifdef __HLSL_VERSION
#define WALNUT_CONST(name, value) static const uint name = (value)
#define WALNUT_NAMESPACE_BEGIN
#define WALNUT_NAMESPACE_END
#define WALNUT_GPU_NAMESPACE_BEGIN
#define WALNUT_GPU_NAMESPACE_END
#define WALNUT_ROW_MAJOR row_major
#else
#include <DirectXMath.h>

#include <cstdint>
#define WALNUT_CONST(name, value) inline constexpr uint32_t name = (value)
#define WALNUT_NAMESPACE_BEGIN namespace walnut {
#define WALNUT_NAMESPACE_END }
#define WALNUT_GPU_NAMESPACE_BEGIN namespace walnut::gpu {
#define WALNUT_GPU_NAMESPACE_END }
#define WALNUT_ROW_MAJOR
namespace walnut::gpu
{
    using uint = std::uint32_t;
    using float2 = DirectX::XMFLOAT2;
    using float3 = DirectX::XMFLOAT3;
    using float4 = DirectX::XMFLOAT4;
    using float4x4 = DirectX::XMFLOAT4X4;
}
#endif

#endif