#include "core/Window.h"

#include <stdexcept>

namespace walnut
{
    namespace
    {
        constexpr wchar_t kWindowClassName[] = L"WalnutRendererWindow";
        constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
        constexpr DWORD kWindowExStyle = WS_EX_APPWINDOW;
    }

    Window::Window(const WindowDesc& desc)
    {
        m_instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Window::WndProcSetup;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kWindowClassName;
        if (!RegisterClassExW(&wc))
            throw std::runtime_error("RegisterClassExW failed");

        // Scale the requested client size by the primary monitor's DPI.
        const UINT dpi = GetDpiForSystem();
        RECT rect{ 0, 0,
                   MulDiv(static_cast<int>(desc.width), static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
                   MulDiv(static_cast<int>(desc.height), static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI) };
        AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, kWindowExStyle, dpi);

        m_hwnd = CreateWindowExW(kWindowExStyle, kWindowClassName, desc.title.c_str(), kWindowStyle,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                 nullptr, nullptr, m_instance, this);
        if (!m_hwnd)
            throw std::runtime_error("CreateWindowExW failed");

        RECT client{};
        GetClientRect(m_hwnd, &client);
        m_width = static_cast<uint32_t>(client.right - client.left);
        m_height = static_cast<uint32_t>(client.bottom - client.top);
        m_resizePending = false;

        ShowWindow(m_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(m_hwnd);
    }

    Window::~Window()
    {
        if (m_hwnd)
            DestroyWindow(m_hwnd);
        UnregisterClassW(kWindowClassName, m_instance);
    }

    bool Window::PumpMessages()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                m_open = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return m_open;
    }

    void Window::AddMessageHandler(MessageHandler handler)
    {
        m_handlers.push_back(std::move(handler));
    }

    bool Window::ConsumeResize(uint32_t& width, uint32_t& height)
    {
        if (!m_resizePending || m_minimized || m_width == 0 || m_height == 0)
            return false;
        m_resizePending = false;
        width = m_width;
        height = m_height;
        return true;
    }

    float Window::GetDpiScale() const
    {
        return static_cast<float>(GetDpiForWindow(m_hwnd)) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    }

    void Window::SetTitle(const std::wstring& title)
    {
        SetWindowTextW(m_hwnd, title.c_str());
    }

    LRESULT CALLBACK Window::WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // Stash the Window* passed to CreateWindowExW, then switch to the regular thunk.
        if (msg == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* window = static_cast<Window*>(create->lpCreateParams);
            window->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::WndProcThunk));
            return window->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return window->HandleMessage(hwnd, msg, wParam, lParam);
    }

    LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        for (const MessageHandler& handler : m_handlers)
        {
            if (handler(hwnd, msg, wParam, lParam))
                return 0;
        }

        switch (msg)
        {
        case WM_SIZE:
        {
            m_minimized = (wParam == SIZE_MINIMIZED);
            const uint32_t width = LOWORD(lParam);
            const uint32_t height = HIWORD(lParam);
            if (!m_minimized && (width != m_width || height != m_height))
            {
                m_width = width;
                m_height = height;
                m_resizePending = true;
            }
            return 0;
        }
        case WM_DPICHANGED:
        {
            // Resize to the rect Windows suggests for the new monitor DPI.
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { 320, 240 };
            return 0;
        }
        case WM_SYSCOMMAND:
            // Don't let a lone ALT press open the window menu and freeze rendering.
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            m_hwnd = nullptr;
            m_open = false;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
