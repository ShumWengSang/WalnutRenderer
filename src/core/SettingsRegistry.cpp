#include "core/SettingsRegistry.h"

#include "core/Log.h"

#include <DirectXMath.h>

#include <algorithm>
#include <charconv>
#include <format>

namespace walnut
{
    namespace
    {
        bool ParseFloat(std::string_view text, float& out)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        bool ParseInt(std::string_view text, int& out)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }
    }

    void SettingsRegistry::AddBool(std::string name, bool* value)
    {
        AddCustom(std::move(name),
                  [value](std::string_view v) {
                      if (v == "1" || v == "true" || v == "on") { *value = true; return true; }
                      if (v == "0" || v == "false" || v == "off") { *value = false; return true; }
                      return false;
                  },
                  [value] { return std::string(*value ? "1" : "0"); });
    }

    void SettingsRegistry::AddInt(std::string name, int* value, int min, int max)
    {
        AddCustom(std::move(name),
                  [value, min, max](std::string_view v) {
                      int parsed = 0;
                      if (!ParseInt(v, parsed))
                          return false;
                      *value = std::clamp(parsed, min, max);
                      return true;
                  },
                  [value] { return std::to_string(*value); });
    }

    void SettingsRegistry::AddFloat(std::string name, float* value)
    {
        AddCustom(std::move(name), [value](std::string_view v) { return ParseFloat(v, *value); },
                  [value] { return std::format("{}", *value); });
    }

    void SettingsRegistry::AddAngle(std::string name, float* radians)
    {
        AddCustom(std::move(name),
                  [radians](std::string_view v) {
                      float degrees = 0.0f;
                      if (!ParseFloat(v, degrees))
                          return false;
                      *radians = DirectX::XMConvertToRadians(degrees);
                      return true;
                  },
                  [radians] { return std::format("{}", DirectX::XMConvertToDegrees(*radians)); });
    }

    void SettingsRegistry::AddEnumImpl(std::string name, std::function<int()> get, std::function<void(int)> set,
                                       std::vector<EnumName> names)
    {
        AddCustom(std::move(name),
                  [set, names](std::string_view v) {
                      for (const EnumName& entry : names)
                          if (entry.name == v) { set(entry.value); return true; }
                      int number = 0;
                      if (!ParseInt(v, number))
                          return false;
                      if (std::ranges::none_of(names, [number](const EnumName& entry) { return entry.value == number; }))
                          return false;
                      set(number);
                      return true;
                  },
                  [get, names] {
                      const int current = get();
                      for (const EnumName& entry : names)
                          if (entry.value == current)
                              return std::string(entry.name);
                      return std::to_string(current);
                  });
    }

    void SettingsRegistry::AddCustom(std::string name, std::function<bool(std::string_view)> set, std::function<std::string()> get)
    {
        m_entries.push_back(Entry{ std::move(name), std::move(set), std::move(get) });
    }

    bool SettingsRegistry::Set(std::string_view name, std::string_view value)
    {
        const auto entry = std::ranges::find(m_entries, name, &Entry::name);
        if (entry == m_entries.end())
        {
            log::Warn("--set: unknown setting '{}' (see --list-settings)", name);
            return false;
        }
        if (!entry->set(value))
        {
            log::Warn("--set: bad value '{}' for '{}' (currently {})", value, name, entry->get());
            return false;
        }
        log::Info("--set {} = {}", name, entry->get());
        return true;
    }

    std::vector<std::string> SettingsRegistry::List() const
    {
        std::vector<std::string> lines;
        for (const Entry& entry : m_entries)
            lines.push_back(std::format("{} = {}", entry.name, entry.get()));
        return lines;
    }
}
