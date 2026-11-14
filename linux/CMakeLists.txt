# The native Linux build of BuffrX. Kept beside (not inside) the Windows build so the Windows toolchain
# and its runtime staging are untouched: the shared engine sources compile here against the Linux
# runtime in work/linux-runtime, and the Linux-only layers live under src/linux.
cmake_minimum_required(VERSION 3.20)
project(BuffrX-Linux CXX C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LINUX_RUNTIME "${ROOT}/work/linux-runtime" CACHE PATH "Staged Ubuntu libobs packages")
set(TOOLCHAIN "${ROOT}/work/toolchain-linux" CACHE PATH "ImGui, GLFW, stb")
set(BUFFRX_IMGUI_ROOT "${TOOLCHAIN}/imgui-1.92.9b" CACHE PATH "Dear ImGui")
set(BUFFRX_GLFW_ROOT "${TOOLCHAIN}/glfw-3.4" CACHE PATH "GLFW")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers")
add_compile_definitions(_GNU_SOURCE=1)
include_directories(
  "${ROOT}/src"
  "${ROOT}/src/engine"
  "${ROOT}/src/engine/ui"
  "${ROOT}/src/linux"
  "${ROOT}/src/linux/compat"
  "${LINUX_RUNTIME}/usr/include/obs"
  "${LINUX_RUNTIME}/usr/include"
  "${LINUX_RUNTIME}/usr/include/x86_64-linux-gnu"
  "${BUFFRX_IMGUI_ROOT}"
  "${BUFFRX_IMGUI_ROOT}/backends"
  "${BUFFRX_IMGUI_ROOT}/misc/cpp"
  "${BUFFRX_GLFW_ROOT}/include"
  "${TOOLCHAIN}"
)

# GLFW, statically, with only X11: XWayland is present on Wayland desktops and avoids a second build
# dependency tree in the AppImage.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
add_subdirectory("${BUFFRX_GLFW_ROOT}" glfw)

add_library(buffrx-imgui STATIC
  "${BUFFRX_IMGUI_ROOT}/imgui.cpp"
  "${BUFFRX_IMGUI_ROOT}/imgui_draw.cpp"
  "${BUFFRX_IMGUI_ROOT}/imgui_tables.cpp"
  "${BUFFRX_IMGUI_ROOT}/imgui_widgets.cpp"
  "${BUFFRX_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
  "${BUFFRX_IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp"
  "${BUFFRX_IMGUI_ROOT}/misc/cpp/imgui_stdlib.cpp"
)
target_compile_options(buffrx-imgui PRIVATE -w)

# The engine's portable core plus the Linux-only layers. The Windows files for the same roles are
# listed here as the exclusions below.
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS "${ROOT}/src/engine/*.cpp" "${ROOT}/src/engine/ui/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES
  "${ROOT}/src/engine/platform.cpp"
  "${ROOT}/src/engine/ui/ui_host.cpp"
  "${ROOT}/src/engine/ui/web_clip.cpp"
  "${ROOT}/src/engine/toast.cpp"
  "${ROOT}/src/engine/screenshot.cpp"
  "${ROOT}/src/engine/capture_enum.cpp"
  "${ROOT}/src/engine/gpu.cpp"
  "${ROOT}/src/engine/sleep_patch.cpp"
  "${ROOT}/src/engine/crash_log.cpp"
  "${ROOT}/src/engine/rec_mark.cpp"
  "${ROOT}/src/engine/audio_sessions.cpp"
  "${ROOT}/src/engine/process_tuning.cpp"
  "${ROOT}/src/engine/share.cpp"
  "${ROOT}/src/engine/link.cpp"
  "${ROOT}/src/engine/discord_webhook.cpp"
  "${ROOT}/src/engine/video_probe.cpp"
  "${ROOT}/src/engine/update_feed.cpp"
  "${ROOT}/src/engine/update_download.cpp"
  "${ROOT}/src/engine/update_apply.cpp"
)
file(GLOB LINUX_SOURCES CONFIGURE_DEPENDS "${ROOT}/src/linux/*.cpp")
list(APPEND ENGINE_SOURCES ${LINUX_SOURCES} "${ROOT}/src/engine/update_feed.cpp")

add_executable(buffrx-engine ${ENGINE_SOURCES})
target_include_directories(buffrx-engine PRIVATE "${ROOT}/src/shared" ${GTK3_INCLUDE_DIRS})
target_compile_definitions(buffrx-engine PRIVATE BUFFRX_LINUX=1)
target_compile_options(buffrx-engine PRIVATE -include "${ROOT}/src/linux/compat/crt.h")
target_link_directories(buffrx-engine PRIVATE
  "${LINUX_RUNTIME}/usr/lib/x86_64-linux-gnu"
)
target_link_libraries(buffrx-engine PRIVATE
  buffrx-imgui
  glfw
  obs
  avformat avcodec avutil swscale swresample avfilter
  curl crypto
  X11 Xrandr ${GTK3_LIBRARIES}
  GL pthread dl m z
)
set_target_properties(buffrx-engine PROPERTIES
  BUILD_RPATH "${LINUX_RUNTIME}/usr/lib/x86_64-linux-gnu"
  INSTALL_RPATH "$ORIGIN/../lib/buffrx/libobs"
)

add_library(buffrx-pathshim SHARED "${ROOT}/src/linux/pathshim.c")
target_compile_definitions(buffrx-pathshim PRIVATE _LARGEFILE64_SOURCE=1)
set_target_properties(buffrx-pathshim PROPERTIES OUTPUT_NAME buffrx-pathshim PREFIX "")
target_link_libraries(buffrx-pathshim PRIVATE dl)
