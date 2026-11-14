// Editor-only stand-ins (never compiled by DXC). The HLSL Tools extension (configured by shaders/shadertoolsconfig.json)
// predates Shader Model 6.5/6.6, so it doesn't know bindless heap indexing, fields read through ConstantBuffer<T>, or
// inline ray tracing. Its config defines WALNUT_INTELLISENSE, and these declarations stand in for them, so the editor
// can check and complete the rest of the code. They only need the right names and signatures.
//
// shadertoolsconfig.json sets the include root (shaders/) and the defines DXC would see (__HLSL_VERSION, which picks
// the HLSL side of shared/*.h; RAYTRACED_SHADOWS=1, so the ray-traced code is checked too). It must not contain
// comments: the extension then ignores the whole file.
#ifndef WALNUT_INTELLISENSE_HLSLI
#define WALNUT_INTELLISENSE_HLSLI

// ResourceDescriptorHeap[index] (SM 6.6). The editor doesn't check what it's assigned to.
static const uint ResourceDescriptorHeap[1];

// RayQuery<FLAGS> (SM 6.5): ShadowRays.hlsli declares this type instead under WALNUT_INTELLISENSE.
static const uint RAY_FLAG_NONE = 0x00;
static const uint RAY_FLAG_FORCE_OPAQUE = 0x01;
static const uint RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH = 0x04;
static const uint RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES = 0x200;
static const uint COMMITTED_NOTHING = 0;
static const uint COMMITTED_TRIANGLE_HIT = 1;

struct RayQueryStandIn
{
    void TraceRayInline(RaytracingAccelerationStructure scene, uint rayFlags, uint instanceMask, RayDesc ray) {}
    bool Proceed() { return false; }
    uint CandidateInstanceID() { return 0; }
    uint CandidatePrimitiveIndex() { return 0; }
    float2 CandidateTriangleBarycentrics() { return float2(0.0f, 0.0f); }
    void CommitNonOpaqueTriangleHit() {}
    uint CommittedStatus() { return COMMITTED_NOTHING; }
};

#endif
