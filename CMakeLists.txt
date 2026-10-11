cmake_minimum_required(VERSION 3.21)
project(recline_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(WIN32)
  set(VR_PLATFORM win64)
  set(VR_BINARY_PATH_KEY binary_path_windows)
  set(FRAME_DEFAULT OFF)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(VR_PLATFORM linuxarm64)
  set(VR_BINARY_PATH_KEY binary_path_linux_arm)
  set(FRAME_DEFAULT ON)
else()
  set(VR_PLATFORM linux64)
  set(VR_BINARY_PATH_KEY binary_path_linux)
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
# OS-specific code sits behind common/platform.h and driver/pose_hook.h, one
# implementation file each per platform; this block picks them and the toolchain
# flags.
if(WIN32)
  set(PLATFORM_SOURCES common/platform_win.cpp)
  # MinHook detour, as Space Calibrator uses on Windows (see pose_hook_win.cpp).
  set(POSE_HOOK_SOURCES driver/pose_hook_win.cpp)
  set(POSE_HOOK_LIBS minhook)
else()
  set(PLATFORM_SOURCES common/platform_linux.cpp)
  set(POSE_HOOK_SOURCES driver/pose_hook_linux.cpp common/slot_patch.cpp)
  set(POSE_HOOK_LIBS)
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

# The overlay dlopen's libvulkan, so it needs only the headers. Without them it
# falls back to SetOverlayRaw, which flickers on every update: required on the
# Frame, optional elsewhere.
if(RECLINE_FRAME)
  find_package(Vulkan REQUIRED)
else()
  find_package(Vulkan)
endif()

set(COMMON_INC ${CMAKE_CURRENT_SOURCE_DIR}/common ${OPENVR_INCLUDE_DIR})

# --- shared code ------------------------------------------------------------------
# Linked into the driver as well, so: position-independent, hidden symbols.
add_library(recline_shm STATIC common/recline_shm.cpp ${PLATFORM_SOURCES})
target_include_directories(recline_shm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(recline_shm PRIVATE ${WARN} ${HIDDEN})
target_link_libraries(recline_shm PUBLIC Threads::Threads ${CMAKE_DL_LIBS} ${SYSTEM_LIBS})
set_target_properties(recline_shm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- drivers: self-contained .so / .dll exporting only HmdDriverFactory ----------
function(recline_driver target name)
  add_library(${target} SHARED ${ARGN})
  target_include_directories(${target} PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
  target_compile_options(${target} PRIVATE ${WARN} ${HIDDEN})
  if(NOT WIN32)
    target_link_options(${target} PRIVATE ${SHARED_LIB_LINK}
                        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/driver/exports.map)
  endif()
  target_link_libraries(${target} PRIVATE recline_shm)
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME driver_${name}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/${name}/bin/${VR_PLATFORM}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/${name}/bin/${VR_PLATFORM})
endfunction()

set(DRIVER_SOURCES driver/driver_recline.cpp ${POSE_HOOK_SOURCES})
if(RECLINE_FRAME)
  list(APPEND DRIVER_SOURCES driver/spawn_inject.cpp)
endif()
recline_driver(driver_recline recline ${DRIVER_SOURCES})
target_link_libraries(driver_recline PRIVATE ${POSE_HOOK_LIBS})
if(RECLINE_FRAME)
  target_compile_definitions(driver_recline PRIVATE RECLINE_FRAME)
endif()

# --- XRService passthrough shim ---------------------------------------------
# Loaded into XRService only (via the driver's targeted spawn injection) to
# correct the passthrough mesh and camera poses for the recline transform. Resolves
# VR_GetGenericInterface / VRPaths at runtime via RTLD_NEXT, so it must NOT
# link libopenvr_api. Ships next to the driver .so. Frame builds only.
if(RECLINE_FRAME)
  add_library(recline_xrshim SHARED xrshim/recline_xrshim.cpp common/slot_patch.cpp
              common/recline_shm.cpp ${PLATFORM_SOURCES})
  target_include_directories(recline_xrshim PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
  target_compile_options(recline_xrshim PRIVATE ${WARN} ${HIDDEN})
  target_link_options(recline_xrshim PRIVATE ${SHARED_LIB_LINK}
                      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/xrshim/exports.map)
  target_link_libraries(recline_xrshim PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${SYSTEM_LIBS})
  set_target_properties(recline_xrshim PROPERTIES OUTPUT_NAME recline_xrshim
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/recline/bin/${VR_PLATFORM})
endif()

# --- apps -----------------------------------------------------------------------
add_library(recline_client STATIC common/recline_client.cpp)
target_include_directories(recline_client PUBLIC ${COMMON_INC})
target_compile_options(recline_client PRIVATE ${WARN})
target_link_libraries(recline_client PUBLIC recline_shm)

function(recline_app target)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE ${COMMON_INC})
  target_compile_options(${target} PRIVATE ${WARN})
  target_link_libraries(${target} PRIVATE recline_client ${OPENVR_API})
  target_link_options(${target} PRIVATE ${STATIC_RUNTIME})
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage)
endfunction()

recline_app(recline_overlay overlay/recline_overlay.cpp overlay/vk_texture.cpp)
target_include_directories(recline_overlay PRIVATE overlay)
if(Vulkan_FOUND)
  target_link_libraries(recline_overlay PRIVATE Vulkan::Headers)
  target_compile_definitions(recline_overlay PRIVATE RECLINE_HAVE_VULKAN)
else()
  message(WARNING "Vulkan headers not found: the overlay will use SetOverlayRaw, which flickers")
endif()
if(RECLINE_FRAME)
  target_compile_definitions(recline_overlay PRIVATE RECLINE_FRAME)
endif()
recline_app(reclinectl cli/reclinectl.cpp)

# --- staging ----------------------------------------------------------------------
# ${CMAKE_BINARY_DIR}/stage is the complete install layout; on Linux its install.sh
# copies it into place. The non-binary files are copied at configure time, so editing or
# adding one re-runs configure on the next build.
file(GLOB_RECURSE STAGED_FILES CONFIGURE_DEPENDS packaging/* scripts/*.sh)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STAGED_FILES})
configure_file(packaging/overlay/recline_overlay.vrmanifest.in
               ${CMAKE_BINARY_DIR}/stage/recline_overlay.vrmanifest @ONLY)
file(COPY packaging/recline/ DESTINATION ${CMAKE_BINARY_DIR}/stage/recline)
file(COPY packaging/overlay/ DESTINATION ${CMAKE_BINARY_DIR}/stage
     PATTERN "*.in" EXCLUDE PATTERN "*.svg" EXCLUDE)
if(NOT WIN32)
  file(COPY scripts/install.sh scripts/uninstall.sh DESTINATION ${CMAKE_BINARY_DIR}/stage)
endif()

# --- host unit tests (math + seqlock protocol; no SteamVR needed) ------------------
if(RECLINE_BUILD_TESTS)
  enable_testing()
  foreach(t test_math test_shm)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${COMMON_INC})
    target_compile_options(${t} PRIVATE ${WARN})
    target_link_libraries(${t} PRIVATE recline_shm)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  # The pose hook against a Space Calibrator-style hook installed after it.
  add_executable(test_pose_hook tests/test_pose_hook.cpp ${POSE_HOOK_SOURCES})
  target_include_directories(test_pose_hook PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
  target_compile_options(test_pose_hook PRIVATE ${WARN})
  target_link_libraries(test_pose_hook PRIVATE recline_shm ${POSE_HOOK_LIBS})
  if(WIN32)
    # Its own module with its own MinHook copy, as Space Calibrator's.
    add_library(fake_calibrator SHARED tests/fake_calibrator_win.cpp)
    target_include_directories(fake_calibrator PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
    target_link_libraries(fake_calibrator PRIVATE minhook)
    set_target_properties(fake_calibrator PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_link_libraries(test_pose_hook PRIVATE fake_calibrator)
  else()
    target_sources(test_pose_hook PRIVATE tests/fake_calibrator_linux.cpp)
  endif()
  add_test(NAME test_pose_hook COMMAND test_pose_hook)

  add_executable(bench_hook tests/bench_hook.cpp)
  target_include_directories(bench_hook PRIVATE ${COMMON_INC})
  target_compile_options(bench_hook PRIVATE ${WARN})
  target_link_libraries(bench_hook PRIVATE recline_shm)
endif()
