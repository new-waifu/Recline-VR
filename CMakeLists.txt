cmake_minimum_required(VERSION 3.21)
project(recline_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# VR_PLATFORM names the bin/ subdirectory, as SteamVR expects for drivers (and
# as packaging/recline_overlay.vrmanifest lists for the overlay).
if(WIN32)
  set(VR_PLATFORM win64)
  set(FRAME_DEFAULT OFF)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(VR_PLATFORM linuxarm64)
  set(FRAME_DEFAULT ON)
else()
  set(VR_PLATFORM linux64)
  set(FRAME_DEFAULT OFF)
endif()

# Steam Frame features: the XRService passthrough shim and the driver's spawn
# injection that loads it. Off elsewhere (e.g. a Linux x86-64 PC): the pose hook,
# overlay and CLI are generic.
option(RECLINE_FRAME "Build the Steam Frame specific parts" ${FRAME_DEFAULT})
option(RECLINE_BUILD_TESTS "Build host unit tests" ON)
if(WIN32 AND RECLINE_FRAME)
  message(FATAL_ERROR "RECLINE_FRAME is Linux only")
endif()

# --- platform -----------------------------------------------------------------------
# OS-specific code sits behind common/platform.h and driver/pose_hook.h, with one
# implementation file per platform (*_${RECLINE_OS}.cpp); this block picks the
# suffix, the pose hook's backend and the toolchain flags.
if(WIN32)
  set(RECLINE_OS win)
  # MinHook detour, as Space Calibrator uses on Windows (see pose_hook_win.cpp).
  set(POSE_HOOK_LIBS minhook)
else()
  set(RECLINE_OS linux)
  set(POSE_HOOK_LIBS recline_slot_patch)
endif()

if(MSVC)
  # Matching GCC's -Wall -Wextra -Wno-unused-parameter: C4100 unreferenced
  # parameter, C4244/C4267 implicit narrowing, C4324 padding from alignas.
  set(WARN /W4 /wd4100 /wd4244 /wd4267 /wd4324)
  set(HIDDEN)            # a DLL exports only what is marked __declspec(dllexport)
  set(STATIC_RUNTIME)
  set(SHARED_LIB_LINK)
  set(SYSTEM_LIBS)
  # Static CRT (like -static-libstdc++ elsewhere): nothing to ship next to the
  # binaries. Set before any target, the vendored libraries included.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
  # No incremental-link thunks or .ilk files in the shipped binaries.
  add_link_options(/INCREMENTAL:NO)
else()
  set(WARN -Wall -Wextra -Wno-unused-parameter)
  set(HIDDEN -fvisibility=hidden -fvisibility-inlines-hidden)
  set(STATIC_RUNTIME -static-libstdc++ -static-libgcc)
  include(CheckLinkerFlag)
  # Hide symbols of the statically linked C++ runtime (GNU ld / gold / lld-gnu).
  check_linker_flag(CXX "-Wl,--exclude-libs,ALL" HAVE_EXCLUDE_LIBS)
  set(SHARED_LIB_LINK ${STATIC_RUNTIME} -Wl,-z,defs)
  if(HAVE_EXCLUDE_LIBS)
    list(APPEND SHARED_LIB_LINK -Wl,--exclude-libs,ALL)
  endif()
  set(SYSTEM_LIBS rt)
endif()

# OpenVR SDK, vendored and pinned (see THIRD_PARTY.md). Its own CMakeLists builds
# the openvr_api loader as a static library, linked into the apps; the drivers and
# the shim use only the headers.
set(OPENVR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/headers")
set(USE_LIBCXX OFF CACHE BOOL "OpenVR SDK: build with libc++ under Clang")
add_subdirectory(third_party/openvr EXCLUDE_FROM_ALL)
if(WIN32)
  set(OPENVR_API openvr_api64)  # upstream's name for it there
  # openvr.h declares its API dllimport unless told the library is static.
  target_compile_definitions(${OPENVR_API} INTERFACE OPENVR_BUILD_STATIC)
else()
  set(OPENVR_API openvr_api)
endif()
# Upstream puts its outputs in its source tree, and expects C++11 (its own
# -std=c++11 would lose to our CMAKE_CXX_STANDARD).
set_target_properties(${OPENVR_API} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/openvr
                      CXX_STANDARD 11)

if(WIN32)
  # MinHook, vendored and pinned (see THIRD_PARTY.md), through its own CMakeLists.
  add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)
  # Our code only, not the vendored libraries above.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)

# The overlay dlopen's libvulkan, so it needs only the headers (FindVulkan would
# also insist on the loader library). Without them it falls back to
# SetOverlayRaw, which flickers on every update: required on the Frame and in CI,
# optional elsewhere.
option(RECLINE_REQUIRE_VULKAN "Fail without the Vulkan headers" ${RECLINE_FRAME})
find_path(RECLINE_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS ENV VULKAN_SDK PATH_SUFFIXES include)
if(NOT RECLINE_VULKAN_INCLUDE_DIR AND RECLINE_REQUIRE_VULKAN)
  message(FATAL_ERROR "Vulkan headers (vulkan/vulkan.h) not found; set VULKAN_SDK")
endif()

set(COMMON_INC ${CMAKE_CURRENT_SOURCE_DIR}/common ${OPENVR_INCLUDE_DIR})
set(STAGE_DIR ${CMAKE_BINARY_DIR}/stage)
# Where every binary goes. The generator expression keeps multi-config
# generators (Visual Studio) from appending a per-config subdirectory.
set(STAGE_BIN_DIR ${STAGE_DIR}/bin/${VR_PLATFORM}$<0:>)

# --- version ------------------------------------------------------------------------
# recline_version.h, from git on every build (see cmake/version.cmake); the
# header is only rewritten, and its users rebuilt, when the version changes.
# The same version goes into the stage's VERSION file.
set(RECLINE_VERSION "" CACHE STRING "Version to embed instead of the one git describes")
set(VERSION_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(recline_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${VERSION_DIR}/recline_version.h
          -DOUT_TEXT=${STAGE_DIR}/VERSION -DOVERRIDE=${RECLINE_VERSION}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${VERSION_DIR}/recline_version.h ${STAGE_DIR}/VERSION
  VERBATIM)

function(recline_uses_version target)
  add_dependencies(${target} recline_version)
  target_include_directories(${target} PRIVATE ${VERSION_DIR})
endfunction()

# An app linking the openvr_api loader, staged next to the driver.
function(recline_app target)
  add_executable(${target} ${ARGN})
  target_compile_options(${target} PRIVATE ${WARN})
  target_link_libraries(${target} PRIVATE recline_client ${OPENVR_API})
  target_link_options(${target} PRIVATE ${STATIC_RUNTIME})
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${STAGE_BIN_DIR})
  recline_uses_version(${target})
endfunction()

add_subdirectory(common)
add_subdirectory(driver)
if(RECLINE_FRAME)
  add_subdirectory(xrshim)
endif()
add_subdirectory(overlay)
add_subdirectory(cli)

# --- staging ----------------------------------------------------------------------
# ${STAGE_DIR} is the complete install layout, and also the driver's root (the
# directory holding driver.vrdrivermanifest):
#
#   bin/<platform>/            driver, overlay, reclinectl (and the Frame's shim)
#   resources/                 icon, driver settings, overlay input manifests
#   driver.vrdrivermanifest, recline_overlay.vrmanifest, install and uninstall
#
# A build fills one bin/<platform>/; CI merges the three into one package. The
# non-binary files are copied at configure time, so editing or adding one re-runs
# configure on the next build.
file(GLOB_RECURSE STAGED_FILES CONFIGURE_DEPENDS packaging/* scripts/*)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STAGED_FILES} LICENSE THIRD_PARTY.md)
file(COPY packaging/ DESTINATION ${STAGE_DIR} PATTERN "*.svg" EXCLUDE)
file(COPY LICENSE THIRD_PARTY.md DESTINATION ${STAGE_DIR})
# Both installers: a package built on one platform may be merged with the others.
file(COPY scripts/ DESTINATION ${STAGE_DIR})

# --- host unit tests (no SteamVR needed) -------------------------------------------
if(RECLINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
