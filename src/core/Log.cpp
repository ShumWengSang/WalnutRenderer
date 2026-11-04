#include "core/Log.h"

#include <Windows.h>

#include <fstream>
#include <mutex>

namespace walnut::log
{
    namespace
    {
        std::mutex g_mutex;
        std::ofstream g_file;

        const char* LevelTag(Level level)
        {
            switch (level)
            {
            case Level::Info: return "[info] ";
            case Level::Warning: return "[warn] ";
            case Level::Error: return "[error]";
            }
            return "[?]    ";
        }
    }

    void Init(const std::filesystem::path& logFile)
    {
        std::scoped_lock lock(g_mutex);
        g_file.open(logFile, std::ios::out | std::ios::trunc);
    }

    void Shutdown()
    {
        std::scoped_lock lock(g_mutex);
        g_file.close();
    }

    void Write(Level level, std::string_view message)
    {
        std::string line = std::format("{} {}\n", LevelTag(level), message);

        std::scoped_lock lock(g_mutex);
        OutputDebugStringA(line.c_str());
        if (g_file.is_open())
        {
            g_file << line;
            g_file.flush();
        }
    }
}
