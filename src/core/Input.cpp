#include "core/Input.h"

#include "core/Log.h"

#include <windowsx.h>

namespace walnut
{
    void Input::RegisterRawMouse(HWND hwnd)
    {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
        device.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
        device.dwFlags = 0;
        device.hwndTarget = hwnd;
        if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
            log::Warn("RegisterRawInputDevices failed ({}); raw mouse deltas unavailable", GetLastError());
    }

    void Input::BeginFrame()
    {
        m_keyPressed.fill(false);
        m_keyReleased.fill(false);
        m_mousePressed.fill(false);
        m_mouseReleased.fill(false);
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_wheel = 0.0f;
    }

    void Input::ClearState()
    {
        for (int vk = 0; vk < kKeyCount; ++vk)
        {
            if (m_keyDown[vk])
                SetKey(vk, false);
        }
        for (size_t i = 0; i < kButtonCount; ++i)
        {
            if (m_mouseDown[i])
                SetMouseButton(static_cast<MouseButton>(i), false);
        }
    }

    void Input::SetUICapture(bool keyboardCaptured, bool mouseCaptured)
    {
        m_keyboardCaptured = keyboardCaptured;
        m_mouseCaptured = mouseCaptured;
    }

    void Input::SetKey(int vk, bool down)
    {
        if (!ValidKey(vk))
            return;
        if (down && !m_keyDown[vk])
            m_keyPressed[vk] = true;
        else if (!down && m_keyDown[vk])
            m_keyReleased[vk] = true;
        m_keyDown[vk] = down;
    }

    void Input::SetMouseButton(MouseButton button, bool down)
    {
        const size_t i = Index(button);
        if (down && !m_mouseDown[i])
            m_mousePressed[i] = true;
        else if (!down && m_mouseDown[i])
            m_mouseReleased[i] = true;
        m_mouseDown[i] = down;
    }

    void Input::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            SetKey(static_cast<int>(wParam), true);
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            SetKey(static_cast<int>(wParam), false);
            break;

        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetMouseButton(MouseButton::Left, true); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: SetMouseButton(MouseButton::Right, true); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: SetMouseButton(MouseButton::Middle, true); break;
        case WM_LBUTTONUP: SetMouseButton(MouseButton::Left, false); break;
        case WM_RBUTTONUP: SetMouseButton(MouseButton::Right, false); break;
        case WM_MBUTTONUP: SetMouseButton(MouseButton::Middle, false); break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
            SetMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2, true);
            break;
        case WM_XBUTTONUP:
            SetMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2, false);
            break;

        case WM_MOUSEMOVE:
            m_mouseX = GET_X_LPARAM(lParam);
            m_mouseY = GET_Y_LPARAM(lParam);
            break;
        case WM_MOUSEWHEEL:
            m_wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            break;

        case WM_INPUT:
        {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
                && raw.header.dwType == RIM_TYPEMOUSE
                && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                m_mouseDeltaX += raw.data.mouse.lLastX;
                m_mouseDeltaY += raw.data.mouse.lLastY;
            }
            break;
        }

        case WM_KILLFOCUS:
            // Key-up messages go to whichever window has focus, so release everything we hold.
            ClearState();
            break;
        case WM_ACTIVATEAPP:
            if (!wParam)
                ClearState();
            break;
        default:
            break;
        }
        (void)hwnd;
    }
}
