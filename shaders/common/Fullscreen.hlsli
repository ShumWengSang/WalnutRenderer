// The "fullscreen triangle": 3 vertices made up from SV_VertexID, no vertex buffer.
//
//   id 0 -> (-1,  1)   top-left corner of the screen
//   id 1 -> ( 3,  1)   far off to the right
//   id 2 -> (-1, -3)   far off to the bottom
//
// This one oversized triangle covers the whole [-1, 1] screen square; the parts outside are clipped away.
// It's slightly cheaper than two triangles (a quad), which would shade the pixels along the diagonal twice.
#ifndef WALNUT_FULLSCREEN_HLSLI
#define WALNUT_FULLSCREEN_HLSLI

float4 FullscreenTrianglePosition(uint vertexId)
{
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2); // (0,0), (2,0), (0,2)
    return float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

#endif
