# WalnutRenderer

A small Direct3D 12 learning engine for Windows. It uses raw Win32 for the window and input, Dear ImGui for UI, and compiles HLSL at runtime with DXC.

## Requirements

- Windows 10/11 x64 with a D3D12 (feature level 12_0) GPU.
- Visual Studio Build Tools 2022 or newer, with the **Desktop development with C++** workload and a Windows SDK.
- CMake 3.26+ and Ninja.
- Optional, but needed for the D3D12 debug layer: the **Graphics Tools** Windows feature.
  Install it with *Settings → System → Optional features*, or from an elevated PowerShell:
  `Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0`

Install the tools with winget:

```powershell
winget install Kitware.CMake Ninja-build.Ninja
winget install Microsoft.VisualStudio.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

## Building

### VS Code
1. Install the recommended extensions: C/C++, CMake Tools, and HLSL Tools.
2. Open the folder and pick the **x64 Debug** configure preset. CMake Tools sets up the MSVC environment for you.
3. Press **F5**. This builds and launches under the debugger (`.vscode/launch.json`).

### Command line
In VS Code, new terminals open as **Developer PowerShell (x64)** ([.vscode/vs-dev-shell.ps1](.vscode/vs-dev-shell.ps1)), with `cl`, `cmake` and `ninja` on PATH. Outside VS Code, use a *Developer PowerShell / x64 Native Tools* prompt, or dot-source that script:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
.\build\x64-debug\bin\WalnutRenderer.exe
```

The presets are `x64-debug` and `x64-release` (RelWithDebInfo).

## Command-line options

| Option | Effect |
|---|---|
| `--debug-layer` / `--no-debug-layer` | Force the D3D12 debug layer on or off. It is on by default in Debug builds. |
| `--gpu-validation` | Turns on GPU-based validation, which also enables the debug layer. Slow. |
| `--exit-after-frames=N` | Quit after N frames. Useful for smoke tests. |

Logs go to the debugger output and to `WalnutRenderer.log` next to the exe. When the debug layer is active, its messages also go to the log.

## Controls

| Input | Action |
|---|---|
| WASD / arrow keys | Move the triangle |
| Mouse wheel | Scale |
| Right mouse drag | Rotate (raw mouse deltas) |
| Space | Toggle auto-rotate |
| Middle click | Reset the transform |
| F5 | Recompile shaders. Edit `build/<preset>/bin/shaders/triangle.hlsl` to try it live. |
| Esc | Quit |

## Layout

```
cmake/Dependencies.cmake  FetchContent: DirectX-Headers, Agility SDK, DXC, Dear ImGui (all pinned)
shaders/                  HLSL sources, copied next to the exe and compiled at runtime
src/core/                 Application loop, Win32 Window, Input, Timer, Log
src/gfx/                  Device, CommandQueue (+fence), DescriptorHeap, SwapChain, ShaderCompiler
src/renderer/             Renderer (frames in flight, begin/end frame), TrianglePass
src/ui/                   ImGuiLayer (Win32 + DX12 backends)
```

## How a frame works

1. `Input::BeginFrame()` resets the per-frame state. `Window::PumpMessages()` then passes Win32 messages to ImGui and to `Input`.
2. If the window was resized, the renderer flushes the GPU and calls `ResizeBuffers` on the swapchain.
3. ImGui starts a new frame. `Input` is told whether ImGui is using the keyboard or mouse, and ignores that device for game input while it is.
4. `Renderer::BeginFrame()` waits on this frame slot's fence and resets its command allocator. It then moves the back buffer from PRESENT to RENDER_TARGET, clears it, and sets the viewport and scissor.
5. The triangle and the ImGui overlay are recorded into the command list.
6. `Renderer::EndFrame()` moves the back buffer back to PRESENT and executes the command list. It then presents and signals the frame fence.

The **Agility SDK** is enabled through the `D3D12SDKVersion` and `D3D12SDKPath` exports in `main.cpp`. `D3D12Core.dll` is loaded from `bin/D3D12/`, and the log shows which copy was used.

## Pinned dependency versions

| Dependency | Version |
|---|---|
| D3D12 Agility SDK / DirectX-Headers | 1.620.1 |
| DXC | 1.9.2609.5 |
| Dear ImGui | v1.93.0-docking |

Versions are set at the top of `cmake/Dependencies.cmake`. Keep the Agility SDK and DirectX-Headers on the same version.
