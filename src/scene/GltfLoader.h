#pragma once

#include "scene/Scene.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct cgltf_data;

namespace walnut
{
    // Owns a parsed glTF file: the JSON turned into cgltf structs, plus the .bin buffers it points into.
    class GltfDocument
    {
    public:
        // Parses the .gltf/.glb, loads its buffers and validates it (for example that every accessor fits inside
        // its buffer). Returns null and sets 'error' on failure.
        static std::unique_ptr<GltfDocument> Load(const std::filesystem::path& path, std::string& error);
        ~GltfDocument();

        GltfDocument(const GltfDocument&) = delete;
        GltfDocument& operator=(const GltfDocument&) = delete;

        const cgltf_data& GetData() const { return *m_data; }
        const std::filesystem::path& GetPath() const { return m_path; }

    private:
        GltfDocument(std::filesystem::path path, cgltf_data* data) : m_path(std::move(path)), m_data(data) {}

        std::filesystem::path m_path;
        cgltf_data* m_data = nullptr;
    };

    // What a glTF file contains, gathered for the log and the UI.
    struct GltfSummary
    {
        // Top-level arrays of the file.
        size_t scenes = 0, nodes = 0, meshes = 0, materials = 0, textures = 0, images = 0, samplers = 0;
        size_t accessors = 0, bufferViews = 0, buffers = 0;
        uint64_t bufferBytes = 0;

        // Node hierarchy of the default scene.
        size_t rootNodes = 0, meshNodes = 0, maxDepth = 0;

        // Geometry, summed over every primitive of every mesh (each counted once, however many nodes use it).
        size_t primitives = 0, triangleListPrimitives = 0, indexedPrimitives = 0;
        size_t withNormals = 0, withTangents = 0, withUv0 = 0, withColors = 0;
        size_t indices16Bit = 0, indices32Bit = 0;
        uint64_t vertices = 0, indices = 0;

        // Materials and the textures they reference.
        size_t opaque = 0, alphaMask = 0, alphaBlend = 0, doubleSided = 0;
        size_t baseColorMaps = 0, normalMaps = 0, metallicRoughnessMaps = 0, occlusionMaps = 0, emissiveMaps = 0;
        size_t jpegImages = 0, pngImages = 0, otherImages = 0;

        std::vector<std::string> extensionsUsed;
    };

    GltfSummary SummarizeGltf(const cgltf_data& data);
    void LogGltfSummary(const GltfSummary& summary);

    // Converts a glTF file into the engine's Scene: merged vertex/index arrays, one ScenePrimitive per glTF
    // primitive and one SceneInstance per (node, primitive) pair, all in the engine's left-handed space. Images
    // that materials use are decoded (relative URIs resolved against 'baseDirectory') and given mip chains.
    Scene BuildSceneFromGltf(const cgltf_data& data, const std::filesystem::path& baseDirectory);

    // A scene file after loading: what it contained (for the log and the UI) and how long it took.
    struct SceneFileInfo
    {
        std::filesystem::path path;
        std::optional<GltfSummary> summary; // set when the file parsed
        std::string error;                  // why it didn't, otherwise
        double loadMs = 0.0;  // parsing the file
        double buildMs = 0.0; // converting it into a Scene (decoding images included)
    };

    struct LoadedScene
    {
        Scene scene; // empty if the file couldn't be loaded
        SceneFileInfo info;
    };

    // Parses a glTF file, logs what it contains and converts it into a Scene, kept on the CPU (the GPU doesn't see it
    // yet). The glTF document is freed before returning: everything needed was copied out of it.
    LoadedScene LoadSceneFile(const std::filesystem::path& path);
}
