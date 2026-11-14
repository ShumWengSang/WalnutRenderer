# WalnutRenderer

A Direct3D 12 renderer written from scratch to learn how modern real-time rendering works: raw Win32, Dear ImGui, HLSL compiled at runtime with DXC, and no engine underneath. It is written in C++20 with CMake, for Windows.

It renders Sponza (a classic test scene: a large palace courtyard) with PBR materials, a sun with shadow-mapped or ray-traced shadows, and up to 1024 clustered point lights with cube-map or ray-traced shadows, in forward or deferred. Each technique has an alternative you can switch to live, and every comparison is measured on the GPU.

[![WalnutRenderer showcase video (30 s): click to play](docs/media/showcase-poster.jpg)](docs/media/showcase.mp4)

<sub>▶ 30 s showcase, 1080p60 ([showcase.mp4](docs/media/showcase.mp4), 71 MB; [720p](docs/media/showcase-720p.mp4), 9 MB). Every frame is rendered offline at an exact time with `--record`, so it's smooth and reproducible (`tools/make-media.ps1`).</sub>

| | |
|---|---|
| ![The sun sweeps across the sky; shadows follow](docs/media/sun.gif) | ![512 animated point lights at night with cube shadow maps](docs/media/night.gif) |
| **Sun and shadow map.** The sun moves; a 2048² shadow map with 3×3 PCF is re-rendered every frame. | **512 point lights**, clustered, animated; the nearest 32 cast shadows from cube maps. |
| ![Lights per cluster heat map as the lamp count changes](docs/media/clusters.gif) | ![Point light shadows: off, cube maps, ray traced](docs/media/pointshadows.gif) |
| **Clustered lighting.** Lights per cluster (16×9×24 view-space grid) as the lamp count goes 32 → 1024. | **Point light shadows**: none, cube shadow maps, and inline ray tracing for every lamp. |
| ![Sun shadow: shadow map, ray traced, and their difference](docs/media/sunshadows.gif) | |
| **Sun shadows**: shadow map vs one ray per pixel, and where they disagree. | |

## Highlights

- **Techniques side by side.** Shadow map vs ray-traced sun shadows, matte (Lambert) vs physically based (PBR) materials, forward vs deferred shading, brute-force vs clustered lights, and cube-map vs ray-traced lamp shadows. You switch between them in the UI, and difference views show where two techniques disagree.
- **Many lights, cheaply.** Clustered lighting: the screen is split into small 3D boxes, and each pixel only checks the lights in its box. At 1024 lights it's 8.5× faster than brute force (forward).
- **Modern Direct3D 12 only.** Bindless resources (shaders find every buffer and texture by index in one big table), Enhanced Barriers (D3D12's newer, explicit way of telling the GPU a resource changes role), and inline ray tracing (any shader can cast a ray with `RayQuery`).
- **Measured.** GPU timings come from PIX Timing Captures, and every configuration can be reproduced from the command line.
- **Checked pixel by pixel.** A 51-shot screenshot matrix is compared pixel by pixel before and after changes; a 17-commit refactor changed no pixel.
- **Made for experimenting.** Live shader reload (F5), `--set` for any setting, 22 debug views, and `--record` for smooth, reproducible videos.

<details>
<summary><b>Feature list</b> — every area at a glance, with the technical terms</summary>

| Area | What's there |
|---|---|
| **API** | Direct3D 12 with the Agility SDK (1.620.1): Shader Model 6.6 **bindless** (`ResourceDescriptorHeap[]`), **Enhanced Barriers** only, inline ray tracing (DXR 1.1 `RayQuery`), one shared root signature |
| **Geometry** | glTF 2.0 loading (cgltf), merged vertex/index buffers, **vertex pulling**, one draw per instance, per-instance world bounds |
| **Materials** | glTF metallic-roughness PBR: base color, metallic-roughness, normal, occlusion and emissive maps, alpha test, double-sided; CPU mip chains (sRGB-correct, renormalized normals); mesh or derived tangents |
| **Lighting** | Cook-Torrance GGX specular + Lambert diffuse, sky/ground hemisphere ambient with an analytic environment BRDF, HDR FP16 target, exposure + ACES / Reinhard tonemapping |
| **Sun shadows** | 2048² shadow map (slope-scaled + normal-offset bias, alpha-tested casters, PCF), or one ray-traced shadow ray per pixel (BLAS per primitive + TLAS); a difference view compares them |
| **Rendering paths** | Depth pre-pass (depth-EQUAL shading), forward or **deferred** (4-target G-buffer, octahedral normals), same shading code in both |
| **Many lights** | Up to 1024 point lights, **clustered** (16×9×24 view-space clusters built by a compute shader with groupshared batching), or brute force to compare |
| **Point light shadows** | **Cube shadow maps** for the nearest N lamps (one 2D array, sampled as a `TextureCubeArray`, sphere-culled draws), or **ray traced** for every lamp |
| **Debug views** | 22 views: material maps, normals, lighting terms, shadow visibility, lights per cluster, and difference views comparing shadow techniques |
| **Tooling** | Live shader reload (F5), PIX markers and capture scripts (with PIX's MCP servers), `--set` for any setting, `--screenshot`, a 51-shot screenshot matrix compared pixel by pixel, `--record` for videos, GPU-based validation runs |

</details>

<details>
<summary><b>Feature details</b> — how each feature is implemented</summary>

It loads a glTF scene (Sponza by default) and renders it with:

- **Bindless resources**: one shader-visible descriptor heap and one shared root signature; shaders find buffers and textures through `ResourceDescriptorHeap[index]` (Shader Model 6.6).
- **Vertex pulling**: no input layouts; vertex shaders read a merged `StructuredBuffer` of vertices. One `DrawIndexedInstanced` per instance, with the instance index as the only per-draw root constant.
- **glTF materials** (metallic-roughness): base color, metallic/roughness, normal, occlusion and emissive maps (including `KHR_materials_emissive_strength`), with CPU-generated mip chains (linear-light averaging for sRGB colors, re-normalized normals for normal maps). Normal mapping uses the mesh's tangents, or derives them per pixel from screen-space derivatives. Alpha test for `MASK` materials, and double-sided materials are drawn without culling.
- **Lighting**: PBR shading (`shaders/common/Brdf.hlsli`): Lambert diffuse plus Cook-Torrance specular (GGX distribution, height-correlated Smith visibility, Schlick Fresnel), for a directional sun and for a sky/ground hemisphere ambient (an analytic split-sum environment BRDF), with occlusion and emission. Sun shadows come from either:
  - a **shadow map** (2048², slope-scaled bias + normal offset, alpha-tested casters, PCF soft edges), or
  - **ray tracing** (DXR 1.1 inline `RayQuery` against a BLAS-per-primitive / TLAS acceleration structure, alpha-tested leaves via candidate hits), when the GPU supports raytracing tier 1.1. A difference view compares the two.
- **HDR**: the scene renders into an FP16 target in linear light; a fullscreen pass applies exposure and a tonemap curve (ACES fitted, Reinhard, clamp) and writes the back buffer through an `_SRGB` view.
- **Depth pre-pass and deferred shading**: a depth-only pass lets the scene pass shade each pixel once (depth EQUAL). **View → Forward / Deferred** switches between shading in each draw and writing a G-buffer (base color, two octahedral normals, roughness/metallic/occlusion, emissive) that a fullscreen pass lights with the same shader code.
- **Many point lights, clustered**: up to 1024 lamps (inverse-square falloff with a smooth range window, same BRDF as the sun), drawn as instanced glowing markers. A compute shader sorts them every frame into a 16×9×24 grid of view-space clusters, and each pixel shades only its cluster's list (**Lighting → Point lights → Brute force / Clustered**), in forward and deferred alike.
- **Point light shadows**: **Lighting → Point lights → Off / Cube maps / Ray traced**. Cube maps: the lamps nearest the camera (up to 32) each render six 90° depth faces into one texture array, sampled as a `TextureCubeArray` by direction. Ray traced: one inline `RayQuery` to every lamp that reaches the pixel, so every lamp casts shadows.
- **Reversed-Z** depth (D32_FLOAT, infinite far plane), **Enhanced Barriers** only, a fly camera, and PIX markers throughout.

</details>

## Performance

All numbers are GPU time per frame; lower is better.

GPU frame time (median), PIX Timing Captures of the current code. RTX 3070 Ti, 1280×720, Release, vsync off, Sponza start view, sun on, frozen animation (`--time=2`). Each row is one `--set` configuration, so any of them can be reproduced with `tools\pix-capture.ps1 -AppArgs "..."`.

**One sun, no point lights**

| Configuration | GPU frame | Biggest pass |
|---|---|---|
| Forward, no depth pre-pass | 0.39 ms | scene 0.31 ms |
| Forward + depth pre-pass, shadow map | **0.30 ms** | scene 0.17 ms, pre-pass 0.04, shadow map 0.06 |
| Forward + pre-pass, ray-traced sun shadows | 0.56 ms | scene (with rays) 0.48 ms |
| Deferred, shadow map | 0.33 ms | G-buffer 0.12 ms, lighting 0.08 ms |

**1024 point lights** (no point light shadows)

| | Brute force | Clustered | Speed-up |
|---|---|---|---|
| Forward | 9.64 ms | **1.14 ms** | 8.5× |
| Deferred | 6.04 ms | **0.88 ms** | 6.8× |

The cluster build (compute) takes 0.12 ms at 1024 lamps; each pixel then visits about 37 lamps instead of 1024.

**1024 point lights with shadows** (forward, clustered)

| Point light shadows | GPU frame | Shadow cost |
|---|---|---|
| Off | 1.14 ms | – |
| Cube maps, nearest 16 | 1.64 ms | cube rendering 0.45 ms |
| Cube maps, nearest 32 | 2.11 ms | cube rendering 0.88 ms (48 MB of cubes) |
| Ray traced, every lamp | 7.49 ms | rays in the scene pass, ~6.3 ms |

The default start-up configuration (128 lamps, 16 cube shadows, forward) runs at 0.98 ms on the GPU.

A 17-commit refactor of the code changed no pixel, and timings stayed within 2%: the same configurations measured before it (1024 lamps forward / deferred, 16 / 32 cubes, rays) were 1.13 / 0.88 / 1.63 / 2.12 / 7.34 ms.

## Quick start

### Requirements

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

### Build and run

**In VS Code**

1. Install the recommended extensions: C/C++, CMake Tools, and HLSL Tools. HLSL Tools reads its include path and defines from `shaders/shadertoolsconfig.json`, and sees editor-only stand-ins for the newer HLSL features it doesn't know (`shaders/common/IntelliSense.hlsli`).
2. Open the folder and pick the **x64 Debug** configure preset. CMake Tools sets up the MSVC environment for you.
3. Press **F5**. This builds and launches under the debugger (`.vscode/launch.json`).

**From the command line**

In VS Code, new terminals open as **Developer PowerShell (x64)** ([.vscode/vs-dev-shell.ps1](.vscode/vs-dev-shell.ps1)), with `cl`, `cmake` and `ninja` on PATH. Outside VS Code, use a *Developer PowerShell / x64 Native Tools* prompt, or dot-source that script:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
.\build\x64-debug\bin\WalnutRenderer.exe
```

The presets are `x64-debug` and `x64-release` (RelWithDebInfo).

### Get the models

Models aren't in the repository. Download them into the git-ignored `assets/` folder with:

```powershell
.\tools\fetch-assets.ps1                               # Sponza (~50 MB)
.\tools\fetch-assets.ps1 -Models Sponza,DamagedHelmet  # any model folder from glTF-Sample-Assets
```

<details>
<summary><b>How the asset download works</b> — sparse fetch at a pinned commit, path baked into the exe</summary>

The script does a sparse, shallow fetch of [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) at a pinned commit, downloading only each model's `glTF/` folder (plus its README and LICENSE) into `assets/<Model>/`. CMake bakes the absolute path of `assets/` into the exe (`WALNUT_ASSET_DIR`), so every build folder finds the same models.

</details>

### Controls

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

### Command-line options

The most useful ones: `--set=NAME=VALUE` changes any setting at startup, `--list-settings` lists them all, `--screenshot=FILE.png` saves a frame, and `--help` logs every option.

<details>
<summary><b>All command-line options</b> — debug layer, PIX, scene, camera, settings, screenshots, recording</summary>

| Option | Effect |
|---|---|
| `--debug-layer` / `--no-debug-layer` | Force the D3D12 debug layer on or off. It is on by default in Debug builds. |
| `--gpu-validation` | Turns on GPU-based validation, which also enables the debug layer. Slow. |
| `--exit-after-frames=N` | Quit after N frames. Useful for smoke tests. |
| `--exit-after-seconds=S` | Quit after S seconds. Used by `tools/pix-capture.ps1`. |
| `--pix` | Load PIX's GPU capturer (`WinPixGpuCapturer.dll`) at startup, so you can attach PIX or take captures from inside the app. |
| `--pix-capture-frame=N` | Take a GPU capture of frame N into `bin/captures/`. Implies `--pix`. |
| `--scene=PATH` | glTF scene to use. Relative paths start at `assets/`. The default is `Sponza/Sponza.gltf`. |
| `--time=S` | Animations (the point lights' bobbing) use time S instead of the clock, so frames are reproducible. |
| `--camera=X,Y,Z,YAW,PITCH` | Start camera: position in meters, yaw and pitch in degrees (the **Camera** section shows the current ones). |
| `--set=NAME=VALUE` | Change a setting at startup, e.g. `--set=view.path=deferred --set=points.shadows=ray`. Repeatable. |
| `--list-settings` | Log every `--set` name with its current value, then quit. |
| `--screenshot=FILE.png` | Save the tonemapped frame (without the UI) at frame 5 (`--screenshot-frame=N`), then quit. |
| `--size=WxH` | Window client size (default 1280x720). |
| `--record=SCRIPT` | Play a timeline of camera keyframes and setting changes (format in `src/core/RecordingScript.h`, examples in `tools/recordings/`), rendering every frame at an exact time and saving it as a PNG into `--record-dir=DIR` (default `recording/` next to the exe), then quit. |
| `--help` | Log all options, then quit. |

</details>

Logs go to the debugger output and to `WalnutRenderer.log` next to the exe. When the debug layer is active, its messages also go to the log.
