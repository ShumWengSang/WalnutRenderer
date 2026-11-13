#include "renderer/FrameConstants.h"

#include "renderer/PointShadowMaps.h"
#include "scene/Camera.h"
#include "scene/Lighting.h"

#include <algorithm>
#include <cmath>

namespace walnut
{
    FrameConstants BuildFrameConstants(const FrameInputs& in)
    {
        using namespace DirectX;
        FrameConstants c{};
        const auto scaled = [](const XMFLOAT3& color, float intensity) {
            return XMFLOAT3{ color.x * intensity, color.y * intensity, color.z * intensity };
        };

        // Camera. Clustered lighting needs view space and the field of view too.
        const XMMATRIX viewProjection = in.camera.GetViewProjectionMatrix(in.aspect);
        XMStoreFloat4x4(&c.camera.viewProjection, viewProjection);
        XMStoreFloat4x4(&c.camera.inverseViewProjection, XMMatrixInverse(nullptr, viewProjection));
        XMStoreFloat4x4(&c.camera.view, in.camera.GetViewMatrix());
        c.camera.position = in.camera.position;
        c.camera.tanHalfFovY = std::tan(in.camera.verticalFov * 0.5f);
        c.camera.tanHalfFovX = c.camera.tanHalfFovY * in.aspect;

        c.sun.direction = in.lighting.GetSunDirection();
        c.sun.illuminance = scaled(in.lighting.sunColor, in.lighting.sunIntensity);

        const SunShadowSettings& sunShadows = in.settings.sunShadows;
        XMStoreFloat4x4(&c.sunShadow.viewProjection, in.lighting.GetSunViewProjection(in.sceneBounds));
        c.sunShadow.mapIndex = in.shadowMapIndex;
        c.sunShadow.enabled = in.frame.sunShadows ? 1u : 0u;
        c.sunShadow.raytraced = in.frame.sunRaytraced ? 1u : 0u;
        c.sunShadow.filterRadius = sunShadows.filterRadius < 0 ? UINT32_MAX : static_cast<uint32_t>(sunShadows.filterRadius);
        const float shadowExtent = GetSunShadowExtent(in.sceneBounds);
        c.sunShadow.texelWorldSize = shadowExtent / static_cast<float>(in.shadowMapSize);
        c.sunShadow.depthPerMeter = 1.0f / shadowExtent;
        c.sunShadow.constantBias = sunShadows.constantBias;
        c.sunShadow.slopeBias = sunShadows.slopeBias;
        c.sunShadow.normalOffset = sunShadows.normalOffset;

        c.ambient.skyRadiance = scaled(in.lighting.skyColor, in.lighting.ambientIntensity);
        c.ambient.groundRadiance = scaled(in.lighting.groundColor, in.lighting.ambientIntensity);

        c.shading.model = static_cast<uint32_t>(in.frame.shading);
        c.shading.roughnessOverride = in.debug.roughnessOverride;
        c.shading.metallicOverride = in.debug.metallicOverride;

        c.points.bufferIndex = in.pointLightBufferIndex;
        c.points.count = in.pointLightCount;
        c.points.shadowTechnique = static_cast<uint32_t>(in.frame.pointShadows);
        c.points.shadowMapIndex = in.pointShadowMapIndex;
        c.points.shadowNearScale = PointShadowMaps::kNearScale;

        // The cluster slices span the near plane to the farthest corner of the scene's bounds: nothing visible is
        // farther.
        c.clusters.nearDepth = in.camera.nearZ;
        c.clusters.farDepth = std::max(in.camera.nearZ * 2.0f, in.sceneBounds.FarthestCornerDistance(in.camera.position));
        c.clusters.gridIndex = in.clusterGridIndex;
        c.clusters.lightIndicesIndex = in.clusterLightIndicesIndex;
        c.clusters.enabled = in.settings.clusteredLighting ? 1u : 0u;

        c.scene.vertexBufferIndex = in.sceneBindings.vertices;
        c.scene.instanceBufferIndex = in.sceneBindings.instances;
        c.scene.materialBufferIndex = in.sceneBindings.materials;
        c.scene.indexBufferIndex = in.sceneBindings.indices;
        c.scene.tlasIndex = in.tlasIndex;
        c.scene.rayAlphaTest = in.frame.alphaTest ? 1u : 0u;
        return c;
    }
}