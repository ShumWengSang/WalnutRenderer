# WalnutRenderer

A small Direct3D 12 learning engine for Windows. It uses raw Win32 for the window and input, Dear ImGui for UI, and compiles HLSL at runtime with DXC.

It loads a glTF scene (Sponza by default) and renders it with:
- **Bindless resources**: one shader-visible descriptor heap and one shared root signature; shaders find buffers and textures through `ResourceDescriptorHeap[index]` (Shader Model 6.6).
- **Vertex pulling**: no input layouts; vertex shaders read a merged `StructuredBuffer` of vertices. One `DrawIndexedInstanced` per instance, with the instance index as the only per-draw root constant.
- **glTF materials** (metallic-roughness): base color, metallic/roughness, normal, occlusion and emissive maps (including `KHR_materials_emissive_strength`), with CPU-generated mip chains (linear-light averaging for sRGB colors, re-normalized normals for normal maps). Normal mapping uses the mesh's tangents, or derives them per pixel from screen-space derivatives. Alpha test for `MASK` materials, and double-sided materials are drawn without culling.
- **Lighting**: PBR shading (`shaders/common/Brdf.hlsli`): Lambert diffuse plus Cook-Torrance specular (GGX distribution, height-correlated Smith visibility, Schlick Fresnel), for a directional sun and for a sky/ground hemisphere ambient (an analytic split-sum environment BRDF), with occlusion and emission. Sun shadows come from either:
  - a **shadow map** (2048², slope-scaled bias + normal offset, alpha-tested casters, PCF soft edges), or
  - **ray tracing** (DXR 1.1 inline `RayQuery` against a BLAS-per-primitive / TLAS acceleration structure, alpha-tested leaves via candidate hits), when the GPU supports raytracing tier 1.1. A difference view compares the two.
- **HDR**: the scene renders into an FP16 target in linear light; a fullscreen pass applies exposure and a tonemap curve (ACES fitted, Reinhard, clamp) and writes the back buffer through an `_SRGB` view.
- **Depth pre-pass and deferred shading**: a depth-only pass lets the scene pass shade each pixel once (depth EQUAL). **View → Forward / Deferred** switches between shading in each draw and writing a G-buffer (base color, two octahedral normals, roughness/metallic/occlusion, emissive) that a fullscreen pass lights with the same shader code.
- **Reversed-Z** depth (D32_FLOAT, infinite far plane), **Enhanced Barriers** only, a fly camera, and PIX markers throughout.

## Requirements

- Windows 10/11 x64 with a D3D12 (feature level 12_0) GPU whose driver supports **Enhanced Barriers** (`D3D12_OPTIONS12::EnhancedBarriersSupported`), **Shader Model 6.6** and **Resource Binding Tier 3**. None of these has an automatic fallback, so the app skips adapters without them (the log says what's missing); a driver update usually fixes that.
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
| Right mouse + move | Look around (raw mouse deltas) |
| W/S, A/D | Move forward/back, strafe left/right |
| Q/E | Move down/up |
| Shift | Move faster |
| Mouse wheel | Change move speed |
| F1 | Hide or show the UI |
| F5 | Recompile shaders. Edit `shaders/scene.hlsl` in the repo and press F5 to see the change live. |
| Esc | Quit |

The camera starts inside large scenes like Sponza, or framing small ones from outside. **Camera → Reset camera** goes back there.

## Layout

```
cmake/Dependencies.cmake  FetchContent: DirectX-Headers, Agility SDK, DXC, Dear ImGui, WinPixEventRuntime, cgltf, stb_image (all pinned)
shaders/                  HLSL sources, compiled at runtime straight from here (a copy next to the exe is the fallback):
                          scene.hlsl (draws), deferred.hlsl (deferred lighting), tonemap.hlsl; common/ holds the shared
                          includes (bindless, frame constants, scene data, BRDF, lighting, G-buffer layout, debug views)
src/core/                 Application loop, Win32 Window, Input, Timer, Log
src/gfx/                  Device, CommandQueue (+fence), DescriptorHeap, SwapChain, RootSignature (shared bindless),
                          Buffer, Texture, UploadContext (staging ring), FrameAllocator (per-frame constants),
                          MipChain, ShaderCompiler, Pix
src/renderer/             Renderer (frames in flight, frame constants, pass setup), ScenePass, ShadowMap,
                          AccelerationStructures (BLAS/TLAS), GBuffer, DeferredPass, TonemapPass
src/scene/                Camera, fly controller, lighting, glTF loading (cgltf) into a flat Scene, image decoding (stb_image)
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
| GPU (command list) | `Frame` › `Clear scene`, `ShadowMap` › `Shadow casters`, `ScenePass`, `Tonemap`, `ImGui` |
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
4. `Renderer::BeginFrame()` waits on this frame slot's fence, resets its command allocator and per-frame upload memory, and prepares the HDR scene color target and the depth buffer: Enhanced Barriers from UNDEFINED to RENDER_TARGET / DEPTH_STENCIL_WRITE (with DISCARD, so it doesn't matter which layout the last frame left them in), then clears of both. It binds the bindless state (descriptor heap, then the shared root signature).
5. `Renderer::SetFrameConstants()` binds the frame's camera and light data at `b2` (see `src/renderer/FrameConstants.h`); every pass can read it.
6. The shadow pass renders depth from the sun into a 2048² shadow map (`ShadowMap`: UNDEFINED → DEPTH_STENCIL_WRITE with DISCARD, clear, depth-only draws, then DEPTH_STENCIL_WRITE → DIRECT_QUEUE_SHADER_RESOURCE so pixel shaders can read it). The sun's camera is orthographic, fitted to the scene's bounding sphere.
   - At load (not per frame), when ray tracing is supported, `AccelerationStructures` builds one BLAS per primitive from the scene's vertex/index buffers and a TLAS over the instances.
7. The scene from the camera. `ScenePass` records one draw per instance (opaque first, then alpha-tested; PSOs switch only between those groups).
   - **Depth pre-pass** (`Renderer::BeginDepthPrepass()`, depth only): every draw writes its depth with no pixel shader (alpha-tested ones run a small shader that only discards). Shared, `precise` vertex math keeps the depths bit-identical to the next pass's.
   - **Forward** (`Renderer::BeginScenePass()`): the draws again, depth test EQUAL and no depth writes, so the pixel shader runs once per visible pixel. It samples the material (`SampleMaterial`) and lights it (`ShadeSurface` in `shaders/common/Lighting.hlsli`).
   - **Deferred** (`Renderer::BeginGBufferPass()`): the same EQUAL draws write the material into four G-buffer render targets instead. `Renderer::BeginDeferredPass()` then makes the G-buffer and depth buffer readable (RENDER_TARGET / DEPTH_STENCIL_WRITE → DIRECT_QUEUE_SHADER_RESOURCE), and `DeferredPass` draws one fullscreen triangle that rebuilds each pixel's world position from depth and calls the same `ShadeSurface`.
   - Either way, pixels are shaded with the glTF metallic-roughness BRDF: sun (diffuse + GGX specular) plus a sky/ground ambient (diffuse + reflection, darkened by the occlusion map) plus emission. The sun's visibility comes from the shadow map (slope-scaled bias, normal offset, PCF) or, with **Lighting → Ray traced**, from one `RayQuery` shadow ray per pixel against the TLAS. In ray-traced mode the shadow map pass is cleared but draws nothing.
8. `Renderer::BeginBackBufferPass()` records one barrier call with two transitions: scene color RENDER_TARGET → SHADER_RESOURCE (`SyncBefore = RENDER_TARGET`, `SyncAfter = PIXEL_SHADING`) and back buffer PRESENT → RENDER_TARGET. `TonemapPass` draws a fullscreen triangle that reads scene color and writes the back buffer through its `_SRGB` view.
9. `Renderer::BeginUiPass()` switches to the back buffer's plain `UNORM` view (ImGui's colors are already sRGB), and ImGui is recorded.
10. `Renderer::EndFrame()` records an Enhanced Barrier from RENDER_TARGET back to PRESENT (`SyncBefore = RENDER_TARGET`, `SyncAfter = NONE`) and executes the command list. It then presents and signals the frame fence.

### Sun shadows: shadow map vs ray traced

Switch with **Lighting → Shadow map / Ray traced**; **View → Show → Shadow difference** colors where they disagree (red: only the shadow map shadows; blue: only the ray does). Measured in step 2 (Lambert shading, no depth pre-pass) with PIX Timing Captures, Release, 1280×720, vsync off, RTX 3070 Ti, Sponza start view:

| | Shadow pass | Scene pass | GPU frame |
|---|---|---|---|
| Shadow map, hard (1 tap) | 0.06 ms | 0.09 ms | 0.18 ms |
| Shadow map, PCF 3×3 (default) | 0.06 ms | 0.14 ms | 0.22 ms |
| Ray traced, alpha-tested leaves | 0.00 ms | 0.64 ms | 0.66 ms |
| Ray traced, leaves forced opaque | 0.00 ms | 0.57 ms | 0.60 ms |

Ray-traced shadows are exact (no resolution, bias or acne) but cost about 0.5 ms here: one ray for every pixel the scene pass shades, including pixels later covered by closer geometry (overdraw). The shadow map's cost is mostly fixed (103 depth-only draws) and its quality depends on texel size.

### Materials: Lambert vs PBR

Everything the shader does with a material can be switched or isolated:
- **Lighting → Lambert / PBR (GGX)**: matte-only shading (base color / π, as in steps 1–2) or the full BRDF.
- **View → Normal mapping**: off, mesh tangents, or tangents derived from screen-space derivatives. [NormalTangentMirrorTest](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/NormalTangentMirrorTest) renders the same with both.
- **View → Roughness / Metallic** overrides replace every material's value, to see what each one does.
- **View → Show**: the material maps (roughness, metallic, normal map, occlusion, emissive) and the lighting terms (sun specular, ambient).

Measured like the shadows above (shadow map, PCF 3×3, still without the depth pre-pass): the scene pass went from 0.14 ms (step 2, Lambert with base color only) to 0.27 ms with PBR, normal mapping and all five maps; the GPU frame from 0.22 ms to 0.35 ms. The Lambert switch doesn't save time: it picks the result of one shader that has already sampled every map.

### Depth pre-pass, forward vs deferred

**View → Depth pre-pass** (forward only; deferred always uses it) and **View → Forward / Deferred**. The image is the same every way: with and without the pre-pass it's bit-identical, and deferred differs from forward by about 0.2 of 255 on average (the G-buffer's 8/16-bit storage; with ray-traced shadows, a pixel or two where a grazing ray lands the other way). **Renderer** shows the G-buffer's memory: 18.8 MB at 1280×720 (20 bytes per pixel, plus the depth buffer).

GPU frame times, PIX Timing Captures (Release, 1280×720, vsync off, RTX 3070 Ti, p50):

| | Start view, shadow map | Start view, ray traced | Upper gallery, shadow map | Upper gallery, ray traced |
|---|---|---|---|---|
| Forward, no pre-pass | 0.352 ms | 0.713 ms | – | – |
| Forward + pre-pass | 0.265 ms | 0.498 ms | 0.248 ms | 0.332 ms |
| Deferred | 0.293 ms | 0.449 ms | 0.269 ms | 0.354 ms |

- **The pre-pass** costs about 0.04 ms and halves the scene pass's pixel shader invocations at the start view (PIX counters: 1,829,025 → 916,264 for 921,600 pixels): early-Z rejects hidden fragments before the shader runs. Ray tracing gains most, since every skipped fragment is also a skipped ray.
- **Deferred** pays for writing and reading the G-buffer (the G-buffer pass costs about 85% of forward shading: sampling the material dominates, and the shadow-map lighting pass is only 0.05 ms). It wins only where lighting is expensive and the forward pass wastes it: at the start view with ray-traced shadows, where the fullscreen pass traces each ray once in fully packed 2×2 quads instead of on small triangles' partly empty ones. With one sun, forward + pre-pass is the better default; deferred pays off with many lights (light volumes or tiles) and screen-space effects that need the G-buffer anyway.

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
| stb_image | commit d14d46f |

Versions are set at the top of `cmake/Dependencies.cmake`. Keep the Agility SDK and DirectX-Headers on the same version.
