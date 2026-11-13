#pragma once

#include "shared/Constants.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace walnut
{
    // One point light as the shaders read it (32 bytes). Must match PointLight in shaders/common/PointLight.hlsli.
    struct PointLightGpu
    {
        DirectX::XMFLOAT3 position; // world space
        float range;                // meters: the light fades smoothly to exactly zero here
        DirectX::XMFLOAT3 intensity; // linear color * intensity: at distance d, a surface facing it receives intensity / d^2
        uint32_t shadowSlot = kNoShadowSlot; // which cube of PointShadowMaps holds its shadow
    };

    // A field of colored lamps scattered through a scene, for testing many lights. Positions and colors come from a
    // fixed random seed, so every run (and every light count) places the same lamps in the same spots.
    class PointLights
    {
    public:
        static constexpr uint32_t kMaxLights = 1024;

        // Places kMaxLights lamps inside the given bounds: across most of the floor area, between 10% and 45% of the
        // height (where the floor and walls are, not up in the roof).
        void Generate(const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax);

        // Writes the first 'count' lamps (at most kMaxLights) for this moment. 'animate' bobs each lamp up and down
        // by 15% of its range, each at its own phase.
        void Update(double timeSeconds, uint32_t count, float range, float intensity, bool animate,
                    std::vector<PointLightGpu>& out) const;

    private:
        struct Lamp
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 color; // linear, brightest channel 1
            float phase;             // radians
        };
        std::vector<Lamp> m_lamps;
    };
}
