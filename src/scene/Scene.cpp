#include "scene/Scene.h"

#include "core/Log.h"

namespace walnut
{
    void LogSceneStats(const Scene& scene)
    {
        constexpr double kMB = 1024.0 * 1024.0;
        log::Info("  {} instances of {} primitives{}", scene.instances.size(), scene.primitives.size(),
                  scene.skippedPrimitives ? std::format(" ({} skipped)", scene.skippedPrimitives) : "");
        size_t blend = 0;
        for (const SceneMaterial& material : scene.materials)
            blend += material.alphaMode == AlphaMode::Blend;
        if (blend)
            log::Warn("  {} alpha-blend materials are drawn as opaque (no transparency pass yet)", blend);
        log::Info("  Merged geometry: {} vertices ({:.1f} MB), {} indices ({:.1f} MB, 32-bit)",
                  scene.vertices.size(), static_cast<double>(scene.vertices.size() * sizeof(SceneVertex)) / kMB,
                  scene.indices.size(), static_cast<double>(scene.indices.size() * sizeof(uint32_t)) / kMB);
        const DirectX::XMFLOAT3& lo = scene.boundsMin;
        const DirectX::XMFLOAT3& hi = scene.boundsMax;
        log::Info("  World bounds: ({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f}), size {:.2f} x {:.2f} x {:.2f}",
                  lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
        if (scene.mirroredInstances)
            log::Warn("  {} instances have mirrored (negative-determinant) transforms", scene.mirroredInstances);
    }
}
