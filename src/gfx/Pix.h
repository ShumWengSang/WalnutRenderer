#pragma once

// PIX integration: markers, counters and capture hooks.
//
// Use PIXScopedEvent / PIXBeginEvent / PIXEndEvent / PIXSetMarker from pix3.h directly:
//   PIXScopedEvent(pix::kColorCpu, L"Update");                      // CPU timeline
//   PIXScopedEvent(commandList, pix::kColorGpu, L"Triangle");       // GPU timeline
//
// CPU events show up in Timing Captures. GPU events (on command lists and queues) show up
// in both Timing Captures and GPU Captures.
//
// Keep event names constant (don't format frame numbers into them): PIX groups executions
// by name, so a per-frame name creates a new event definition every frame and defeats
// per-event statistics.
#include "gfx/D3D12Common.h"

#include <WinPixEventRuntime/pix3.h>

#include <cstdint>
#include <filesystem>

namespace walnut::pix
{
    inline const UINT8 kColorFrame = PIX_COLOR_INDEX(1);
    inline const UINT8 kColorCpu = PIX_COLOR_INDEX(2);
    inline const UINT8 kColorWait = PIX_COLOR_INDEX(3);
    inline const UINT8 kColorGpu = PIX_COLOR_INDEX(4);

    // Loads WinPixGpuCapturer.dll from the newest PIX install so the app can take GPU captures
    // (PIX UI "Attach", or CaptureNextFrames). Must run before the D3D12 device is created.
    // Not needed when the app is launched from PIX/pixtool, which inject the capturer themselves.
    bool LoadGpuCapturer();

    // True when WinPixGpuCapturer.dll is in the process (loaded by us or injected by PIX).
    bool IsGpuCapturerLoaded();

    // Writes a GPU capture of the next numFrames presented frames to 'file' (.wpix).
    bool CaptureNextFrames(const std::filesystem::path& file, uint32_t numFrames = 1);
}
