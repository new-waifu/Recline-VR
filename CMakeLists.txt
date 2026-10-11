cmake_minimum_required(VERSION 3.21)
project(recline_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(VR_PLATFORM linuxarm64)
  set(VR_BINARY_PATH_KEY binary_path_linux_arm)
  set(FRAME_DEFAULT ON)
else()
  set(VR_PLATFORM linux64)
  set(VR_BINARY_PATH_KEY binary_path_linux)
  set(FRAME_DEFAULT OFF)
endif()

# Steam Frame features: the XRService passthrough shim and the driver's spawn
# injection that loads it, plus linking the runtime's fixed /opt/steamvr copy of
# libopenvr_api.so. Off elsewhere (e.g. a Linux x86-64 PC): the pose hook, overlay
# and CLI are generic.
option(RECLINE_FRAME "Build the Steam Frame specific parts" ${FRAME_DEFAULT})
option(RECLINE_BUILD_TESTS "Build host unit tests" ON)

# OpenVR SDK headers, vendored and pinned (see THIRD_PARTY.md).
set(OPENVR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr" CACHE PATH "Directory with openvr.h / openvr_driver.h")
# Directory containing libopenvr_api.so. On the Frame the apps use the runtime's own
# copy in place; elsewhere the install location varies, so it is bundled next to
# the apps instead.
if(RECLINE_FRAME)
  set(OPENVR_LIB_DEFAULT "/opt/steamvr/bin/linuxarm64")
else()
  set(OPENVR_LIB_DEFAULT "$ENV{HOME}/.steam/steam/steamapps/common/SteamVR/bin/linux64")
endif()
set(OPENVR_LIB_DIR "${OPENVR_LIB_DEFAULT}" CACHE PATH "Directory with libopenvr_api.so")

find_package(Threads REQUIRED)

# The overlay dlopen's libvulkan, so it needs only the headers. Without them it
# falls back to SetOverlayRaw, which flickers on every update: required on the
# Frame, optional elsewhere.
if(RECLINE_FRAME)
  find_package(Vulkan REQUIRED)
else()
  find_package(Vulkan)
endif()

include(CheckLinkerFlag)
# Hide symbols of the statically linked C++ runtime (GNU ld / gold / lld-gnu).
check_linker_flag(CXX "-Wl,--exclude-libs,ALL" HAVE_EXCLUDE_LIBS)
set(DRIVER_EXTRA_LINK)
if(HAVE_EXCLUDE_LIBS)
  set(DRIVER_EXTRA_LINK -Wl,--exclude-libs,ALL)
endif()

set(WARN -Wall -Wextra -Wno-unused-parameter)
set(COMMON_INC ${CMAKE_CURRENT_SOURCE_DIR}/common ${OPENVR_INCLUDE_DIR})

# --- shared code ------------------------------------------------------------------
# Linked into the driver .so as well, so: position-independent, hidden symbols.
add_library(recline_shm STATIC common/recline_shm.cpp)
target_include_directories(recline_shm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(recline_shm PRIVATE ${WARN} -fvisibility=hidden -fvisibility-inlines-hidden)
set_target_properties(recline_shm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- drivers: self-contained .so exporting only HmdDriverFactory -------------
function(recline_driver target name)
  add_library(${target} SHARED ${ARGN})
  target_include_directories(${target} PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
  target_compile_options(${target} PRIVATE ${WARN} -fvisibility=hidden -fvisibility-inlines-hidden)
  target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc ${DRIVER_EXTRA_LINK}
                      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/driver/exports.map -Wl,-z,defs)
  target_link_libraries(${target} PRIVATE recline_shm Threads::Threads ${CMAKE_DL_LIBS} rt)
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME driver_${name}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/${name}/bin/${VR_PLATFORM})
endfunction()

set(DRIVER_SOURCES driver/driver_recline.cpp common/slot_patch.cpp)
if(RECLINE_FRAME)
  list(APPEND DRIVER_SOURCES driver/spawn_inject.cpp)
endif()
recline_driver(driver_recline recline ${DRIVER_SOURCES})
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
              common/recline_shm.cpp)
  target_include_directories(recline_xrshim PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
  target_compile_options(recline_xrshim PRIVATE ${WARN} -fvisibility=hidden -fvisibility-inlines-hidden)
  target_link_options(recline_xrshim PRIVATE -static-libstdc++ -static-libgcc ${DRIVER_EXTRA_LINK}
                      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/xrshim/exports.map -Wl,-z,defs)
  target_link_libraries(recline_xrshim PRIVATE Threads::Threads ${CMAKE_DL_LIBS} rt)
  set_target_properties(recline_xrshim PROPERTIES OUTPUT_NAME recline_xrshim
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/recline/bin/${VR_PLATFORM})
endif()

# --- apps -----------------------------------------------------------------------
if(RECLINE_FRAME)
  set(APP_RPATH "${OPENVR_LIB_DIR}")
else()
  set(APP_RPATH "$ORIGIN")
endif()
function(recline_app target)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE ${COMMON_INC})
  target_compile_options(${target} PRIVATE ${WARN})
  target_link_directories(${target} PRIVATE ${OPENVR_LIB_DIR})
  target_link_libraries(${target} PRIVATE recline_client openvr_api Threads::Threads rt)
  target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  set_target_properties(${target} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "${APP_RPATH}"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage)
endfunction()

# The apps need the runtime's libopenvr_api.so for the target architecture.
# A host (x86-64) build without one still builds the drivers and the tests.
find_library(OPENVR_API_LIB openvr_api PATHS ${OPENVR_LIB_DIR} NO_DEFAULT_PATH)
if(OPENVR_API_LIB)
  add_library(recline_client STATIC common/recline_client.cpp)
  target_include_directories(recline_client PUBLIC ${COMMON_INC})
  target_compile_options(recline_client PRIVATE ${WARN})
  target_link_libraries(recline_client PUBLIC recline_shm)

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
  if(NOT RECLINE_FRAME)
    file(COPY ${OPENVR_API_LIB} DESTINATION ${CMAKE_BINARY_DIR}/stage)
  endif()
elseif(RECLINE_FRAME)
  message(FATAL_ERROR "libopenvr_api.so not found in OPENVR_LIB_DIR=${OPENVR_LIB_DIR}")
else()
  message(WARNING "libopenvr_api.so not found in OPENVR_LIB_DIR=${OPENVR_LIB_DIR}: "
                  "skipping recline_overlay and reclinectl")
endif()

# --- staging ----------------------------------------------------------------------
# ${CMAKE_BINARY_DIR}/stage is the complete install layout; its install.sh copies
# it into place. The non-binary files are copied at configure time, so editing or
# adding one re-runs configure on the next build.
file(GLOB_RECURSE STAGED_FILES CONFIGURE_DEPENDS packaging/* scripts/*.sh)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STAGED_FILES})
configure_file(packaging/overlay/recline_overlay.vrmanifest.in
               ${CMAKE_BINARY_DIR}/stage/recline_overlay.vrmanifest @ONLY)
file(COPY packaging/recline/ DESTINATION ${CMAKE_BINARY_DIR}/stage/recline)
file(COPY packaging/overlay/ scripts/install.sh scripts/uninstall.sh DESTINATION ${CMAKE_BINARY_DIR}/stage
     PATTERN "*.in" EXCLUDE PATTERN "*.svg" EXCLUDE)

# --- host unit tests (math + seqlock protocol; no SteamVR needed) ------------------
if(RECLINE_BUILD_TESTS)
  enable_testing()
  foreach(t test_math test_shm)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${COMMON_INC})
    target_compile_options(${t} PRIVATE ${WARN})
    target_link_libraries(${t} PRIVATE recline_shm Threads::Threads rt)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  add_executable(bench_hook tests/bench_hook.cpp)
  target_include_directories(bench_hook PRIVATE ${COMMON_INC})
  target_compile_options(bench_hook PRIVATE ${WARN})
  target_link_libraries(bench_hook PRIVATE recline_shm)
endif()
