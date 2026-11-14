// Per-frame constants at b2 (root parameter FrameData). The layout is shaders/shared/FrameConstants.h, shared with C++.
#ifndef WALNUT_FRAME_CONSTANTS_HLSLI
#define WALNUT_FRAME_CONSTANTS_HLSLI

#include "common/Bindless.hlsli"
#include "shared/FrameConstants.h"

WALNUT_CONSTANT_BUFFER(FrameConstants, g_frame, b2);

#endif