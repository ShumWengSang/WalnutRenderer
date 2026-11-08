#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace walnut
{
    // One vertex of the merged vertex array; also the element the vertex shader will read (F4).
    struct SceneVertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;  // zero if the file has none
        DirectX::XMFLOAT4 tangent; // xyz = direction, w = bitangent sign (+1/-1); zero if the file has none
        DirectX::XMFLOAT2 uv;
    };
    static_assert(sizeof(SceneVertex) == 48);

    inline constexpr uint32_t kNoMaterial = UINT32_MAX;

    // One glTF primitive: a range of the merged index array, drawn with one material.
    struct ScenePrimitive
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t firstVertex = 0; // the vertices its indices point at; the indices already include this offset
        uint32_t vertexCount = 0;
        uint32_t materialIndex = kNoMaterial; // into the glTF materials array
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{}; // object space
    };

    // A primitive placed in the world. A node whose mesh has 3 primitives becomes 3 instances.
    struct SceneInstance
    {
        DirectX::XMFLOAT4X4 world; // row-vector convention (position * world), in the engine's left-handed space
        uint32_t primitiveIndex = 0;
    };

    // Everything needed to draw a scene, independent of the file format it came from. CPU memory only.
    struct Scene
    {
        std::vector<SceneVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<ScenePrimitive> primitives;
        std::vector<SceneInstance> instances;
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{}; // world space, over every instance

        uint32_t skippedPrimitives = 0; // not triangle lists, or without positions
        uint32_t mirroredInstances = 0; // world matrix with a negative determinant: front faces flip winding
    };

    void LogSceneStats(const Scene& scene);
}
