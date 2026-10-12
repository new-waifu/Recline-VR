// Persistent UI options of the overlay (overlay.conf in the user config
// directory), shared with reclinectl. The transform itself is never persisted.
#pragma once

#include <filesystem>
#include <string>

#include "recenter_math.h"

namespace recline {

struct OverlayOptions {
  RecenterOptions rc;
  bool seatedReset = true;
  // Keep the panel's icon in the SteamVR dashboard. When off, the overlay runs
  // without a dashboard entry (the input actions still work) and the panel is
  // only created while summoned.
  bool showInDashboard = true;
};

// std::filesystem path from a UTF-8 string (the platform layer's encoding).
std::filesystem::path Utf8Path(const std::string& s);

std::filesystem::path OverlayOptionsPath();

// Missing file or keys leave the defaults.
void LoadOverlayOptions(OverlayOptions* o);
bool SaveOverlayOptions(const OverlayOptions& o);

}  // namespace recline
