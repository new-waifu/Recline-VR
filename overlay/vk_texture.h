// Persistent Vulkan texture for the dashboard overlay.
//
// SetOverlayRaw allocates a fresh shared-memory texture block on every call
// (vrclient even warns it is meant for images of "a hundred or two pixels"),
// and each replacement shows as a flicker. Instead, the software-rendered
// canvas is copied into one of two persistent VkImages and handed over with
// SetOverlayTexture. libvulkan is dlopen'ed, so a missing loader or a device
// without the required extensions just means the caller falls back to
// SetOverlayRaw.
#pragma once

#include <openvr.h>

#include <cstdint>
#include <memory>
#include <string>

namespace recline {

class VkOverlayTexture {
 public:
  VkOverlayTexture();
  ~VkOverlayTexture();
  VkOverlayTexture(const VkOverlayTexture&) = delete;
  VkOverlayTexture& operator=(const VkOverlayTexture&) = delete;

  // Creates the Vulkan instance, device and two w x h RGBA8 images. False (with
  // a reason) if Vulkan is unavailable; the caller then uses SetOverlayRaw.
  bool Init(uint32_t w, uint32_t h, std::string* err);

  // Copies tightly packed RGBA8 pixels (w*h*4) to the next image and sets it
  // as the overlay texture.
  bool Upload(vr::VROverlayHandle_t overlay, const uint8_t* rgba, std::string* err);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace recline
