#include "overlay_options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "platform.h"

namespace recline {

namespace fs = std::filesystem;

fs::path Utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path OverlayOptionsPath() { return Utf8Path(UserConfigDir()) / "recline-vr" / "overlay.conf"; }

void LoadOverlayOptions(OverlayOptions* o) {
  std::ifstream f(OverlayOptionsPath());
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    char* end;
    const double v = strtod(line.c_str() + eq + 1, &end);
    if (end == line.c_str() + eq + 1) continue;
    if (k == "levelOnly") o->rc.levelOnly = v != 0;
    else if (k == "keepPosition") o->rc.keepPosition = v != 0;
    else if (k == "eyeHeight" && v >= 0.2 && v <= 2.5) o->rc.eyeHeight = v;
    else if (k == "seatedReset") o->seatedReset = v != 0;
    else if (k == "showInDashboard") o->showInDashboard = v != 0;
    else if (k == "systemRecenter") o->systemRecenter = v != 0;
    else if (k == "systemRecenterTilt")
      o->systemRecenterTilt = std::clamp(v, kMinSystemRecenterTilt, kMaxSystemRecenterTilt);
  }
}

bool SaveOverlayOptions(const OverlayOptions& o) {
  const fs::path path = OverlayOptionsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    char buf[256];
    snprintf(buf, sizeof buf,
             "levelOnly=%d\nkeepPosition=%d\neyeHeight=%.2f\nseatedReset=%d\nshowInDashboard=%d\n"
             "systemRecenter=%d\nsystemRecenterTilt=%.0f\n",
             o.rc.levelOnly, o.rc.keepPosition, o.rc.eyeHeight, o.seatedReset, o.showInDashboard,
             o.systemRecenter, o.systemRecenterTilt);
    f << buf;
    if (!f) return false;
  }
  // Explicit mode (and repair of an older file): SteamVR may launch us with
  // umask 0, which would leave the file world-writable.
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read, ec);
  return true;
}

}  // namespace recline
