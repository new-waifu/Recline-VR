// recline_overlay: SteamVR dashboard overlay + global input actions that
// drive driver_recline over /recline_v1.
//
//   recline_overlay               run as overlay (VRApplication_Overlay)
//   recline_overlay --register    add manifest + enable auto-launch, then exit
//   recline_overlay --unregister  disable auto-launch + remove manifest, then exit

#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "canvas.h"
#include "platform.h"
#include "recline_client.h"
#include "vk_texture.h"

namespace {

using namespace recline;

constexpr const char* kAppKey = "user.recline.overlay";
constexpr const char* kOverlayKey = "user.recline.overlay.dashboard";
constexpr int kW = 1024, kH = 700;

const recline_font::FontFace& kSmall = recline_font::kFace22;
const recline_font::FontFace& kBig = recline_font::kFace34;

const Color kBg{24, 26, 32, 255};
const Color kPanel{40, 44, 54, 255};
const Color kText{230, 232, 238, 255};
const Color kDim{150, 156, 170, 255};
const Color kAccent{70, 140, 255, 255};
const Color kGood{90, 200, 120, 255};
const Color kWarn{255, 190, 70, 255};
const Color kBad{255, 95, 95, 255};

// std::filesystem path from a UTF-8 string (the platform layer's encoding).
std::filesystem::path Utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

const char* AppErr(vr::EVRApplicationError e) {
  return vr::VRApplications() ? vr::VRApplications()->GetApplicationsErrorNameFromEnum(e) : "?";
}

bool InitVR(vr::EVRApplicationType type) {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, type);
  if (e != vr::VRInitError_None) {
    fprintf(stderr, "VR_Init failed: %s (is SteamVR running?)\n",
            vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return false;
  }
  return true;
}

int Register(const std::string& manifest) {
  std::error_code ec;
  if (!std::filesystem::exists(Utf8Path(manifest), ec)) {
    fprintf(stderr, "manifest not found: %s\n", manifest.c_str());
    return 1;
  }
  if (!InitVR(vr::VRApplication_Utility)) return 1;
  auto* apps = vr::VRApplications();
  if (apps->IsApplicationInstalled(kAppKey))
    printf("%s already registered; re-adding to pick up the current path\n", kAppKey);
  vr::EVRApplicationError e = apps->AddApplicationManifest(manifest.c_str(), false);
  if (e != vr::VRApplicationError_None) {
    fprintf(stderr, "AddApplicationManifest(%s): %s\n", manifest.c_str(), AppErr(e));
    vr::VR_Shutdown();
    return 1;
  }
  e = apps->SetApplicationAutoLaunch(kAppKey, true);
  if (e != vr::VRApplicationError_None) {
    fprintf(stderr, "SetApplicationAutoLaunch: %s\n", AppErr(e));
    vr::VR_Shutdown();
    return 1;
  }
  printf("registered %s (%s), auto-launch on\n", kAppKey, manifest.c_str());
  vr::VR_Shutdown();
  return 0;
}

int Unregister(const std::string& manifest) {
  if (!InitVR(vr::VRApplication_Utility)) return 1;
  auto* apps = vr::VRApplications();
  if (!apps->IsApplicationInstalled(kAppKey)) {
    printf("%s is not registered\n", kAppKey);
    vr::VR_Shutdown();
    return 0;
  }
  vr::EVRApplicationError e = apps->SetApplicationAutoLaunch(kAppKey, false);
  if (e != vr::VRApplicationError_None)
    fprintf(stderr, "SetApplicationAutoLaunch(false): %s\n", AppErr(e));
  e = apps->RemoveApplicationManifest(manifest.c_str());
  int rc = 0;
  if (e != vr::VRApplicationError_None) {
    fprintf(stderr, "RemoveApplicationManifest(%s): %s\n", manifest.c_str(), AppErr(e));
    rc = 1;
  } else {
    printf("unregistered %s\n", kAppKey);
  }
  vr::VR_Shutdown();
  return rc;
}

// ---------------------------------------------------------------------------
// Persistent UI options (not the transform: that is never persisted).

struct Options {
  RecenterOptions rc;
  bool seatedReset = true;
};

namespace fs = std::filesystem;

fs::path ConfigPath() { return Utf8Path(UserConfigDir()) / "recline-vr" / "overlay.conf"; }

void LoadOptions(Options* o) {
  std::ifstream f(ConfigPath());
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
  }
}

void SaveOptions(const Options& o) {
  const fs::path path = ConfigPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    char buf[128];
    snprintf(buf, sizeof buf, "levelOnly=%d\nkeepPosition=%d\neyeHeight=%.2f\nseatedReset=%d\n",
             o.rc.levelOnly, o.rc.keepPosition, o.rc.eyeHeight, o.seatedReset);
    f << buf;
  }
  // Explicit mode (and repair of an older file): SteamVR may launch us with
  // umask 0, which would leave the file world-writable.
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read, ec);
}

// ---------------------------------------------------------------------------

class App {
 public:
  explicit App(std::string dir) : dir_(std::move(dir)), canvas_(kW, kH) {}

  int Run() {
    if (!InitVR(vr::VRApplication_Overlay)) return 1;
    LoadOptions(&opt_);
    const char* flip = getenv("RECLINE_MOUSE_FLIP_Y");
    if (flip) flipMouseY_ = atoi(flip) != 0;

    auto* ov = vr::VROverlay();
    vr::EVROverlayError oe = ov->CreateDashboardOverlay(kOverlayKey, "Recline", &main_, &thumb_);
    if (oe != vr::VROverlayError_None) {
      fprintf(stderr, "CreateDashboardOverlay: %s\n", ov->GetOverlayErrorNameFromEnum(oe));
      vr::VR_Shutdown();
      return 1;
    }
    ov->SetOverlayWidthInMeters(main_, 2.0f);
    ov->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{{float(kW), float(kH)}};
    ov->SetOverlayMouseScale(main_, &scale);
    SetThumbnail();
    {
      std::string err;
      vkOk_ = vk_.Init(kW, kH, &err);
      renderer_ = vkOk_ ? "Vulkan texture" : "SetOverlayRaw (" + err + ")";
      fprintf(stderr, "renderer: %s\n", renderer_.c_str());
    }

    viewPitch_ = MeasureViewPitch(vr::VRSystem());
    fprintf(stderr, "lens-centre view pitch: %.2f deg\n", Deg(viewPitch_));
    SetupInput();
    OpenShmIfNeeded();
    RefreshStatus();
    Render();

    double nextStatus = 0;
    while (!quit_) {
      PumpEvents();
      PollActions();
      const double now = MonotonicSeconds();
      if (now >= nextStatus) {
        nextStatus = now + 0.25;
        OpenShmIfNeeded();
        RefreshStatus();
        dirty_ = true;
      }
      if ((dirty_ || uploadPending_) && ov->IsOverlayVisible(main_)) Render();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    vr::VR_Shutdown();
    return 0;
  }

 private:
  // --- setup -----------------------------------------------------------------

  void SetupInput() {
    auto* in = vr::VRInput();
    const std::string manifest = dir_ + "/actions.json";
    vr::EVRInputError e = in->SetActionManifestPath(manifest.c_str());
    if (e != vr::VRInputError_None) {
      fprintf(stderr, "SetActionManifestPath(%s) failed: %d\n", manifest.c_str(), int(e));
      return;
    }
    in->GetActionSetHandle("/actions/recline", &actionSet_);
    in->GetActionHandle("/actions/recline/in/recenter", &actRecenter_);
    in->GetActionHandle("/actions/recline/in/reset", &actReset_);
    in->GetActionHandle("/actions/recline/out/haptic", &actHaptic_);
    in->GetInputSourceHandle("/user/hand/left", &srcLeft_);
    in->GetInputSourceHandle("/user/hand/right", &srcRight_);
    inputOk_ = true;
  }

  void OpenShmIfNeeded() {
    if (shm_) return;
    std::string err;
    shm_ = OpenShm(/*takeOverUninitialized=*/false, &err);
    if (!shm_) shmError_ = err;
  }

  // --- events & input ------------------------------------------------------------

  void PumpEvents() {
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        quit_ = true;
      }
    }
    while (vr::VROverlay()->PollNextOverlayEvent(main_, &ev, sizeof ev)) {
      switch (ev.eventType) {
        case vr::VREvent_MouseMove:
        case vr::VREvent_MouseButtonDown: {
          const int x = int(ev.data.mouse.x);
          const int y = flipMouseY_ ? kH - 1 - int(ev.data.mouse.y) : int(ev.data.mouse.y);
          if (x != mouseX_ || y != mouseY_) dirty_ = true;
          mouseX_ = x;
          mouseY_ = y;
          if (ev.eventType == vr::VREvent_MouseButtonDown &&
              ev.data.mouse.button == vr::VRMouseButton_Left)
            OnClick(x, y);
          break;
        }
        case vr::VREvent_OverlayShown:
          dirty_ = true;
          uploaded_.clear();
          break;
        case vr::VREvent_Quit:
          quit_ = true;
          break;
        default:
          break;
      }
    }
  }

  void PollActions() {
    if (!inputOk_) return;
    vr::VRActiveActionSet_t set{};
    set.ulActionSet = actionSet_;
    set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    set.ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
    // Global overlay priority so the actions work while a game has focus.
    set.nPriority = vr::k_nActionSetOverlayGlobalPriorityMin;
    if (vr::VRInput()->UpdateActionState(&set, sizeof set, 1) != vr::VRInputError_None) return;
    if (Pressed(actRecenter_)) DoRecenter();
    if (Pressed(actReset_)) DoReset();
  }

  bool Pressed(vr::VRActionHandle_t a) {
    vr::InputDigitalActionData_t d{};
    if (vr::VRInput()->GetDigitalActionData(a, &d, sizeof d, vr::k_ulInvalidInputValueHandle) !=
        vr::VRInputError_None)
      return false;
    return d.bActive && d.bChanged && d.bState;
  }

  void Haptic() {
    if (!inputOk_) return;
    for (auto src : {srcLeft_, srcRight_})
      vr::VRInput()->TriggerHapticVibrationAction(actHaptic_, 0, 0.12f, 160.0f, 0.6f, src);
  }

  // --- actions -----------------------------------------------------------------

  void SetMessage(const std::string& m, Color c) {
    message_ = m;
    messageColor_ = c;
    dirty_ = true;
    fprintf(stderr, "%s\n", m.c_str());
  }

  void DoRecenter() {
    OpenShmIfNeeded();
    if (!shm_) return SetMessage("Shared memory unavailable: " + shmError_, kBad);
    RecenterOptions rc = opt_.rc;
    rc.viewPitch = viewPitch_;  // level what the user sees through the lenses
    Rigid traw;
    std::string err;
    if (!BuildTraw(vr::VRSystem(), rc, &traw, &err)) return SetMessage(err, kBad);
    const SendResult r = SendCommand(shm_, CMD_RECENTER, &traw);
    if (r != SendResult::Ok) return SetMessage(std::string("Recenter failed: ") + SendResultText(r), kBad);
    if (opt_.seatedReset) {
      // Let the new R reach a couple of frames before sampling the seated zero.
      std::this_thread::sleep_for(std::chrono::duration<double>(2.0 / DisplayHz()));
      vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
    }
    Haptic();
    SetMessage(opt_.seatedReset ? "Recentered (seated zero reset too)" : "Recentered", kGood);
    RefreshStatus();
  }

  void DoReset() {
    OpenShmIfNeeded();
    if (!shm_) return SetMessage("Shared memory unavailable: " + shmError_, kBad);
    const SendResult r = SendCommand(shm_, CMD_RESET, nullptr);
    if (r != SendResult::Ok) return SetMessage(std::string("Reset failed: ") + SendResultText(r), kBad);
    if (opt_.seatedReset) {
      // The seated zero was moved by the recenter; put it back in front of the
      // user's (now normal) head pose.
      std::this_thread::sleep_for(std::chrono::duration<double>(2.0 / DisplayHz()));
      vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
    }
    Haptic();
    SetMessage(opt_.seatedReset ? "Back to normal (seated zero reset too)" : "Back to normal", kGood);
    RefreshStatus();
  }

  // SteamVR's own route to an overlay app's bindings is buried; open it directly.
  void OpenBindings() {
    const vr::EVRInputError e = vr::VRInput()->OpenBindingUI(
        kAppKey, actionSet_, vr::k_ulInvalidInputValueHandle, /*bShowOnDesktop=*/false);
    if (e != vr::VRInputError_None)
      SetMessage("Could not open the binding UI (error " + std::to_string(int(e)) + ")", kBad);
    else
      SetMessage("Opened the controller binding editor", kText);
  }

  float DisplayHz() {
    vr::ETrackedPropertyError e;
    const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &e);
    return (e == vr::TrackedProp_Success && hz > 20.0f) ? hz : 90.0f;
  }

  void RefreshStatus() {
    status_ = QueryStatus(shm_, &lastPoseCount_);
    vr::EVRSettingsError se = vr::VRSettingsError_None;
    safeModeBlocked_ = vr::VRSettings()->GetBool("driver_recline", "blocked_by_safe_mode", &se) &&
                       se == vr::VRSettingsError_None;
  }

  // --- UI ----------------------------------------------------------------------------

  enum Hit {
    kHitNone, kHitRecenter, kHitReset, kHitRotFull, kHitRotLevel, kHitPosHeight, kHitPosKeep,
    kHitHeightMinus, kHitHeightPlus, kHitSeated, kHitBindings,
    kHitHeightSitting, kHitHeightStanding
  };

  // Approximate average eye heights (m) for the "Target height" presets.
  static constexpr double kSittingEyeHeight = 1.20;
  static constexpr double kStandingEyeHeight = 1.50;
  bool IsPreset(double h) const { return std::fabs(opt_.rc.eyeHeight - h) < 0.005; }

  void OnClick(int x, int y) {
    for (const auto& h : hits_) {
      if (!h.first.Contains(x, y)) continue;
      switch (h.second) {
        case kHitRecenter: DoRecenter(); return;
        case kHitBindings: OpenBindings(); return;
        case kHitReset: DoReset(); return;
        case kHitRotFull: opt_.rc.levelOnly = false; break;
        case kHitRotLevel: opt_.rc.levelOnly = true; break;
        case kHitPosHeight: opt_.rc.keepPosition = false; break;
        case kHitPosKeep: opt_.rc.keepPosition = true; break;
        case kHitHeightSitting: opt_.rc.eyeHeight = kSittingEyeHeight; opt_.rc.keepPosition = false; break;
        case kHitHeightStanding: opt_.rc.eyeHeight = kStandingEyeHeight; opt_.rc.keepPosition = false; break;
        case kHitHeightMinus: opt_.rc.eyeHeight = std::max(0.2, std::round((opt_.rc.eyeHeight - 0.05) * 100) / 100); break;
        case kHitHeightPlus: opt_.rc.eyeHeight = std::min(2.5, std::round((opt_.rc.eyeHeight + 0.05) * 100) / 100); break;
        case kHitSeated: opt_.seatedReset = !opt_.seatedReset; break;
        default: return;
      }
      SaveOptions(opt_);
      dirty_ = true;
      return;
    }
  }

  void Button(Rect r, const std::string& label, Hit id, bool selected,
              const recline_font::FontFace& f = kSmall) {
    const bool hover = r.Contains(mouseX_, mouseY_);
    canvas_.FillRect(r, selected ? kAccent : (hover ? Color{60, 66, 80, 255} : kPanel));
    if (!selected) canvas_.StrokeRect(r, hover ? kAccent : Color{70, 76, 90, 255});
    canvas_.TextCentered(r, label, f, kText);
    hits_.push_back({r, id});
  }

  void Render() {
    dirty_ = false;
    hits_.clear();
    Canvas& c = canvas_;
    c.Clear(kBg);
    c.Text(32, 22, "Recline VR", kBig, kText);

    Button({32, 80, 470, 90}, "RECLINE RECENTER", kHitRecenter, false, kBig);
    Button({522, 80, 470, 90}, "Reset to normal", kHitReset, false, kBig);

    int y = 196;
    c.Text(32, y + 12, "Rotation", kSmall, kDim);
    Button({200, y, 220, 46}, "Full", kHitRotFull, !opt_.rc.levelOnly);
    Button({430, y, 220, 46}, "Level only", kHitRotLevel, opt_.rc.levelOnly);
    y += 58;
    c.Text(32, y + 12, "Position", kSmall, kDim);
    Button({200, y, 220, 46}, "Target height", kHitPosHeight, !opt_.rc.keepPosition);
    Button({430, y, 220, 46}, "Keep position", kHitPosKeep, opt_.rc.keepPosition);
    y += 58;
    char buf[160];
    const Color heightCol = opt_.rc.keepPosition ? kDim : kText;
    c.Text(32, y + 12, "Eye height", kSmall, kDim);
    snprintf(buf, sizeof buf, "Sitting %.2f", kSittingEyeHeight);
    Button({200, y, 220, 46}, buf, kHitHeightSitting, IsPreset(kSittingEyeHeight));
    snprintf(buf, sizeof buf, "Standing %.2f", kStandingEyeHeight);
    Button({430, y, 220, 46}, buf, kHitHeightStanding, IsPreset(kStandingEyeHeight));
    snprintf(buf, sizeof buf, "%.2f m", opt_.rc.eyeHeight);
    Button({680, y, 50, 46}, "-", kHitHeightMinus, false);
    c.TextCentered({730, y, 130, 46}, buf, kSmall, heightCol);
    Button({860, y, 50, 46}, "+", kHitHeightPlus, false);
    y += 58;
    Button({32, y + 6, 34, 34}, opt_.seatedReset ? "x" : "", kHitSeated, opt_.seatedReset);
    c.Text(80, y + 12, "Also reset seated zero pose after recenter / reset", kSmall, kText);
    Button({700, y, 292, 46}, "Edit controller bindings", kHitBindings, false);

    // Status panel
    y += 54;
    c.FillRect({32, y, kW - 64, kH - y - 24}, kPanel);
    int ty = y + 12;
    auto line = [&](const std::string& s, Color col) {
      if (ty + kSmall.cellH > kH - 28) return;
      c.Text(48, ty, s, kSmall, col);
      ty += kSmall.cellH + 3;
    };
    const auto& st = status_.state;
    if (!shm_) {
      line("Driver: shared memory unavailable (" + shmError_ + ")", kBad);
    } else if (safeModeBlocked_) {
      line("Driver: blocked by SteamVR safe mode - re-enable it in Settings > Startup", kBad);
    } else if (!status_.driverAlive) {
      line("Driver: NOT LOADED (no poses) - check vrserver.txt / vrpathreg", kBad);
    } else if (st.flags & FLAG_HOOK_REFUSED) {
      line("Driver: loaded, but the pose hook was refused (see vrserver.txt)", kBad);
    } else if (st.flags & FLAG_HOOK_INSTALLED) {
      line("Driver: loaded, hook installed", kGood);
    } else {
      line("Driver: loaded, hook not installed", kWarn);
    }
    const bool active = status_.driverAlive && (st.flags & FLAG_TRANSFORM_ACTIVE);
    if (active) {
      double yaw, pitch, roll;
      ToYawPitchRoll({st.R_q[0], st.R_q[1], st.R_q[2], st.R_q[3]}, &yaw, &pitch, &roll);
      snprintf(buf, sizeof buf, "Transform ACTIVE: yaw %.0f  pitch %.0f  roll %.0f deg", Deg(yaw),
               Deg(pitch), Deg(roll));
      line(buf, kAccent);
    } else {
      line("Transform: none (normal tracking)", kDim);
    }
    if (st.flags & FLAG_WRAPPED)
      line("Another pose hook runs before recline (expected for calibrators)", kDim);
    if (st.flags & FLAG_CONFLICT_DOWNSTREAM)
      line("Warning: something modifies poses after recline", kWarn);
    if (active) {
      line("The boundary is drawn un-rotated while reclined.", kDim);
#ifdef RECLINE_FRAME
      // Streaming: trackers calibrated on the PC side never get R.
      line("PC Space Calibrator calibration is invalid while reclined.", kDim);
#endif
    }
    if (!message_.empty()) line(message_, messageColor_);

    Upload();
  }

  // Every SetOverlayRaw replaces the overlay's texture, which the compositor
  // shows as a flicker. Only upload when the pixels changed, and rate-limit;
  // a throttled change stays pending and goes out on a later loop pass.
  void Upload() {
    const double now = MonotonicSeconds();
    const size_t n = size_t(kW) * kH * 4;
    if (uploaded_.size() == n && memcmp(uploaded_.data(), canvas_.Data(), n) == 0) {
      uploadPending_ = false;
      return;
    }
    if (now - lastUpload_ < kMinUploadInterval) {
      uploadPending_ = true;
      return;
    }
    uploaded_.assign(canvas_.Data(), canvas_.Data() + n);
    lastUpload_ = now;
    uploadPending_ = false;
    if (vkOk_) {
      std::string err;
      if (vk_.Upload(main_, uploaded_.data(), &err)) return;
      vkOk_ = false;  // fall back for good
      renderer_ = "SetOverlayRaw (Vulkan upload failed: " + err + ")";
      fprintf(stderr, "renderer: %s\n", renderer_.c_str());
    }
    vr::VROverlay()->SetOverlayRaw(main_, uploaded_.data(), kW, kH, 4);
  }

  // Dashboard icon, rendered from packaging/overlay/icon.svg.
  void SetThumbnail() {
    const std::string icon = dir_ + "/icon.png";
    const vr::EVROverlayError e = vr::VROverlay()->SetOverlayFromFile(thumb_, icon.c_str());
    if (e != vr::VROverlayError_None)
      fprintf(stderr, "icon %s: %s\n", icon.c_str(), vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
  }

  std::string dir_;
  Canvas canvas_;
  Options opt_;
  vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid, thumb_ = vr::k_ulOverlayHandleInvalid;
  vr::VRActionSetHandle_t actionSet_ = vr::k_ulInvalidActionSetHandle;
  vr::VRActionHandle_t actRecenter_ = vr::k_ulInvalidActionHandle, actReset_ = vr::k_ulInvalidActionHandle,
                       actHaptic_ = vr::k_ulInvalidActionHandle;
  vr::VRInputValueHandle_t srcLeft_ = vr::k_ulInvalidInputValueHandle,
                           srcRight_ = vr::k_ulInvalidInputValueHandle;
  bool inputOk_ = false;
  ReclineShm* shm_ = nullptr;
  std::string shmError_;
  DriverStatus status_;
  uint64_t lastPoseCount_ = 0;
  bool safeModeBlocked_ = false;
  std::vector<std::pair<Rect, Hit>> hits_;
  int mouseX_ = -1, mouseY_ = -1;
  // OpenVR reports overlay mouse coordinates with the origin at the bottom
  // left; our canvas is top-down. Override with RECLINE_MOUSE_FLIP_Y=0.
  bool flipMouseY_ = true;
  bool dirty_ = true, quit_ = false;
  static constexpr double kMinUploadInterval = 0.1;  // s
  std::vector<uint8_t> uploaded_;  // last pixels sent to SteamVR
  VkOverlayTexture vk_;
  bool vkOk_ = false;
  std::string renderer_;  // logged to stderr
  double viewPitch_ = 0;  // lens-centre view axis vs head axis (rad)
  double lastUpload_ = 0;
  bool uploadPending_ = false;
  std::string message_;
  Color messageColor_ = kText;
};

void Usage() {
  printf("usage: recline_overlay [--register | --unregister]\n"
         "  (no args)      run as SteamVR dashboard overlay\n"
         "  --register     register manifest with SteamVR and enable auto-launch\n"
         "  --unregister   disable auto-launch and remove the manifest\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = ExecutableDir();
  const std::string manifest = dir + "/recline_overlay.vrmanifest";
  if (argc > 1) {
    const std::string a = argv[1];
    if (a == "--register") return Register(manifest);
    if (a == "--unregister") return Unregister(manifest);
    Usage();
    return a == "--help" || a == "-h" ? 0 : 2;
  }
  return App(dir).Run();
}
