// Debug views (View > Show): kView<name> for each view of shaders/shared/DebugViewList.h (the same list makes
// SceneDebugView in C++), shared by the forward (scene.hlsl) and deferred (deferred.hlsl) shaders.
#ifndef WALNUT_DEBUG_VIEWS_HLSLI
#define WALNUT_DEBUG_VIEWS_HLSLI

#include "shared/Constants.h"

#define WALNUT_VIEW(name, value, setting, label, flags, group) static const uint kView##name = value;
#include "shared/DebugViewList.h"
#undef WALNUT_VIEW

#endif
