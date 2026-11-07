// HLSL side of the bindless root signature (src/gfx/RootSignature.h). Keep the two in sync.
//
//   b0  root constants  per-draw values (e.g. descriptor indices); each shader declares its own
//                       ConstantBuffer<T> at b0 with up to 8 uints
//   b1  root CBV        per-draw data, e.g. a world matrix
//   b2  root CBV        per-frame data, e.g. camera and lights
//   s0..s2              static samplers declared below
//
// Textures and buffers are not bound at all: shaders index the descriptor heap directly with
// ResourceDescriptorHeap[index] (Shader Model 6.6).

#ifndef WALNUT_BINDLESS_HLSLI
#define WALNUT_BINDLESS_HLSLI

SamplerState g_samplerAnisotropicWrap : register(s0);
SamplerState g_samplerLinearClamp     : register(s1);
SamplerState g_samplerPointClamp      : register(s2);

#endif
