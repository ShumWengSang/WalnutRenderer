#include "scene/Camera.h"

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
}
