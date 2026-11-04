#include "gfx/Pix.h"

#include "core/Log.h"
#include "core/StringUtils.h"

namespace walnut::pix
{
    bool LoadGpuCapturer()
    {
        if (IsGpuCapturerLoaded())
        {
            log::Info("PIX GPU capturer already present (launched from PIX)");
            return true;
        }

        // Searches %ProgramFiles%\Microsoft PIX\<newest version>\WinPixGpuCapturer.dll.
        if (HMODULE module = PIXLoadLatestWinPixGpuCapturerLibrary())
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            log::Info("Loaded PIX GPU capturer: {}", ToUtf8(path));
            return true;
        }

        log::Warn("Could not load WinPixGpuCapturer.dll; is PIX installed?");
        return false;
    }

    bool IsGpuCapturerLoaded()
    {
        return GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr;
    }

    bool CaptureNextFrames(const std::filesystem::path& file, uint32_t numFrames)
    {
        if (!IsGpuCapturerLoaded())
        {
            log::Warn("PIX GPU capture requested, but the GPU capturer is not loaded (run with --pix)");
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);

        const HRESULT hr = PIXGpuCaptureNextFrames(file.c_str(), numFrames);
        if (FAILED(hr))
        {
            log::Error("PIXGpuCaptureNextFrames failed (0x{:08X})", static_cast<uint32_t>(hr));
            return false;
        }
        log::Info("PIX GPU capture of {} frame(s) queued: {}", numFrames, ToUtf8(file.wstring()));
        return true;
    }
}
