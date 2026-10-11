# Host unit tests: recenter math, the seqlock protocol, and the pose hook against
# a Space Calibrator-style hook installed after it. bench_hook is run by hand.
foreach(t test_math test_shm test_pose_hook bench_hook)
  add_executable(${t} ${t}.cpp)
  target_compile_options(${t} PRIVATE ${WARN})
  target_link_libraries(${t} PRIVATE recline_shm)
endforeach()
foreach(t test_math test_shm test_pose_hook)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

target_link_libraries(test_pose_hook PRIVATE recline_pose_hook)
if(WIN32)
  # Its own module with its own MinHook copy, as Space Calibrator's.
  add_library(fake_calibrator SHARED fake_calibrator_win.cpp)
  target_include_directories(fake_calibrator PRIVATE ${COMMON_INC} ${PROJECT_SOURCE_DIR}/driver)
  target_link_libraries(fake_calibrator PRIVATE minhook)
  set_target_properties(fake_calibrator PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_link_libraries(test_pose_hook PRIVATE fake_calibrator)
else()
  target_sources(test_pose_hook PRIVATE fake_calibrator_linux.cpp)
endif()
