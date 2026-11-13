#pragma once

#include "renderer/DebugViews.h"

#include <imgui.h>

#include <initializer_list>

// Small ImGui helpers for the settings panel's recurring patterns.
namespace walnut::ui
{
    template <typename T>
    struct RadioOption
    {
        const char* label;
        T value;
        bool enabled = true;
        const char* tooltip = nullptr;
    };

    // A row of radio buttons, one per option, then an optional dimmed label (with its tooltip). 'value' is written
    // only when an option is clicked, so a setting that the UI shows differently (or can't offer) isn't overwritten.
    // Returns whether it changed.
    template <typename T>
    bool EnumRadio(const char* label, T& value, std::initializer_list<RadioOption<T>> options, const char* tooltip = nullptr)
    {
        bool changed = false;
        bool first = true;
        for (const RadioOption<T>& option : options)
        {
            if (!first)
                ImGui::SameLine();
            first = false;
            ImGui::BeginDisabled(!option.enabled);
            const bool clicked = ImGui::RadioButton(option.label, value == option.value);
            ImGui::EndDisabled();
            if (option.tooltip)
                ImGui::SetItemTooltip("%s", option.tooltip);
            if (clicked && !(value == option.value))
            {
                value = option.value;
                changed = true;
            }
        }
        if (label)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", label);
            if (tooltip)
                ImGui::SetItemTooltip("%s", tooltip);
        }
        return changed;
    }

    // A combo box for an enum whose values are 0, 1, 2, ... in the order of 'names'.
    template <typename E>
    bool EnumCombo(const char* label, E& value, std::initializer_list<const char*> names)
    {
        int index = static_cast<int>(value);
        if (!ImGui::Combo(label, &index, names.begin(), static_cast<int>(names.size())))
            return false;
        value = static_cast<E>(index);
        return true;
    }

    // A checkbox that switches an override on (value >= 0) or off (negative), and a 0..1 slider for its value.
    // 'lastValue' keeps the slider's value while the override is off.
    void OverrideSlider(const char* label, float& value, float& lastValue);

    // The debug views (shaders/shared/DebugViewList.h), grouped under their headings. Views the current path or GPU
    // can't show are disabled.
    void DebugViewCombo(const char* label, SceneDebugView& view, bool deferred, bool canTrace);
}
