#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace walnut
{
    class Camera;
    class SettingsRegistry;

    // A timeline for --record: camera keyframes and setting changes over time, played back at a fixed frame rate so
    // every frame is rendered at an exact time (smooth, and the same every run). A text file, one command per line
    // ('#' starts a comment; times in seconds, angles in degrees):
    //
    //   fps 60
    //   duration 30
    //   camera T X Y Z YAW PITCH        a camera keyframe; the path is a Catmull-Rom spline through them. Two
    //                                   keyframes at the same time make a cut (the camera jumps).
    //   set T NAME VALUE                at time T, --set NAME=VALUE
    //   ramp T0 T1 NAME FROM TO         from T0 to T1, move a number setting smoothly from FROM to TO (integers stay
    //                                   integers when FROM and TO are written without a decimal point)
    class RecordingScript
    {
    public:
        // Returns nothing and sets 'error' (with the line number) if the file can't be read or parsed.
        static std::optional<RecordingScript> Load(const std::filesystem::path& path, std::string& error);

        double GetFps() const { return m_fps; }
        uint32_t GetFrameCount() const;
        double GetFrameTime(uint32_t frame) const { return static_cast<double>(frame) / m_fps; }

        // Moves the camera to time 'time' and applies the settings due by then. Call with increasing times.
        void Apply(double time, Camera& camera, SettingsRegistry& settings);

    private:
        struct Key
        {
            double time;
            float x, y, z, yaw, pitch;
        };
        struct Change
        {
            double start, end; // equal for 'set'
            std::string name;
            std::string value;          // 'set'
            double from = 0.0, to = 0.0; // 'ramp'
            bool ramp = false;
            bool integer = false;
            bool done = false;
        };

        double m_fps = 60.0;
        double m_duration = 0.0;
        std::vector<Key> m_keys;
        std::vector<Change> m_changes;
    };
}
