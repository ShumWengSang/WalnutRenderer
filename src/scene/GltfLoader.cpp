#include "scene/GltfLoader.h"

#include "core/Log.h"
#include "core/StringUtils.h"

#include <cgltf.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>

namespace walnut
{
    namespace
    {
        const char* ResultToString(cgltf_result result)
        {
            switch (result)
            {
            case cgltf_result_success:         return "success";
            case cgltf_result_data_too_short:  return "data too short";
            case cgltf_result_unknown_format:  return "unknown format";
            case cgltf_result_invalid_json:    return "invalid JSON";
            case cgltf_result_invalid_gltf:    return "invalid glTF";
            case cgltf_result_invalid_options: return "invalid options";
            case cgltf_result_file_not_found:  return "file not found";
            case cgltf_result_io_error:        return "I/O error";
            case cgltf_result_out_of_memory:   return "out of memory";
            case cgltf_result_legacy_gltf:     return "glTF 1.0 is not supported";
            default:                           return "unknown error";
            }
        }

        // cgltf's default reader passes its UTF-8 paths to fopen(), which reads them in the ANSI code page and
        // fails on non-ASCII folders. This one converts to UTF-16 first. Used for the .gltf and every .bin it loads.
        cgltf_result ReadFileUtf8(const cgltf_memory_options* memory, const cgltf_file_options*, const char* path,
                                  cgltf_size* size, void** data)
        {
            std::ifstream file(std::filesystem::path(ToWide(path)), std::ios::binary | std::ios::ate);
            if (!file)
                return cgltf_result_file_not_found;
            const auto fileSize = static_cast<size_t>(file.tellg());
            file.seekg(0);

            void* buffer = memory->alloc_func ? memory->alloc_func(memory->user_data, fileSize) : std::malloc(fileSize);
            if (!buffer)
                return cgltf_result_out_of_memory;
            if (!file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(fileSize)))
            {
                memory->free_func ? memory->free_func(memory->user_data, buffer) : std::free(buffer);
                return cgltf_result_io_error;
            }
            *size = fileSize;
            *data = buffer;
            return cgltf_result_success;
        }

        void ReleaseFile(const cgltf_memory_options* memory, const cgltf_file_options*, void* data)
        {
            memory->free_func ? memory->free_func(memory->user_data, data) : std::free(data);
        }

        std::string ToLower(std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }
    }

    std::unique_ptr<GltfDocument> GltfDocument::Load(const std::filesystem::path& path, std::string& error)
    {
        cgltf_options options{};
        options.file.read = &ReadFileUtf8;
        options.file.release = &ReleaseFile;

        const std::string pathUtf8 = ToUtf8(path.wstring());
        cgltf_data* data = nullptr;
        // 1. Parse the JSON (or the JSON chunk of a .glb) into cgltf structs. Indices into other arrays become pointers.
        if (const cgltf_result result = cgltf_parse_file(&options, pathUtf8.c_str(), &data); result != cgltf_result_success)
        {
            error = std::format("parsing failed: {}", ResultToString(result));
            return nullptr;
        }
        auto document = std::unique_ptr<GltfDocument>(new GltfDocument(path, data));

        // 2. Load each buffer: a .bin file next to the .gltf, base64 data inside the JSON, or the .glb's binary chunk.
        if (const cgltf_result result = cgltf_load_buffers(&options, data, pathUtf8.c_str()); result != cgltf_result_success)
        {
            error = std::format("loading buffers failed: {}", ResultToString(result));
            return nullptr;
        }

        // 3. Check what parsing doesn't: accessors fit in their buffer views, views fit in their buffers, index
        //    values are in range, and so on. Later steps copy raw bytes using these numbers, so they must be valid.
        if (const cgltf_result result = cgltf_validate(data); result != cgltf_result_success)
        {
            error = std::format("validation failed: {}", ResultToString(result));
            return nullptr;
        }
        return document;
    }

    GltfDocument::~GltfDocument()
    {
        cgltf_free(m_data);
    }

    GltfSummary SummarizeGltf(const cgltf_data& data)
    {
        GltfSummary s;
        s.scenes = data.scenes_count;
        s.nodes = data.nodes_count;
        s.meshes = data.meshes_count;
        s.materials = data.materials_count;
        s.textures = data.textures_count;
        s.images = data.images_count;
        s.samplers = data.samplers_count;
        s.accessors = data.accessors_count;
        s.bufferViews = data.buffer_views_count;
        s.buffers = data.buffers_count;
        for (size_t i = 0; i < data.buffers_count; ++i)
            s.bufferBytes += data.buffers[i].size;

        // Walk the node tree from the default scene's roots (or every parentless node if there are no scenes).
        auto visit = [&](auto& self, const cgltf_node& node, size_t depth) -> void {
            s.maxDepth = std::max(s.maxDepth, depth);
            if (node.mesh)
                ++s.meshNodes;
            for (size_t i = 0; i < node.children_count; ++i)
                self(self, *node.children[i], depth + 1);
        };
        const cgltf_scene* scene = data.scene ? data.scene : (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
        if (scene)
        {
            s.rootNodes = scene->nodes_count;
            for (size_t i = 0; i < scene->nodes_count; ++i)
                visit(visit, *scene->nodes[i], 1);
        }
        else
        {
            for (size_t i = 0; i < data.nodes_count; ++i)
                if (!data.nodes[i].parent)
                {
                    ++s.rootNodes;
                    visit(visit, data.nodes[i], 1);
                }
        }

        for (size_t m = 0; m < data.meshes_count; ++m)
        {
            const cgltf_mesh& mesh = data.meshes[m];
            for (size_t p = 0; p < mesh.primitives_count; ++p)
            {
                const cgltf_primitive& primitive = mesh.primitives[p];
                ++s.primitives;
                if (primitive.type == cgltf_primitive_type_triangles)
                    ++s.triangleListPrimitives;

                bool hasNormal = false, hasTangent = false, hasUv0 = false, hasColor = false;
                for (size_t a = 0; a < primitive.attributes_count; ++a)
                {
                    const cgltf_attribute& attribute = primitive.attributes[a];
                    switch (attribute.type)
                    {
                    case cgltf_attribute_type_position: s.vertices += attribute.data->count; break;
                    case cgltf_attribute_type_normal:   hasNormal = true; break;
                    case cgltf_attribute_type_tangent:  hasTangent = true; break;
                    case cgltf_attribute_type_texcoord: hasUv0 |= attribute.index == 0; break;
                    case cgltf_attribute_type_color:    hasColor = true; break;
                    default: break;
                    }
                }
                s.withNormals += hasNormal;
                s.withTangents += hasTangent;
                s.withUv0 += hasUv0;
                s.withColors += hasColor;

                if (primitive.indices)
                {
                    ++s.indexedPrimitives;
                    s.indices += primitive.indices->count;
                    if (primitive.indices->component_type == cgltf_component_type_r_16u)
                        ++s.indices16Bit;
                    else if (primitive.indices->component_type == cgltf_component_type_r_32u)
                        ++s.indices32Bit;
                }
            }
        }

        for (size_t i = 0; i < data.materials_count; ++i)
        {
            const cgltf_material& material = data.materials[i];
            switch (material.alpha_mode)
            {
            case cgltf_alpha_mode_mask:  ++s.alphaMask; break;
            case cgltf_alpha_mode_blend: ++s.alphaBlend; break;
            default:                     ++s.opaque; break;
            }
            s.doubleSided += material.double_sided;
            if (material.has_pbr_metallic_roughness)
            {
                s.baseColorMaps += material.pbr_metallic_roughness.base_color_texture.texture != nullptr;
                s.metallicRoughnessMaps += material.pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr;
            }
            s.normalMaps += material.normal_texture.texture != nullptr;
            s.occlusionMaps += material.occlusion_texture.texture != nullptr;
            s.emissiveMaps += material.emissive_texture.texture != nullptr;
        }

        for (size_t i = 0; i < data.images_count; ++i)
        {
            const cgltf_image& image = data.images[i];
            // Embedded images (in a buffer view) carry a MIME type; external ones only have a file name.
            const std::string kind = ToLower(image.mime_type ? image.mime_type : (image.uri ? image.uri : ""));
            if (kind.ends_with("jpeg") || kind.ends_with(".jpg"))
                ++s.jpegImages;
            else if (kind.ends_with("png"))
                ++s.pngImages;
            else
                ++s.otherImages;
        }

        for (size_t i = 0; i < data.extensions_used_count; ++i)
            s.extensionsUsed.emplace_back(data.extensions_used[i]);
        return s;
    }

    void LogGltfSummary(const GltfSummary& s)
    {
        log::Info("  Arrays: {} scenes, {} nodes, {} meshes, {} materials, {} textures, {} images, {} samplers",
                  s.scenes, s.nodes, s.meshes, s.materials, s.textures, s.images, s.samplers);
        log::Info("          {} accessors -> {} buffer views -> {} buffers ({:.1f} MB)",
                  s.accessors, s.bufferViews, s.buffers, static_cast<double>(s.bufferBytes) / (1024.0 * 1024.0));
        log::Info("  Nodes: {} roots, {} with a mesh, hierarchy depth {}", s.rootNodes, s.meshNodes, s.maxDepth);
        log::Info("  Primitives: {} ({} triangle lists, {} indexed: {} with 16-bit, {} with 32-bit indices)",
                  s.primitives, s.triangleListPrimitives, s.indexedPrimitives, s.indices16Bit, s.indices32Bit);
        log::Info("    {} vertices, {} indices ({} triangles)", s.vertices, s.indices, s.indices / 3);
        log::Info("    with NORMAL {}, TANGENT {}, TEXCOORD_0 {}, COLOR_0 {}",
                  s.withNormals, s.withTangents, s.withUv0, s.withColors);
        log::Info("  Materials: {} opaque, {} alpha-mask, {} alpha-blend, {} double-sided",
                  s.opaque, s.alphaMask, s.alphaBlend, s.doubleSided);
        log::Info("    maps: base color {}, normal {}, metallic-roughness {}, occlusion {}, emissive {}",
                  s.baseColorMaps, s.normalMaps, s.metallicRoughnessMaps, s.occlusionMaps, s.emissiveMaps);
        log::Info("  Images: {} JPEG, {} PNG, {} other", s.jpegImages, s.pngImages, s.otherImages);
        std::string extensions;
        for (const std::string& extension : s.extensionsUsed)
            extensions += (extensions.empty() ? "" : ", ") + extension;
        log::Info("  Extensions used: {}", extensions.empty() ? "none" : extensions);
    }
}
