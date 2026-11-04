#pragma once

#include <Windows.h>

#include <cstdint>

namespace walnut
{
    // High-resolution frame timer built on QueryPerformanceCounter.
    class Timer
    {
    public:
        Timer()
        {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            m_secondsPerTick = 1.0 / static_cast<double>(frequency.QuadPart);
            Reset();
        }

        void Reset()
        {
            m_start = Now();
            m_last = m_start;
        }

        // Returns seconds elapsed since the previous Tick().
        float Tick()
        {
            const int64_t now = Now();
            const double delta = static_cast<double>(now - m_last) * m_secondsPerTick;
            m_last = now;
            return static_cast<float>(delta);
        }

        double GetTotalSeconds() const
        {
            return static_cast<double>(m_last - m_start) * m_secondsPerTick;
        }

    private:
        static int64_t Now()
        {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            return counter.QuadPart;
        }

        double m_secondsPerTick = 0.0;
        int64_t m_start = 0;
        int64_t m_last = 0;
    };
}
