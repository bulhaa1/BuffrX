# BuffrX build. Two executables: the engine (console subsystem so its smoke tests can print) and the
# UI shell (windows subsystem, WebView2).
#
# Configure with the pinned toolchain:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_C_COMPILER=<llvm>/bin/x86_64-w64-mingw32-clang.exe \
#     -DCMAKE_CXX_COMPILER=<llvm>/bin/x86_64-w64-mingw32-clang++.exe \
#     -DCMAKE_SYSTEM_NAME=Windows
# The runtime/ folder (libobs DLLs, import libraries, headers, modules, data) comes from
# tools/prepare-runtime.ps1 and is never edited by hand.

cmake_minimum_required(VERSION 3.20)
project(BuffrX CXX)

option(BUFFRX_TESTS_ONLY "Build native logic checks without OBS or product assets" OFF)

# Resources (the engine manifest) are compiled by the toolchain's own llvm-windres; tools/build.ps1
# passes it as CMAKE_RC_COMPILER.
if(NOT BUFFRX_TESTS_ONLY)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Automatic update download and silent install are enabled by default, including in unsigned builds by user decision.
# Turning the switch off returns the product to check-and-notify; test actual release artifacts with Defender.
option(BUFFRX_AUTO_UPDATE "Download and install updates automatically" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(BUFFRX_RUNTIME "${CMAKE_SOURCE_DIR}/runtime" CACHE PATH "Staged OBS runtime (bin, data, modules, lib)")
if(NOT BUFFRX_TESTS_ONLY AND NOT EXISTS "${BUFFRX_RUNTIME}/bin/64bit/obs.dll")
  message(FATAL_ERROR "runtime/ is not staged. Run tools/prepare-runtime.ps1 first.")
endif()

add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wno-unused-parameter;-O2;-g>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(BUFFRX_SHARED_SOURCES
  src/shared/json.h
  src/shared/log.h
  src/shared/pipe.h
)

if(NOT BUFFRX_TESTS_ONLY)
# ---------------------------------------------------------------- interface library
# Dear ImGui (MIT, pinned by tools/bootstrap-toolchain.ps1): the engine's own interface is drawn with
# it, in-process. Third-party code, compiled without our warnings.
set(BUFFRX_IMGUI_ROOT "" CACHE PATH "Dear ImGui sources (work/toolchain/imgui/imgui-<version>)")
if(NOT EXISTS "${BUFFRX_IMGUI_ROOT}/imgui.cpp")
  message(FATAL_ERROR "Dear ImGui is not staged. Run tools/bootstrap-toolchain.ps1 and build through tools/build.ps1.")
endif()
add_library(buffrx-imgui STATIC
  ${BUFFRX_IMGUI_ROOT}/imgui.cpp
  ${BUFFRX_IMGUI_ROOT}/imgui_draw.cpp
  ${BUFFRX_IMGUI_ROOT}/imgui_tables.cpp
  ${BUFFRX_IMGUI_ROOT}/imgui_widgets.cpp
  ${BUFFRX_IMGUI_ROOT}/backends/imgui_impl_win32.cpp
  ${BUFFRX_IMGUI_ROOT}/backends/imgui_impl_dx11.cpp
  ${BUFFRX_IMGUI_ROOT}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(buffrx-imgui PUBLIC "${BUFFRX_IMGUI_ROOT}")
target_compile_options(buffrx-imgui PRIVATE -w)
target_link_libraries(buffrx-imgui PUBLIC d3d11 dxgi dcomp d3dcompiler dwmapi imm32 user32 gdi32)

# ---------------------------------------------------------------- engine
add_executable(buffrx-engine
  src/engine/main.cpp
  src/engine/engine.rc
  src/engine/benchmark.cpp
  src/engine/buffer_watch.cpp
  src/engine/capture_enum.cpp
  src/engine/clip_check.cpp
  src/engine/config.cpp
  src/engine/commands.cpp
  src/engine/encoder_settings.cpp
  src/engine/file_final.cpp
  src/engine/gamepad.cpp
  src/engine/gamepad_poll.cpp
  src/engine/gpu.cpp
  src/engine/library.cpp
  src/engine/library_media.cpp
  src/engine/media.cpp
  src/engine/media_export.cpp
  src/engine/media_math.cpp
  src/engine/encoder_choice.cpp
  src/engine/save_filing.cpp
  src/engine/export_job_json.cpp
  src/engine/stitch_plan.cpp
  src/engine/mp4_edit.cpp
  src/engine/obs_host.cpp
  src/engine/audio_sessions.cpp
  src/engine/profile_probe.cpp
  src/engine/rec_mark.cpp
  src/engine/crash_log.cpp
  src/engine/platform.cpp
  src/engine/screenshot.cpp
  src/engine/session_store.cpp
  src/engine/share.cpp
  src/engine/share_math.cpp
  src/engine/link.cpp
  src/engine/link_math.cpp
  src/engine/short_link.cpp
  src/engine/update_feed.cpp
  src/engine/update_download.cpp
  src/engine/update_pending.cpp
  src/engine/update_apply.cpp
  src/engine/discord_webhook.cpp
  src/engine/toast.cpp
  src/engine/game_watch.cpp
  src/engine/process_tuning.cpp
  src/engine/sleep_patch.cpp
  src/engine/ring_output.cpp
  src/engine/obs_host_dedupe.cpp
  src/engine/ring_disk.cpp
  src/engine/performance.cpp
  src/engine/video_probe.cpp
  src/engine/ui/ui_bridge.cpp
  src/engine/ui/ui_format.cpp
  src/engine/ui/ui_host.cpp
  src/engine/ui/web_clip.cpp
  src/engine/ui/ui_icons.cpp
  src/engine/ui/ui_images.cpp
  src/engine/ui/ui_panel.cpp
  src/engine/ui/ui_live.cpp
  src/engine/ui/ui_parts.cpp
  src/engine/ui/ui_settings.cpp
  src/engine/ui/ui_store.cpp
  src/engine/ui/ui_theme.cpp
  src/engine/ui/ui_tray.cpp
  src/engine/ui/ui_widgets.cpp
  ${BUFFRX_SHARED_SOURCES}
)

target_include_directories(buffrx-engine PRIVATE
  "${BUFFRX_RUNTIME}/include"
  "${BUFFRX_RUNTIME}/include/libobs"
  "${BUFFRX_RUNTIME}/include/ffmpeg"
  "${BUFFRX_WEBVIEW2_INCLUDE}"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src-tauri/icons"
)
target_link_directories(buffrx-engine PRIVATE "${BUFFRX_RUNTIME}/lib")
# The MinGW C++ runtime is shipped beside the binaries (libc++.dll, libunwind.dll), the way every
# other native build of this toolchain does it. Everything else stays dynamic as intended: obs.dll is
# loaded from bin/64bit, and the system DLLs are part of Windows.
target_link_libraries(buffrx-engine PRIVATE
  obs
  buffrx-imgui
  avformat avcodec avutil swscale avfilter
  user32 gdi32 shell32 advapi32 ole32 oleaut32 uuid comdlg32 shlwapi windowscodecs psapi wtsapi32 dxgi mmdevapi
  d2d1 dwrite winmm crypt32 winhttp bcrypt
)
# The engine is a console binary on purpose: --capture-smoke-test and --package-smoke-test print
# machine-readable results, and the launcher starts it with CREATE_NO_WINDOW so no console flashes.
target_compile_definitions(buffrx-engine PRIVATE BUFFRX_ENGINE=1)
# The build switch behind update_policy.h: disabling it returns the build to check-and-notify.
if(BUFFRX_AUTO_UPDATE)
  target_compile_definitions(buffrx-engine PRIVATE BUFFRX_AUTO_UPDATE=1)
endif()

# ---------------------------------------------------------------- measurement tools
# Not part of the package: build.ps1 leaves anything named buffrx-*-tool.exe out of the staging step.
# The present loop is the honest substitute for a game when measuring FPS overhead (see
# docs/measurement.md).
add_executable(buffrx-present-loop src/tools/present_loop.cpp)
target_link_libraries(buffrx-present-loop PRIVATE d3d11 dxgi user32)

endif()

# ---------------------------------------------------------------- tests
# The pure logic only: config, JSON, hotkey chords and library guards. No libobs, no GPU, no window,
# so this target runs anywhere and guards the units a refactor is most likely to break.
set(BUFFRX_TEST_SOURCES
  src/engine/ui/ui_format.cpp
  src/engine/ui/ui_icons.cpp
  src/engine/benchmark.cpp
  src/engine/buffer_watch.cpp
  src/engine/capture_enum.cpp
  src/engine/clip_check.cpp
  src/engine/config.cpp
  src/engine/encoder_settings.cpp
  src/engine/file_final.cpp
  src/engine/gamepad.cpp
  src/engine/gpu.cpp
  src/engine/library.cpp
  src/engine/media_math.cpp
  src/engine/encoder_choice.cpp
  src/engine/save_filing.cpp
  src/engine/export_job_json.cpp
  src/engine/stitch_plan.cpp
  src/engine/mp4_edit.cpp
  src/engine/platform.cpp
  src/engine/rec_mark.cpp
  src/engine/session_store.cpp
  src/engine/share_math.cpp
  src/engine/link.cpp
  src/engine/link_math.cpp
  src/engine/short_link.cpp
  src/engine/toast.cpp
  src/engine/game_watch.cpp
  src/engine/process_tuning.cpp
  src/engine/ring_disk.cpp
  src/engine/performance.cpp
  src/engine/update_feed.cpp
  src/engine/update_download.cpp
  src/engine/update_pending.cpp
  src/engine/update_apply.cpp
  tests/link_secret_tests.cpp
  tests/ring_save_tests.cpp
  tests/save_publish_tests.cpp
  ${BUFFRX_SHARED_SOURCES}
)
add_executable(buffrx-tests tests/engine_tests.cpp ${BUFFRX_TEST_SOURCES})
target_include_directories(buffrx-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
# The tests read ui/src/themes.css to hold the editor page to the engine's theme table.
target_compile_definitions(buffrx-tests PRIVATE BUFFRX_SOURCE_DIR="${CMAKE_SOURCE_DIR}" BUFFRX_TEST_HOOKS)
target_link_libraries(buffrx-tests PRIVATE user32 gdi32 shell32 advapi32 ole32 comdlg32 wtsapi32 dxgi d2d1 dwrite winmm crypt32 bcrypt winhttp)

enable_testing()
add_test(NAME buffrx-tests COMMAND buffrx-tests)
set_tests_properties(buffrx-tests PROPERTIES TIMEOUT 180)
if(BUFFRX_TESTS_ONLY)
  # This target links no OBS; static toolchain libraries keep its run independent of runtime/.
  target_link_options(buffrx-tests PRIVATE -static)
  return()
endif()

# The mutation harness (N4): the same units, fed mutated bytes. Test-only, never packaged.
add_executable(buffrx-fuzz tests/buffrx_fuzz.cpp ${BUFFRX_TEST_SOURCES})
target_include_directories(buffrx-fuzz PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(buffrx-fuzz PRIVATE BUFFRX_SOURCE_DIR="${CMAKE_SOURCE_DIR}" BUFFRX_TEST_HOOKS)
target_link_libraries(buffrx-fuzz PRIVATE user32 gdi32 shell32 advapi32 ole32 comdlg32 wtsapi32 dxgi d2d1 dwrite winmm crypt32 bcrypt winhttp)

# ---------------------------------------------------------------- launcher stubs
# The package entry: tools/package.ps1 copies it to the package root as BuffrX.exe. It checks the package and
# starts bin/64bit/buffrx-engine.exe --show.
add_executable(buffrx-launcher src/launcher/main.cpp src/launcher/launcher.rc)
target_include_directories(buffrx-launcher PRIVATE src-tauri/icons)
# The icon is a binary the resource compiler reads through the .rc; without this a new icon.ico never reached the exes.
set_source_files_properties(src/engine/engine.rc src/launcher/launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src-tauri/icons/icon.ico;${CMAKE_SOURCE_DIR}/src/launcher/launcher.manifest")
target_link_libraries(buffrx-launcher PRIVATE user32 shell32)
# It runs from the package root, where libc++.dll and libunwind.dll are not (they sit in bin\64bit beside
# the engine), so its C++ runtime is linked in. Only here: it links no libobs (the -static trap in
# AGENTS.md is the engine's).
target_link_options(buffrx-launcher PRIVATE -static)





