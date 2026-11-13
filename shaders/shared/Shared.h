// Headers in shaders/shared/ are compiled by both C++ (src/ includes them as "shared/...") and DXC, so values that
// the two sides must agree on are written once. They use only what both languages accept: comments, the preprocessor
// and the macros below.
//
//   WALNUT_CONST(name, value);  C++: inline constexpr uint32_t name = value;   HLSL: static const uint name = value;
//   WALNUT_NAMESPACE_BEGIN/END  C++: namespace walnut { ... }                   HLSL: nothing
#ifndef WALNUT_SHARED_H
#define WALNUT_SHARED_H

#ifdef __HLSL_VERSION
#define WALNUT_CONST(name, value) static const uint name = (value)
#define WALNUT_NAMESPACE_BEGIN
#define WALNUT_NAMESPACE_END
#else
#include <cstdint>
#define WALNUT_CONST(name, value) inline constexpr uint32_t name = (value)
#define WALNUT_NAMESPACE_BEGIN namespace walnut {
#define WALNUT_NAMESPACE_END }
#endif

#endif
