#pragma once

#include "scene/Bounds.h"

#include <DirectXMath.h>

namespace walnut
{
    // The scene's light (what the world contains; how it's rendered is RenderSettings): one sun (a directional light: infinitely far away, so every point receives it from the
    // same direction) and an ambient sky/ground term standing in for all the light bouncing around.
    struct SceneLighting
    {
        // Where the sun is in the sky. Elevation: angle above the horizon. Azimuth: angle around +Y, 0 = towards +Z.
        float sunAzimuth = DirectX::XMConvertToRadians(-35.0f);
        float sunElevation = DirectX::XMConvertToRadians(65.0f);
        DirectX::XMFLOAT3 sunColor = { 1.0f, 0.95f, 0.85f }; // linear, slightly warm
        float sunIntensity = 8.0f;
        // Ambient: light from the sky above the horizon, and from the ground (sunlight bounced off it) below.
        DirectX::XMFLOAT3 skyColor = { 0.55f, 0.65f, 0.85f };    // linear, sky blue
        DirectX::XMFLOAT3 groundColor = { 0.40f, 0.35f, 0.28f }; // linear, warm stone
        float ambientIntensity = 0.25f;

        // Point lights (PointLights): how many of the generated lamps to use, and their shared range and intensity.
        // Range and intensity are set to fit the scene's size when it loads.
        int pointLightCount = 128;
        float pointLightRange = 4.0f;     // meters
        float pointLightIntensity = 8.0f; // at 1 m, a surface facing the lamp receives this much (falls off as 1/d^2)
        bool animatePointLights = true;

        // Scales the point lights' range and intensity to the scene: Sponza (37 m across) gets ~2.6 m of range, small
        // pools of light that only overlap where lamps are close. Intensity grows with range^2, so the pools look
        // the same at any scale.
        void FitToScene(const Aabb& bounds);

        // Unit vector from a surface towards the sun.
        DirectX::XMFLOAT3 GetSunDirection() const;

        // The camera the sun "sees" the scene with, for the shadow map. A directional light's rays are parallel, so
        // the projection is orthographic (a box, not a pyramid). The box is the bounding sphere of the given world
        // bounds, viewed along the sun direction: it holds the whole scene for any sun angle, and doesn't depend on
        // the player camera (so the shadows don't shimmer when it moves). Reversed-Z, like the main camera.
        DirectX::XMMATRIX GetSunViewProjection(const Aabb& bounds) const;
    };

    // Size of the sun camera's box (its width, height and depth) for the given bounds: the bounding sphere's diameter.
    float GetSunShadowExtent(const Aabb& bounds);
}
