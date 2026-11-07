#pragma once

namespace walnut
{
    class Camera;
    class Input;

    // Free-flying "spectator" controls on top of a Camera:
    //   hold right mouse + move mouse : look around (raw mouse deltas)
    //   W/S, A/D                       : move forward/back, strafe left/right (along where you look)
    //   Q/E                            : move down/up (world Y)
    //   Shift                          : move faster
    //   mouse wheel                    : change base speed
    //
    // Input already ignores keys and mouse while ImGui is using them, so typing in a UI field
    // doesn't move the camera.
    class FlyCameraController
    {
    public:
        void Update(Camera& camera, const Input& input, float deltaSeconds);

        float moveSpeed = 2.0f;          // world units (meters) per second
        float boostMultiplier = 4.0f;    // while Shift is held
        float lookSensitivity = 0.0025f; // radians per raw mouse count
    };
}
