#include "scene/PointLights.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace walnut
{
    namespace
    {
        // Hue (0..1) to a fully saturated linear color, then mixed 30% towards white: lamp-like, not neon.
        DirectX::XMFLOAT3 HueToColor(float hue)
        {
            const auto channel = [hue](float offset) {
                const float x = std::fabs(std::fmod(hue * 6.0f + offset, 6.0f) - 3.0f) - 1.0f;
                return std::clamp(x, 0.0f, 1.0f) * 0.7f + 0.3f;
            };
            return { channel(0.0f), channel(4.0f), channel(2.0f) };
        }
    }

    void PointLights::Generate(const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax)
    {
        std::mt19937 random(1234);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const auto between = [&](float a, float b) { return a + (b - a) * unit(random); };

        const DirectX::XMFLOAT3 size{ boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z };
        m_lamps.resize(kMaxLights);
        for (Lamp& lamp : m_lamps)
        {
            lamp.position = { between(boundsMin.x + 0.05f * size.x, boundsMax.x - 0.05f * size.x),
                              between(boundsMin.y + 0.10f * size.y, boundsMin.y + 0.45f * size.y),
                              between(boundsMin.z + 0.05f * size.z, boundsMax.z - 0.05f * size.z) };
            lamp.color = HueToColor(unit(random));
            lamp.phase = between(0.0f, DirectX::XM_2PI);
        }
    }

    void PointLights::Update(double timeSeconds, uint32_t count, float range, float intensity, bool animate,
                             std::vector<PointLightGpu>& out) const
    {
        count = std::min(count, static_cast<uint32_t>(m_lamps.size()));
        out.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Lamp& lamp = m_lamps[i];
            DirectX::XMFLOAT3 position = lamp.position;
            if (animate)
                position.y += 0.15f * range * std::sin(static_cast<float>(timeSeconds) * 1.3f + lamp.phase);
            out[i] = PointLightGpu{
                .position = position,
                .range = range,
                .intensity = { lamp.color.x * intensity, lamp.color.y * intensity, lamp.color.z * intensity },
            };
        }
    }
}
