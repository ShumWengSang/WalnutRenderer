# WalnutRenderer

A small Direct3D 12 learning engine for Windows. It uses raw Win32 for the window and input, Dear ImGui for UI, and compiles HLSL at runtime with DXC.

## Requirements

- Windows 10/11 x64 with a D3D12 (feature level 12_0) GPU whose driver supports **Enhanced Barriers** (`D3D12_OPTIONS12::EnhancedBarriersSupported`). Enhanced Barriers have no automatic fallback, so the app refuses to start without them; a driver update usually fixes that.
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

### Assets
Models aren't in the repository. Download them into the git-ignored `assets/` folder with:

```powershell
.\tools\fetch-assets.ps1                               # Sponza (~50 MB)
.\tools\fetch-assets.ps1 -Models Sponza,DamagedHelmet  # any model folder from glTF-Sample-Assets
```

The script does a sparse, shallow fetch of [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) at a pinned commit, downloading only each model's `glTF/` folder (plus its README and LICENSE) into `assets/<Model>/`. CMake bakes the absolute path of `assets/` into the exe (`WALNUT_ASSET_DIR`), so every build folder finds the same models.

## Command-line options

| Option | Effect |
|---|---|
| `--debug-layer` / `--no-debug-layer` | Force the D3D12 debug layer on or off. It is on by default in Debug builds. |
| `--gpu-validation` | Turns on GPU-based validation, which also enables the debug layer. Slow. |
| `--exit-after-frames=N` | Quit after N frames. Useful for smoke tests. |
| `--exit-after-seconds=S` | Quit after S seconds. Used by `tools/pix-capture.ps1`. |
| `--pix` | Load PIX's GPU capturer (`WinPixGpuCapturer.dll`) at startup, so you can attach PIX or take captures from inside the app. |
| `--pix-capture-frame=N` | Take a GPU capture of frame N into `bin/captures/`. Implies `--pix`. |
| `--scene=PATH` | glTF scene to use. Relative paths start at `assets/`. The default is `Sponza/Sponza.gltf`. |

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
cmake/Dependencies.cmake  FetchContent: DirectX-Headers, Agility SDK, DXC, Dear ImGui, WinPixEventRuntime, cgltf (all pinned)
shaders/                  HLSL sources, copied next to the exe and compiled at runtime
src/core/                 Application loop, Win32 Window, Input, Timer, Log
src/gfx/                  Device, CommandQueue (+fence), DescriptorHeap, SwapChain, ShaderCompiler, Pix
src/renderer/             Renderer (frames in flight, begin/end frame), TrianglePass
src/ui/                   ImGuiLayer (Win32 + DX12 backends)
tools/                    fetch-assets.ps1 (download glTF models), pix-capture.ps1 (capture via pixtool), pix-mcp.cmd (PIX MCP server launcher)
```

## Profiling and debugging with PIX

Install [PIX on Windows](https://devblogs.microsoft.com/pix/download/) **2610.28 or newer**; that release adds the MCP servers. The MCP servers also need the .NET 10 runtime and the ASP.NET Core 10 runtime:
`winget install Microsoft.DotNet.Runtime.10 Microsoft.DotNet.AspNetCore.10`.

### Markers

The code uses WinPixEventRuntime (`src/gfx/Pix.h`). `USE_PIX` is defined in every configuration, so Release builds can be profiled too.

| Timeline | Events |
|---|---|
| CPU | `Frame` › `PumpMessages`, `Update + UI`, `Render` › `Wait for frame fence`, `ImGui::Render`, `Present` |
| GPU (command list) | `Frame` › `Clear back buffer`, `TrianglePass`, `ImGui` |
| Counter | `Frame time (ms)`, recorded with `PIXReportCounter` every frame |

Keep event names constant, with no frame numbers formatted into them. PIX groups executions by name, so a name that changes every frame creates a separate event each frame and breaks per-event statistics.

### Taking captures

```powershell
.\tools\pix-capture.ps1 -Kind Timing -DurationMs 3000   # Timing Capture -> captures\*.wpix
.\tools\pix-capture.ps1 -Kind Gpu                         # GPU Capture of 1 frame
```

You can also run the app with `--pix` and use the **PIX → Capture next frame** button, or attach from the PIX UI. When attached, PIX's own F11 hotkey also works. The `captures/` folder is git-ignored.

### Analyzing with AI agents (MCP)

[.vscode/mcp.json](.vscode/mcp.json) registers PIX's MCP servers through `tools/pix-mcp.cmd`, which always launches the newest installed PIX:

| Server | For | Example tools |
|---|---|---|
| `PixMcp` | Timing Captures (`.wpix`) | `tc_open_capture`, `tc_get_capture_manifest`, `tc_query_pix_event_definitions`, `tc_query_executions_in_event`, `tc_query_gpu_executions_by_event` |
| `DxDmpMcp` | DirectX dump files (`.dxdmp`) from GPU hangs or device removal | `open_dump`, `get_dred`, `list_page_faults`, `query_known_issues` |

PIX also ships agent skills, `pix-timing-capture-analysis` and `pix-diagnose`, which guide an agent through using these tools. Copy them into `~/.copilot/skills/` from `<PIX>\skills\` and `<PIX>\Tools\DxDmp\agent-kit\skills\`. GPU Captures don't have an MCP server yet; use `pixtool` (for example `open-capture X save-event-list Y`) or the PIX UI.

Example prompt: *"Survey captures\WalnutRenderer_timing_….wpix with PixMcp: frame time, Present and fence waits, and GPU pass costs."*

## How a frame works

1. `Input::BeginFrame()` resets the per-frame state. `Window::PumpMessages()` then passes Win32 messages to ImGui and to `Input`.
2. If the window was resized, the renderer flushes the GPU and calls `ResizeBuffers` on the swapchain.
3. ImGui starts a new frame. `Input` is told whether ImGui is using the keyboard or mouse, and ignores that device for game input while it is.
4. `Renderer::BeginFrame()` waits on this frame slot's fence and resets its command allocator. It then records an Enhanced Barrier that changes the back buffer's layout from PRESENT to RENDER_TARGET (`SyncBefore = NONE`, `SyncAfter = RENDER_TARGET`), clears it, and sets the viewport and scissor.
5. The triangle and the ImGui overlay are recorded into the command list.
6. `Renderer::EndFrame()` records an Enhanced Barrier from RENDER_TARGET back to PRESENT (`SyncBefore = RENDER_TARGET`, `SyncAfter = NONE`) and executes the command list. It then presents and signals the frame fence.

### Barriers

The renderer uses **Enhanced Barriers** only: `ID3D12GraphicsCommandList7::Barrier`, plus `CreateCommittedResource3` with an `InitialLayout`. A barrier states three things separately:
- **Sync:** which GPU work must finish first, and which work must wait.
- **Access:** which caches must be flushed or invalidated.
- **Layout:** textures only; how the data is arranged in memory.

ImGui's DX12 backend still uses legacy `ResourceBarrier` on its own textures. That's allowed, because the two models never touch the same resource.

The **Agility SDK** is enabled through the `D3D12SDKVersion` and `D3D12SDKPath` exports in `main.cpp`. `D3D12Core.dll` is loaded from `bin/D3D12/`, and the log shows which copy was used.

## Pinned dependency versions

| Dependency | Version |
|---|---|
| D3D12 Agility SDK / DirectX-Headers | 1.620.1 |
| DXC | 1.9.2609.5 |
| Dear ImGui | v1.93.0-docking |
| WinPixEventRuntime | 1.0.240308001 |
| cgltf | v1.15 |

Versions are set at the top of `cmake/Dependencies.cmake`. Keep the Agility SDK and DirectX-Headers on the same version.
