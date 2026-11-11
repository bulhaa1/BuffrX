// Whether this build may fetch and install an update by itself. CMake and the standard build enable it by
// default, including for unsigned releases by user decision. Turning the switch off returns the product to
// check-and-notify with no other change; that is the easy revert. Builds without a definition default to off.
#pragma once

#ifndef BUFFRX_AUTO_UPDATE
#define BUFFRX_AUTO_UPDATE 0
#endif

namespace bx {

inline constexpr bool kAutoUpdate = BUFFRX_AUTO_UPDATE != 0;

}  // namespace bx
