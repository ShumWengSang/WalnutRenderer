#include "core/RecordingScript.h"

#include "core/SettingsRegistry.h"
#include "scene/Camera.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace walnut
{
    namespace
    {
        // Uniform Catmull-Rom: passes through p1 at t = 0 and p2 at t = 1, with tangents from the neighbors.
        float CatmullRom(float p0, float p1, float p2, float p3, float t)
        {
            const float t2 = t * t, t3 = t2 * t;
            return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                           (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
        }

        double SmoothStep(double t)
        {
            t = std::clamp(t, 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
    }

    std::optional<RecordingScript> RecordingScript::Load(const std::filesystem::path& path, std::string& error)
    {
        std::ifstream file(path);
        if (!file)
        {
            error = "can't open the file";
            return std::nullopt;
        }
        RecordingScript script;
        std::string line;
        for (int lineNumber = 1; std::getline(file, line); ++lineNumber)
        {
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.resize(hash);
            std::istringstream in(line);
            std::string command;
            if (!(in >> command))
                continue;
            bool ok = false;
            if (command == "fps")
                ok = static_cast<bool>(in >> script.m_fps) && script.m_fps > 0.0;
            else if (command == "duration")
                ok = static_cast<bool>(in >> script.m_duration) && script.m_duration > 0.0;
            else if (command == "camera")
            {
                Key key{};
                ok = static_cast<bool>(in >> key.time >> key.x >> key.y >> key.z >> key.yaw >> key.pitch);
                if (ok && !script.m_keys.empty() && key.time < script.m_keys.back().time)
                {
                    error = std::format("line {}: camera keyframes must be in time order", lineNumber);
                    return std::nullopt;
                }
                if (ok)
                    script.m_keys.push_back(key);
            }
            else if (command == "set")
            {
                Change change;
                ok = static_cast<bool>(in >> change.start >> change.name >> change.value);
                change.end = change.start;
                if (ok)
                    script.m_changes.push_back(std::move(change));
            }
            else if (command == "ramp")
            {
                Change change;
                std::string from, to;
                ok = static_cast<bool>(in >> change.start >> change.end >> change.name >> from >> to) && change.end > change.start;
                if (ok)
                {
                    change.ramp = true;
                    change.integer = from.find('.') == std::string::npos && to.find('.') == std::string::npos;
                    change.from = std::stod(from);
                    change.to = std::stod(to);
                    script.m_changes.push_back(std::move(change));
                }
            }
            else
            {
                error = std::format("line {}: unknown command '{}'", lineNumber, command);
                return std::nullopt;
            }
            if (!ok)
            {
                error = std::format("line {}: bad '{}' line", lineNumber, command);
                return std::nullopt;
            }
        }
        if (script.m_duration <= 0.0)
        {
            error = "no 'duration'";
            return std::nullopt;
        }
        std::ranges::stable_sort(script.m_changes, {}, &Change::start);
        return script;
    }

    uint32_t RecordingScript::GetFrameCount() const
    {
        return static_cast<uint32_t>(std::llround(m_duration * m_fps));
    }

    void RecordingScript::Apply(double time, Camera& camera, SettingsRegistry& settings)
    {
        for (Change& change : m_changes)
        {
            if (change.done || time < change.start)
                continue;
            if (!change.ramp)
            {
                settings.Set(change.name, change.value);
                change.done = true;
                continue;
            }
            const double value = change.from + (change.to - change.from) * SmoothStep((time - change.start) / (change.end - change.start));
            settings.Set(change.name, change.integer ? std::to_string(std::llround(value)) : std::format("{}", value), true /*quiet*/);
            change.done = time >= change.end;
        }

        if (m_keys.empty())
            return;
        // The segment [i, i + 1] that contains 'time'.
        size_t i = 0;
        while (i + 1 < m_keys.size() && m_keys[i + 1].time <= time)
            ++i;
        if (i + 1 >= m_keys.size() || m_keys[i + 1].time == m_keys[i].time)
        {
            const Key& key = m_keys[i];
            camera.position = { key.x, key.y, key.z };
            camera.yaw = DirectX::XMConvertToRadians(key.yaw);
            camera.pitch = DirectX::XMConvertToRadians(key.pitch);
            return;
        }
        // Neighbors for the tangents; across a cut (equal times) or past the ends, the segment's own end is used.
        const Key& k1 = m_keys[i];
        const Key& k2 = m_keys[i + 1];
        const Key& k0 = (i > 0 && m_keys[i - 1].time < k1.time) ? m_keys[i - 1] : k1;
        const Key& k3 = (i + 2 < m_keys.size() && m_keys[i + 2].time > k2.time) ? m_keys[i + 2] : k2;
        const float t = static_cast<float>((time - k1.time) / (k2.time - k1.time));
        const auto spline = [&](float Key::*field) { return CatmullRom(k0.*field, k1.*field, k2.*field, k3.*field, t); };
        camera.position = { spline(&Key::x), spline(&Key::y), spline(&Key::z) };
        camera.yaw = DirectX::XMConvertToRadians(spline(&Key::yaw));
        camera.pitch = DirectX::XMConvertToRadians(spline(&Key::pitch));
    }
}
