#include "scene/FlyCameraController.h"

#include "core/Input.h"
#include "scene/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace DirectX;

namespace walnut
{
    namespace
    {
        // Just short of straight up/down: at exactly +-90 degrees the forward vector would be parallel to the
        // world "up" vector, and the view matrix (which builds "right" as cross(up, forward)) breaks down.
        constexpr float kMaxPitch = std::numbers::pi_v<float> * 0.5f - 0.01f;
    }

    void FlyCameraController::Update(Camera& camera, const Input& input, float deltaSeconds)
    {
        // Look: raw mouse deltas, not cursor position. They keep coming when the cursor hits the edge of the
        // screen and are not affected by Windows pointer acceleration.
        if (input.IsMouseDown(MouseButton::Right))
        {
            camera.yaw += static_cast<float>(input.GetMouseDeltaX()) * lookSensitivity;
            camera.pitch -= static_cast<float>(input.GetMouseDeltaY()) * lookSensitivity; // mouse down = look down
            camera.pitch = std::clamp(camera.pitch, -kMaxPitch, kMaxPitch);
            camera.yaw = std::remainder(camera.yaw, 2.0f * std::numbers::pi_v<float>);
        }

        // Speed: each wheel notch changes it by 20%, so it feels the same at 0.5 m/s and at 50 m/s.
        if (const float wheel = input.GetMouseWheel(); wheel != 0.0f)
            moveSpeed = std::clamp(moveSpeed * std::pow(1.2f, wheel), 0.05f, 500.0f);

        // Move: build a direction in camera-relative terms, then convert it to world space.
        float forwardAmount = 0.0f;
        float rightAmount = 0.0f;
        float upAmount = 0.0f;
        if (input.IsKeyDown('W')) forwardAmount += 1.0f;
        if (input.IsKeyDown('S')) forwardAmount -= 1.0f;
        if (input.IsKeyDown('D')) rightAmount += 1.0f;
        if (input.IsKeyDown('A')) rightAmount -= 1.0f;
        if (input.IsKeyDown('E')) upAmount += 1.0f;
        if (input.IsKeyDown('Q')) upAmount -= 1.0f;

        const XMVECTOR forward = camera.GetForward();
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // Left-handed: cross(up, forward) points right.
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));

        XMVECTOR direction = XMVectorScale(forward, forwardAmount);
        direction = XMVectorAdd(direction, XMVectorScale(right, rightAmount));
        direction = XMVectorAdd(direction, XMVectorScale(worldUp, upAmount));

        // Normalize so moving diagonally (W + D) isn't sqrt(2) times faster than moving straight.
        if (XMVectorGetX(XMVector3LengthSq(direction)) > 0.0f)
        {
            const float speed = moveSpeed * (input.IsKeyDown(VK_SHIFT) ? boostMultiplier : 1.0f);
            // Multiplying by deltaSeconds makes the speed independent of frame rate.
            const XMVECTOR step = XMVectorScale(XMVector3Normalize(direction), speed * deltaSeconds);
            XMStoreFloat3(&camera.position, XMVectorAdd(XMLoadFloat3(&camera.position), step));
        }
    }
}
