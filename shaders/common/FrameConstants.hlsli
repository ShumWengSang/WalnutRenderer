// Per-frame constants at b2 (root parameter FrameData). The layout is shaders/shared/FrameConstants.h, shared with C++.
#ifndef WALNUT_FRAME_CONSTANTS_HLSLI
#define WALNUT_FRAME_CONSTANTS_HLSLI

#include "shared/FrameConstants.h"

ConstantBuffer<FrameConstants> g_frame : register(b2);

#endif