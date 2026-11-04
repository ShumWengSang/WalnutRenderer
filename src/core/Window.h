#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace walnut
{
    struct WindowDesc
    {
        std::wstring title = L"WalnutRenderer";
        uint32_t width = 1280; // client-area size in physical pixels at 100% scaling
        uint32_t height = 720;
    };

    // Thin wrapper over a Win32 top-level window and its message pump.
    class Window
    {
    public:
        // Return true to mark the message as handled (DefWindowProc is then skipped).
        using MessageHandler = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;

        explicit Window(const WindowDesc& desc);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        // Processes all pending messages. Returns false once the window has been closed.
        bool PumpMessages();

        // Handlers run in registration order before the window's own handling.
        void AddMessageHandler(MessageHandler handler);

        // Returns true (once) if the client area changed size since the last call.
        bool ConsumeResize(uint32_t& width, uint32_t& height);

        HWND GetHandle() const { return m_hwnd; }
        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        bool IsMinimized() const { return m_minimized; }
        float GetDpiScale() const;

        void SetTitle(const std::wstring& title);

    private:
        static LRESULT CALLBACK WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd = nullptr;
        HINSTANCE m_instance = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_minimized = false;
        bool m_resizePending = false;
        bool m_open = true;
        std::vector<MessageHandler> m_handlers;
    };
}
