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

    // How a material's alpha is used (glTF alphaMode). Values must match scene.hlsl.
    enum class AlphaMode : uint32_t
    {
        Opaque = 0, // alpha ignored
        Mask = 1,   // pixels with alpha < alphaCutoff are discarded (leaves, chains, fences)
        Blend = 2,  // see-through; needs sorted, blended drawing (drawn as opaque for now)
    };

    // A glTF metallic-roughness material: the constant factors for now (textures come next).
    struct SceneMaterial
    {
        DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f }; // linear RGB + alpha
        DirectX::XMFLOAT3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
        float metallicFactor = 1.0f;  // glTF defaults: fully metallic and fully rough
        float roughnessFactor = 1.0f;
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        bool doubleSided = false; // draw back faces too (not culled), with their normal flipped
    };

    // One glTF primitive: a range of the merged index array, drawn with one material.
    struct ScenePrimitive
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t firstVertex = 0; // the vertices its indices point at; the indices already include this offset
        uint32_t vertexCount = 0;
        uint32_t materialIndex = 0; // into Scene::materials; always valid
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
        std::vector<SceneMaterial> materials; // the file's materials, plus a default one if some primitive had none
        std::vector<ScenePrimitive> primitives;
        std::vector<SceneInstance> instances;
        DirectX::XMFLOAT3 boundsMin{}, boundsMax{}; // world space, over every instance

        uint32_t skippedPrimitives = 0; // not triangle lists, or without positions
        uint32_t mirroredInstances = 0; // world matrix with a negative determinant: front faces flip winding
    };

    void LogSceneStats(const Scene& scene);
}
