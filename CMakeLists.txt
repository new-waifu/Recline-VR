cmake_minimum_required(VERSION 3.16)
project(recline_vr CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# OpenVR headers (openvr.h, openvr_driver.h).
set(OPENVR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../openvr" CACHE PATH "Directory with openvr.h / openvr_driver.h")
# Directory containing libopenvr_api.so (the device's own copy by default).
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory with libopenvr_api.so")
option(RECLINE_BUILD_TESTS "Build host unit tests" ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(VR_PLATFORM linuxarm64)
else()
  set(VR_PLATFORM linux64)
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
  target_link_libraries(${target} PRIVATE recline_shm pthread dl rt)
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME driver_${name}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/${name}/bin/${VR_PLATFORM})
  install(TARGETS ${target} LIBRARY DESTINATION ${name}/bin/${VR_PLATFORM})
  install(DIRECTORY packaging/${name}/ DESTINATION ${name})
endfunction()

recline_driver(driver_recline recline driver/driver_recline.cpp common/slot_patch.cpp
               driver/spawn_inject.cpp)

# --- XRService passthrough shim ---------------------------------------------
# Loaded into XRService only (via the driver's targeted spawn injection) to
# correct the passthrough mesh and camera poses for the recline transform. Resolves
# VR_GetGenericInterface / VRPaths at runtime via RTLD_NEXT, so it must NOT
# link libopenvr_api. Ships next to the driver .so.
add_library(recline_xrshim SHARED xrshim/recline_xrshim.cpp common/slot_patch.cpp
            common/recline_shm.cpp)
target_include_directories(recline_xrshim PRIVATE ${COMMON_INC} ${CMAKE_CURRENT_SOURCE_DIR}/driver)
target_compile_options(recline_xrshim PRIVATE ${WARN} -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(recline_xrshim PRIVATE -static-libstdc++ -static-libgcc ${DRIVER_EXTRA_LINK}
                    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/xrshim/exports.map -Wl,-z,defs)
target_link_libraries(recline_xrshim PRIVATE pthread dl rt)
set_target_properties(recline_xrshim PROPERTIES OUTPUT_NAME recline_xrshim
  POSITION_INDEPENDENT_CODE ON
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage/recline/bin/${VR_PLATFORM})
install(TARGETS recline_xrshim LIBRARY DESTINATION recline/bin/${VR_PLATFORM})

# --- apps -----------------------------------------------------------------------
function(recline_app target)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE ${COMMON_INC})
  target_compile_options(${target} PRIVATE ${WARN})
  target_link_directories(${target} PRIVATE ${OPENVR_LIB_DIR})
  target_link_libraries(${target} PRIVATE recline_client openvr_api pthread rt)
  target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  set_target_properties(${target} PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "${OPENVR_LIB_DIR}"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage)
  install(TARGETS ${target} RUNTIME DESTINATION .)
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
  recline_app(reclinectl cli/reclinectl.cpp)
else()
  message(WARNING "libopenvr_api.so not found in OPENVR_LIB_DIR=${OPENVR_LIB_DIR}: "
                  "skipping recline_overlay and reclinectl")
endif()

install(FILES packaging/overlay/recline_overlay.vrmanifest packaging/overlay/actions.json
              packaging/overlay/bindings_frame_controller.json packaging/overlay/bindings_knuckles.json
        DESTINATION .)
install(PROGRAMS scripts/install.sh scripts/uninstall.sh DESTINATION .)

# Stage the non-binary files next to the build outputs too, so the build dir
# itself is a runnable layout.
file(COPY packaging/recline/ DESTINATION ${CMAKE_BINARY_DIR}/stage/recline)
file(COPY packaging/overlay/ scripts/install.sh scripts/uninstall.sh DESTINATION ${CMAKE_BINARY_DIR}/stage)

# --- host unit tests (math + seqlock protocol; no SteamVR needed) ------------------
if(RECLINE_BUILD_TESTS)
  enable_testing()
  foreach(t test_math test_shm)
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${COMMON_INC})
    target_compile_options(${t} PRIVATE ${WARN})
    target_link_libraries(${t} PRIVATE recline_shm pthread rt)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  add_executable(bench_hook tests/bench_hook.cpp)
  target_include_directories(bench_hook PRIVATE ${COMMON_INC})
  target_compile_options(bench_hook PRIVATE ${WARN})
  target_link_libraries(bench_hook PRIVATE recline_shm)
endif()
