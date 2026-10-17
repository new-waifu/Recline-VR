// Hooks for display devices whose driver takes the composited frames itself
// (IVRDriverDirectModeComponent: Steam Link's vrlink, SteamVR's PRISM display
// redirect, the Oculus driver). Such a driver gets each layer with the head
// pose it was rendered from (SubmitLayerPerEye_t::mHmdPose) and may re-project
// it to the headset's own latest pose, which undoes R.
//
// TrackedDeviceAdded is hooked to learn of display devices (HMDs and display
// redirects) as their drivers add them; SubmitLayer is then hooked on the
// device's direct-mode component. One
// implementation per platform, as for the pose hook (pose_hook.h).
#pragma once

#include <openvr_driver.h>

#include <atomic>
#include <cctype>
#include <string>

namespace recline {

using DeviceAddedFn = bool (*)(vr::IVRServerDriverHost*, const char*, vr::ETrackedDeviceClass,
                               vr::ITrackedDeviceServerDriver*);
using LayerPerEye = vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t;
using SubmitLayerFn = void (*)(vr::IVRDriverDirectModeComponent*, const LayerPerEye (&)[2]);

// Installs `hook` in front of IVRServerDriverHost::TrackedDeviceAdded. `*prev`
// receives the function to chain to before `hook` can first run.
bool InstallDeviceAddedHook(vr::IVRServerDriverHost* host, DeviceAddedFn hook,
                            std::atomic<DeviceAddedFn>* prev, std::string* why, std::string* info);

// Installs `hook` in front of `comp`'s SubmitLayer. A component whose
// SubmitLayer is already hooked (the same driver adding its HMD again) succeeds
// without change. `module` receives the path of the file the hooked
// SubmitLayer is in (left alone when already hooked).
bool InstallSubmitLayerHook(vr::IVRDriverDirectModeComponent* comp, SubmitLayerFn hook,
                            std::atomic<SubmitLayerFn>* prev, std::string* module, std::string* why,
                            std::string* info);

// The SteamVR driver name a driver library's path implies, lower case:
// ".../driver_vrlink.dll" -> "vrlink". Empty if the file isn't named driver_*.
inline std::string DriverNameFromModule(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
  const size_t dot = base.find('.');
  if (dot != std::string::npos) base.resize(dot);
  for (char& c : base) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (base.compare(0, 7, "driver_") != 0 || base.size() == 7) return {};
  return base.substr(7);
}

// True if the comma- or space-separated `list` names `name` (case-insensitive)
// or is "*".
inline bool ListNames(const std::string& list, const std::string& name) {
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && (list[i] == ',' || list[i] == ' ')) ++i;
    size_t j = i;
    while (j < list.size() && list[j] != ',' && list[j] != ' ') ++j;
    std::string item = list.substr(i, j - i);
    for (char& c : item) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (item == "*" || (!item.empty() && item == name)) return true;
    i = j;
  }
  return false;
}

}  // namespace recline
