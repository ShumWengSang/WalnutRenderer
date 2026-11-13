#include "scene/Lighting.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace walnut
{
    XMFLOAT3 SceneLighting::GetSunDirection() const
    {
        // Spherical angles to a direction, the same way Camera turns yaw/pitch into a forward vector.
        const float cosElevation = std::cos(sunElevation);
        return { cosElevation * std::sin(sunAzimuth), std::sin(sunElevation), cosElevation * std::cos(sunAzimuth) };
    }

    float GetSunShadowExtent(const Aabb& bounds)
    {
        return std::max(bounds.Diagonal(), 0.01f);
    }

    XMMATRIX SceneLighting::GetSunViewProjection(const Aabb& bounds) const
    {
        const float extent = GetSunShadowExtent(bounds);
        const float radius = extent * 0.5f;
        const XMVECTOR center = (XMLoadFloat3(&bounds.min) + XMLoadFloat3(&bounds.max)) * 0.5f;
        const XMFLOAT3 sunDirection = GetSunDirection();
        const XMVECTOR towardsSun = XMLoadFloat3(&sunDirection);

        // Stand on the sphere's surface on the sun's side, looking back through the center. Any "up" works for a
        // square shadow map, as long as it isn't parallel to the view direction (sun straight overhead).
        const XMVECTOR eye = center + towardsSun * radius;
        const XMVECTOR up = std::abs(sunDirection.y) > 0.99f ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMMATRIX view = XMMatrixLookToLH(eye, -towardsSun, up);

        // Orthographic: x and y map linearly to the map, depth maps linearly from the eye (distance 0) to the far
        // side of the sphere (distance 2 * radius). Passing near and far swapped makes it reversed-Z: depth 1 at
        // the eye, 0 at the far side, so the shadow map uses the same GREATER-is-closer rule as the main depth.
        const XMMATRIX projection = XMMatrixOrthographicLH(extent, extent, extent /*distance mapped to depth 0*/, 0.0f /*distance mapped to depth 1*/);
        return view * projection;
    }
}
