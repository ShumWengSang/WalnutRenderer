#pragma once

#include <cstddef>
#include <cstdint>

namespace walnut
{
    // DebugViewInfo::flags.
    inline constexpr uint32_t kDebugViewForwardOnly = 1;     // not available in deferred (needs a draw's own inputs)
    inline constexpr uint32_t kDebugViewNeedsRayTracing = 2; // only with the ray-traced shader variant

    // What the scene shaders output (View > Show). The list is shaders/shared/DebugViewList.h, which the shaders read
    // too (common/DebugViews.hlsli), so the values can't drift apart.
    enum class SceneDebugView : uint32_t
    {
#define WALNUT_VIEW(name, value, setting, label, flags, group) name = value,
#include "shared/DebugViewList.h"
#undef WALNUT_VIEW
    };

    struct DebugViewInfo
    {
        SceneDebugView view;
        const char* setting; // --set=view.show=<setting>
        const char* label;   // the UI's text
        uint32_t flags;      // kDebugView*
        const char* group;   // a heading shown above this view in the UI ("" for none)
    };

    // Every view, in value order (kDebugViews[i].view == i).
    inline constexpr DebugViewInfo kDebugViews[] = {
#define WALNUT_VIEW(name, value, setting, label, flags, group) { SceneDebugView::name, setting, label, flags, group },
#include "shared/DebugViewList.h"
#undef WALNUT_VIEW
    };
    inline constexpr size_t kDebugViewCount = sizeof(kDebugViews) / sizeof(kDebugViews[0]);

    static_assert([] {
        for (size_t i = 0; i < kDebugViewCount; ++i)
            if (static_cast<size_t>(kDebugViews[i].view) != i)
                return false;
        return true;
    }(), "DebugViewList.h must list the views in value order, without gaps");

    inline const DebugViewInfo& GetDebugViewInfo(SceneDebugView view)
    {
        return kDebugViews[static_cast<size_t>(view)];
    }
}
