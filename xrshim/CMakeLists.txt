# XRService passthrough shim (Frame builds only). Loaded into XRService only, via
# the driver's targeted spawn injection, to correct the passthrough mesh and camera
# poses for the recline transform. Resolves VR_GetGenericInterface / VRPaths at
# runtime via RTLD_NEXT, so it must NOT link openvr_api. Ships next to the driver.
add_library(recline_xrshim SHARED recline_xrshim.cpp)
target_compile_options(recline_xrshim PRIVATE ${WARN} ${HIDDEN})
target_link_options(recline_xrshim PRIVATE ${SHARED_LIB_LINK}
                    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
target_link_libraries(recline_xrshim PRIVATE recline_shm recline_slot_patch)
set_target_properties(recline_xrshim PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${STAGE_DIR}/recline/bin/${VR_PLATFORM})
