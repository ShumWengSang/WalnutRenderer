// HLSL side of the bindless root signature (src/gfx/RootSignature.h). Keep the two in sync.
//
//   b0  root constants  per-draw values (e.g. descriptor indices); each shader declares its own
//                       constant buffer at b0 (WALNUT_CONSTANT_BUFFER) with up to 8 uints
//   b1  root CBV        per-draw data, e.g. a world matrix
//   b2  root CBV        per-frame data, e.g. camera and lights
//   s0..s6              static samplers declared below
//
// Textures and buffers are not bound at all: shaders index the descriptor heap directly with
// ResourceDescriptorHeap[index] (Shader Model 6.6).

#ifndef WALNUT_BINDLESS_HLSLI
#define WALNUT_BINDLESS_HLSLI

// WALNUT_CONSTANT_BUFFER(Type, name, register): a constant buffer whose contents are one struct, read as name.field.
// In the editor (WALNUT_INTELLISENSE, see common/IntelliSense.hlsli) it's a plain variable: the HLSL extension can't
// read fields through ConstantBuffer<T>. DXC never defines WALNUT_INTELLISENSE and compiles the real declaration.
#ifdef WALNUT_INTELLISENSE
#include "common/IntelliSense.hlsli"
#define WALNUT_CONSTANT_BUFFER(Type, name, slot) Type name
#else
#define WALNUT_CONSTANT_BUFFER(Type, name, slot) ConstantBuffer<Type> name : register(slot)
#endif

SamplerState g_samplerAnisotropicWrap : register(s0);
SamplerState g_samplerLinearClamp     : register(s1);
SamplerState g_samplerPointClamp      : register(s2);
SamplerState g_samplerLinearWrap      : register(s3); // trilinear: linear within a mip and between mips
SamplerState g_samplerPointWrap       : register(s4);
// SampleCmp*: compares a reference depth with the stored one ("reference >= stored", for reversed-Z shadow maps).
SamplerComparisonState g_samplerShadow       : register(s5); // nearest texel: 0 or 1
SamplerComparisonState g_samplerShadowLinear : register(s6); // 2x2 texels compared, results blended: 0..1

#endif
