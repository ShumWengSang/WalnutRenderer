#pragma once

#include "renderer/RenderSettings.h"
#include "renderer/SceneGpuData.h"
#include "scene/Bounds.h"
#include "shared/FrameConstants.h"

#include <cstdint>

namespace walnut
{
    class Camera;
    struct SceneLighting;

    // What the frame constants are made from: the settings, and where this frame's resources are in the bindless heap
    // (UINT32_MAX where one doesn't exist).
    struct FrameInputs
    {
        const Camera& camera;
        float aspect; // width / height of the back buffer
        const SceneLighting& lighting;
        const RenderSettings& settings;
        const DebugSettings& debug;
        const FrameSettings& frame;
        Aabb sceneBounds;

        uint32_t shadowMapIndex;
        uint32_t shadowMapSize; // texels per side
        uint32_t tlasIndex = UINT32_MAX;
        SceneGpuData::Bindings sceneBindings{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        uint32_t pointLightBufferIndex = UINT32_MAX;
        uint32_t pointLightCount = 0;
        uint32_t pointShadowMapIndex = UINT32_MAX;
        uint32_t clusterGridIndex = UINT32_MAX;
        uint32_t clusterLightIndicesIndex = UINT32_MAX;
    };

    // Fills every field of the frame constants (shaders/shared/FrameConstants.h). Colors are multiplied by their
    // intensity here, so shaders only see the resulting light amounts.
    FrameConstants BuildFrameConstants(const FrameInputs& inputs);
}