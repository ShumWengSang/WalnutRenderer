#pragma once

#include "gfx/D3D12Common.h"

#include <format>
#include <source_location>
#include <stdexcept>
#include <string>

namespace walnut
{
    class GpuError : public std::runtime_error
    {
    public:
        GpuError(HRESULT hr, const std::string& message)
            : std::runtime_error(message), m_hr(hr) {}

        HRESULT GetResult() const { return m_hr; }

    private:
        HRESULT m_hr;
    };

    inline void ThrowIfFailed(HRESULT hr, const char* expression,
                              std::source_location where = std::source_location::current())
    {
        if (FAILED(hr))
        {
            throw GpuError(hr, std::format("{} failed with HRESULT 0x{:08X} ({}:{})",
                                           expression, static_cast<uint32_t>(hr), where.file_name(), where.line()));
        }
    }

    // Debug names show up in debug-layer messages, PIX and RenderDoc.
    inline void SetDebugName(ID3D12Object* object, const wchar_t* name)
    {
        if (object && name)
            object->SetName(name);
    }
}

#define WR_CHECK(expr) ::walnut::ThrowIfFailed((expr), #expr)
