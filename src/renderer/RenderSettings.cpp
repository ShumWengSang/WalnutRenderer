#include "renderer/RenderSettings.h"

#include "renderer/PointShadowMaps.h"

#include <algorithm>

namespace walnut
{
    FrameSettings ResolveFrameSettings(const RenderSettings& settings, const DebugSettings& debug, const RenderCaps& caps)
    {
        FrameSettings frame;
        frame.deferred = settings.path == RenderPath::Deferred;
        frame.depthPrepass = settings.depthPrepass || frame.deferred;
        // Brute force doesn't read the grid (except the heat map view), so it's skipped there: a fair comparison.
        frame.buildClusters = settings.clusteredLighting || debug.view == SceneDebugView::ClusterLights;

        // Sun. The difference view traces rays AND samples the shadow map, to compare them; it doesn't change the
        // chosen technique, so leaving the view goes back to it.
        frame.sunShadows = settings.sunShadows.enabled;
        frame.compareSunShadows = caps.raytracing && debug.view == SceneDebugView::ShadowDifference;
        frame.sunRaytraced = caps.raytracing && (settings.sunShadows.raytraced || frame.compareSunShadows);
        // With ray-traced shadows nothing reads the shadow map, so its casters are skipped (the pass still clears it,
        // which keeps it valid for the UI's shadow map view).
        frame.drawSunCasters = !frame.sunRaytraced || frame.compareSunShadows;

        // Point lights. The difference view lights with cube maps and also traces rays to compare.
        frame.comparePointShadows = caps.raytracing && debug.view == SceneDebugView::PointShadowDifference;
        PointShadowTechnique technique = settings.pointShadows.technique;
        if (frame.comparePointShadows || (technique == PointShadowTechnique::RayTraced && !caps.raytracing))
            technique = PointShadowTechnique::CubeMaps;
        frame.pointShadows = technique;
        frame.cubeShadowBudget = technique != PointShadowTechnique::CubeMaps ? 0u :
            std::min<uint32_t>(static_cast<uint32_t>(std::max(settings.pointShadows.budget, 0)),
                               PointShadowMaps::kMaxShadowedLights);

        frame.raytracedVariant = frame.sunRaytraced || technique == PointShadowTechnique::RayTraced || frame.comparePointShadows;

        frame.view = debug.view;
        frame.shading = settings.shading;
        frame.normalMapping = settings.normalMapping;
        frame.alphaTest = settings.alphaTest;
        frame.backfaceCulling = settings.backfaceCulling;
        return frame;
    }
}
