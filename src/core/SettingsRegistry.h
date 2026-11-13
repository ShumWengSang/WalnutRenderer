#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace walnut
{
    // Named settings that can be changed from text (--set=name=value) and listed (--list-settings): a small table of
    // getters and setters over the application's settings structs. The UI doesn't use it; it exists so runs can be
    // scripted and reproduced (screenshots, captures, comparisons) without temporary code.
    class SettingsRegistry
    {
    public:
        struct EnumName
        {
            std::string_view name;
            int value;
        };

        void AddBool(std::string name, bool* value);
        void AddInt(std::string name, int* value, int min, int max);
        void AddFloat(std::string name, float* value);
        // A float shown and set in degrees, stored in radians.
        void AddAngle(std::string name, float* radians);
        // An enum set by name or by number.
        template <typename E>
        void AddEnum(std::string name, E* value, std::vector<EnumName> names)
        {
            AddEnumImpl(std::move(name), [value] { return static_cast<int>(*value); },
                        [value](int v) { *value = static_cast<E>(v); }, std::move(names));
        }
        // Anything else: a custom setter (returns false for a bad value) and getter.
        void AddCustom(std::string name, std::function<bool(std::string_view)> set, std::function<std::string()> get);

        // Returns false (and logs why) for an unknown name or a bad value.
        bool Set(std::string_view name, std::string_view value);
        // "name = value" for every setting.
        std::vector<std::string> List() const;

    private:
        struct Entry
        {
            std::string name;
            std::function<bool(std::string_view)> set;
            std::function<std::string()> get;
        };
        void AddEnumImpl(std::string name, std::function<int()> get, std::function<void(int)> set, std::vector<EnumName> names);
        std::vector<Entry> m_entries;
    };
}
