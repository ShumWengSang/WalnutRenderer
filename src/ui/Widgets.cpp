#include "ui/Widgets.h"

namespace walnut::ui
{
    void OverrideSlider(const char* label, float& value, float& lastValue)
    {
        bool enabled = value >= 0.0f;
        if (ImGui::Checkbox(label, &enabled))
            value = enabled ? lastValue : -1.0f;
        ImGui::SameLine();
        ImGui::BeginDisabled(!enabled);
        ImGui::PushID(label);
        float shown = enabled ? value : lastValue;
        if (ImGui::SliderFloat("##value", &shown, 0.0f, 1.0f, "%.2f") && enabled)
            value = shown;
        lastValue = shown;
        ImGui::PopID();
        ImGui::EndDisabled();
    }

    void DebugViewCombo(const char* label, SceneDebugView& view, bool deferred, bool canTrace)
    {
        if (!ImGui::BeginCombo(label, GetDebugViewInfo(view).label))
            return;
        for (const DebugViewInfo& info : kDebugViews)
        {
            if (info.group[0] != '\0')
                ImGui::SeparatorText(info.group);
            const bool unavailable = ((info.flags & kDebugViewNeedsRayTracing) && !canTrace) ||
                                     (deferred && (info.flags & kDebugViewForwardOnly));
            ImGui::BeginDisabled(unavailable);
            if (ImGui::Selectable(info.label, info.view == view))
                view = info.view;
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}
