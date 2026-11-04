#pragma once

#include "gfx/D3D12Common.h"

namespace walnut
{
    class Window;
    class Renderer;

    // Dear ImGui with the official Win32 + DX12 backends.
    class ImGuiLayer
    {
    public:
        ImGuiLayer(Window& window, Renderer& renderer);
        ~ImGuiLayer();

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        void BeginFrame();
        void Render(ID3D12GraphicsCommandList* commandList);

        bool WantsKeyboard() const;
        bool WantsMouse() const;

    private:
        Renderer& m_renderer;
    };
}
