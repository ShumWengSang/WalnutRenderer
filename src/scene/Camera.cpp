#include "scene/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace walnut
{
    XMVECTOR Camera::GetForward() const
    {
        // Spherical angles to a direction. yaw = pitch = 0 gives (0, 0, 1).
        const float cosPitch = std::cos(pitch);
        return XMVectorSet(cosPitch * std::sin(yaw), std::sin(pitch), cosPitch * std::cos(yaw), 0.0f);
    }

    XMMATRIX Camera::GetViewMatrix() const
    {
        const XMVECTOR eye = XMLoadFloat3(&position);
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        return XMMatrixLookToLH(eye, GetForward(), up);
    }

    XMMATRIX Camera::GetProjectionMatrix(float aspect) const
    {
        // A perspective projection maps view-space depth z to clip space so that after the divide by w,
        // NDC depth = clip.z / clip.w lands in [0, 1].
        //
        // Standard: near -> 0, far -> 1. Reversed-Z (used here): near -> 1, far -> 0.
        //
        // Why reverse? NDC depth is proportional to 1/z, so most of the [0, 1] range is spent close to the
        // camera. Floating-point numbers are also densest near 0. Standard Z puts both of these "precision
        // piles" at the near plane, leaving distant geometry with very few distinct depth values (z-fighting).
        // Reversed-Z puts the float precision near 0 = far away, where 1/z is starved, and the two effects
        // roughly cancel out. Paired with a 32-bit float depth buffer, precision is good at any distance.
        //
        // With reversed-Z we can also push the far plane to infinity: depth = near / z, which approaches 0
        // but never reaches it. Nothing is clipped for being too far away.
        //
        // Row-vector matrix (p' = p * M), with f = 1 / tan(fovY / 2):
        //
        //   [ f/aspect  0     0      0 ]      clip.x = x * f / aspect
        //   [ 0         f     0      0 ]      clip.y = y * f
        //   [ 0         0     0      1 ]      clip.z = near           (constant!)
        //   [ 0         0     near   0 ]      clip.w = z              (perspective divide by view depth)
        //
        //   NDC depth = clip.z / clip.w = near / z  ->  1 at z = near, -> 0 as z -> infinity.
        const float f = 1.0f / std::tan(verticalFov * 0.5f);
        return XMMATRIX(
            f / aspect, 0.0f, 0.0f,  0.0f,
            0.0f,       f,    0.0f,  0.0f,
            0.0f,       0.0f, 0.0f,  1.0f,
            0.0f,       0.0f, nearZ, 0.0f);
    }

    void FrameBounds(Camera& camera, const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax)
    {
        const XMVECTOR lo = XMLoadFloat3(&boundsMin);
        const XMVECTOR hi = XMLoadFloat3(&boundsMax);
        const XMVECTOR center = (lo + hi) * 0.5f;
        const float radius = std::max(XMVectorGetX(XMVector3Length(hi - lo)) * 0.5f, 0.01f);

        camera.yaw = 0.0f;
        camera.pitch = XMConvertToRadians(-25.0f);
        // A sphere of radius r just fits a cone with half-angle a at distance r / sin(a).
        const float distance = radius / std::sin(camera.verticalFov * 0.5f);
        XMStoreFloat3(&camera.position, center - camera.GetForward() * distance);
    }

    void PlaceStartCamera(Camera& camera, const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax)
    {
        const XMFLOAT3 size = { boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z };
        const float radius = 0.5f * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
        constexpr float kWalkInRadius = 5.0f; // meters; bigger than any single object, smaller than a room
        if (radius < kWalkInRadius)
        {
            FrameBounds(camera, boundsMin, boundsMax);
            return;
        }

        // Stand a fifth of the way in from one end (right at the end is often behind a wall), a quarter of the way up, looking down the longest horizontal axis.
        const bool alongX = size.x >= size.z;
        camera.position = { (boundsMin.x + boundsMax.x) * 0.5f, boundsMin.y + 0.25f * size.y, (boundsMin.z + boundsMax.z) * 0.5f };
        if (alongX)
            camera.position.x = boundsMin.x + 0.2f * size.x;
        else
            camera.position.z = boundsMin.z + 0.2f * size.z;
        camera.yaw = alongX ? XMConvertToRadians(90.0f) : 0.0f; // yaw 90 = looking down +X
        camera.pitch = XMConvertToRadians(5.0f);
    }
}
