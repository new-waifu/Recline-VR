# The dashboard overlay.
recline_app(recline_overlay recline_overlay.cpp overlay_log.cpp vk_texture.cpp)
if(WIN32)
  # A GUI-subsystem program, so SteamVR starting it opens no console window; it
  # logs to overlay.log instead (overlay_log.h). Still entered through main().
  set_target_properties(recline_overlay PROPERTIES WIN32_EXECUTABLE ON)
  target_link_options(recline_overlay PRIVATE /ENTRY:mainCRTStartup)
endif()
if(RECLINE_VULKAN_INCLUDE_DIR)
  target_include_directories(recline_overlay SYSTEM PRIVATE ${RECLINE_VULKAN_INCLUDE_DIR})
  target_compile_definitions(recline_overlay PRIVATE RECLINE_HAVE_VULKAN)
else()
  message(WARNING "Vulkan headers not found: the overlay will use SetOverlayRaw, which flickers")
endif()
if(RECLINE_FRAME)
  target_compile_definitions(recline_overlay PRIVATE RECLINE_FRAME)
endif()
