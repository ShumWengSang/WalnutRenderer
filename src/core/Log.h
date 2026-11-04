#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace walnut::log
{
    enum class Level
    {
        Info,
        Warning,
        Error,
    };

    // Writes to the debugger output (OutputDebugString) and, once Init() is called, to a log file.
    void Init(const std::filesystem::path& logFile);
    void Shutdown();
    void Write(Level level, std::string_view message);

    template <typename... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void Warn(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(Level::Warning, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void Error(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
    }
}
