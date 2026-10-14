# The pose hook (pose_hook.h), shared with tests/test_pose_hook.
add_library(recline_pose_hook STATIC pose_hook_${RECLINE_OS}.cpp)
target_include_directories(recline_pose_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(recline_pose_hook PRIVATE ${WARN} ${HIDDEN})
target_link_libraries(recline_pose_hook PUBLIC recline_shm ${POSE_HOOK_LIBS})
set_target_properties(recline_pose_hook PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The driver: a self-contained .so / .dll exporting only HmdDriverFactory.
add_library(driver_recline SHARED driver_recline.cpp)
if(RECLINE_FRAME)
  target_sources(driver_recline PRIVATE spawn_inject.cpp)
  target_compile_definitions(driver_recline PRIVATE RECLINE_FRAME)
endif()
target_compile_options(driver_recline PRIVATE ${WARN} ${HIDDEN})
if(NOT WIN32)
  target_link_options(driver_recline PRIVATE ${SHARED_LIB_LINK}
                      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
endif()
target_link_libraries(driver_recline PRIVATE recline_pose_hook)
recline_uses_version(driver_recline)
set_target_properties(driver_recline PROPERTIES PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${STAGE_BIN_DIR} RUNTIME_OUTPUT_DIRECTORY ${STAGE_BIN_DIR})
