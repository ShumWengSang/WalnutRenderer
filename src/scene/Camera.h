#pragma once

#include <DirectXMath.h>

namespace walnut
{
    // A perspective camera described by a position and two angles.
    //
    // Conventions (Direct3D / DirectXMath):
    //  - Left-handed coordinates: +X right, +Y up, +Z forward (into the screen).
    //  - Row vectors: a point is transformed as  p' = p * M,  so matrices chain left to right:
    //      clip = position * World * View * Projection
    class Camera
    {
    public:
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, -2.0f };
        float yaw = 0.0f;   // radians around +Y; 0 looks down +Z, positive turns right
        float pitch = 0.0f; // radians; positive looks up
        float verticalFov = DirectX::XMConvertToRadians(60.0f);
        float nearZ = 0.1f; // the only clip distance: the projection has no far plane (see Camera.cpp)

        // Unit vector the camera looks along.
        DirectX::XMVECTOR GetForward() const;

        // World -> view space: moves the world so the camera sits at the origin looking down +Z.
        DirectX::XMMATRIX GetViewMatrix() const;

        // View -> clip space, reversed-Z with an infinite far plane. 'aspect' is width / height.
        DirectX::XMMATRIX GetProjectionMatrix(float aspect) const;

        DirectX::XMMATRIX GetViewProjectionMatrix(float aspect) const
        {
            return DirectX::XMMatrixMultiply(GetViewMatrix(), GetProjectionMatrix(aspect));
        }
    };
}
