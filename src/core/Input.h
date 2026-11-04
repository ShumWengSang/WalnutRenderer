#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace walnut
{
    enum class MouseButton : uint8_t
    {
        Left,
        Right,
        Middle,
        X1,
        X2,
        Count,
    };

    // Keyboard and mouse state gathered from Win32 messages.
    // Keys are Win32 virtual-key codes (VK_*, 'A'..'Z', '0'..'9').
    //
    // Per-frame usage:
    //   input.BeginFrame();      // reset pressed/released flags and deltas
    //   window.PumpMessages();   // feeds HandleMessage()
    //   ... query state ...
    //
    // Queries return "nothing pressed" while the UI has captured that device
    // (see SetUICapture); the Raw* queries ignore UI capture.
    class Input
    {
    public:
        static constexpr int kKeyCount = 256;

        void RegisterRawMouse(HWND hwnd);
        void BeginFrame();
        void HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void ClearState();

        void SetUICapture(bool keyboardCaptured, bool mouseCaptured);
        bool IsKeyboardCapturedByUI() const { return m_keyboardCaptured; }
        bool IsMouseCapturedByUI() const { return m_mouseCaptured; }

        bool IsKeyDown(int vk) const { return !m_keyboardCaptured && IsKeyDownRaw(vk); }
        bool WasKeyPressed(int vk) const { return !m_keyboardCaptured && ValidKey(vk) && m_keyPressed[vk]; }
        bool WasKeyReleased(int vk) const { return !m_keyboardCaptured && ValidKey(vk) && m_keyReleased[vk]; }
        bool IsKeyDownRaw(int vk) const { return ValidKey(vk) && m_keyDown[vk]; }

        bool IsMouseDown(MouseButton b) const { return !m_mouseCaptured && IsMouseDownRaw(b); }
        bool WasMousePressed(MouseButton b) const { return !m_mouseCaptured && m_mousePressed[Index(b)]; }
        bool WasMouseReleased(MouseButton b) const { return !m_mouseCaptured && m_mouseReleased[Index(b)]; }
        bool IsMouseDownRaw(MouseButton b) const { return m_mouseDown[Index(b)]; }

        // Cursor position in client-area pixels.
        int GetMouseX() const { return m_mouseX; }
        int GetMouseY() const { return m_mouseY; }

        // Raw (unaccelerated) mouse movement accumulated this frame, from WM_INPUT.
        int GetMouseDeltaX() const { return m_mouseCaptured ? 0 : m_mouseDeltaX; }
        int GetMouseDeltaY() const { return m_mouseCaptured ? 0 : m_mouseDeltaY; }
        int GetMouseDeltaXRaw() const { return m_mouseDeltaX; }
        int GetMouseDeltaYRaw() const { return m_mouseDeltaY; }

        // Wheel movement this frame in notches (+ is away from the user).
        float GetMouseWheel() const { return m_mouseCaptured ? 0.0f : m_wheel; }

    private:
        static bool ValidKey(int vk) { return vk >= 0 && vk < kKeyCount; }
        static size_t Index(MouseButton b) { return static_cast<size_t>(b); }

        void SetKey(int vk, bool down);
        void SetMouseButton(MouseButton button, bool down);

        std::array<bool, kKeyCount> m_keyDown{};
        std::array<bool, kKeyCount> m_keyPressed{};
        std::array<bool, kKeyCount> m_keyReleased{};

        static constexpr size_t kButtonCount = static_cast<size_t>(MouseButton::Count);
        std::array<bool, kButtonCount> m_mouseDown{};
        std::array<bool, kButtonCount> m_mousePressed{};
        std::array<bool, kButtonCount> m_mouseReleased{};

        int m_mouseX = 0;
        int m_mouseY = 0;
        int m_mouseDeltaX = 0;
        int m_mouseDeltaY = 0;
        float m_wheel = 0.0f;

        bool m_keyboardCaptured = false;
        bool m_mouseCaptured = false;
    };
}
