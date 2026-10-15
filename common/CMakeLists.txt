# The shared-memory protocol and the platform layer. Linked into the driver as
# well, so: position-independent, hidden symbols.
add_library(recline_shm STATIC recline_shm.cpp platform_${RECLINE_OS}.cpp)
target_include_directories(recline_shm PUBLIC ${COMMON_INC})
target_compile_options(recline_shm PRIVATE ${WARN} ${HIDDEN})
target_link_libraries(recline_shm PUBLIC Threads::Threads ${CMAKE_DL_LIBS} ${SYSTEM_LIBS})
set_target_properties(recline_shm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Code-pointer patching (Linux): the pose hook, spawn injection and the XRService shim.
if(NOT WIN32)
  add_library(recline_slot_patch STATIC slot_patch.cpp)
  target_include_directories(recline_slot_patch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(recline_slot_patch PRIVATE ${WARN} ${HIDDEN})
  set_target_properties(recline_slot_patch PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Client side of the protocol, for the overlay and reclinectl.
add_library(recline_client STATIC recline_client.cpp overlay_options.cpp)
target_compile_options(recline_client PRIVATE ${WARN})
# The loader's openvrpaths.vrpath reader (vrcore), for the registration check.
target_include_directories(recline_client SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/openvr/src)
target_link_libraries(recline_client PUBLIC recline_shm ${OPENVR_API})
recline_uses_version(recline_client)
