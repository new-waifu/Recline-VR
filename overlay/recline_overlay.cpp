// recline_overlay: SteamVR dashboard overlay + global input actions that
// drive driver_recline over /recline_v1.
//
// SteamVR starts it (auto-launch, set up by `reclinectl register`); it takes no
// arguments, and logs to overlay.log (see overlay_log.h).

#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "auto_reset.h"
#include "canvas.h"
#include "overlay_log.h"
#include "overlay_options.h"
#include "platform.h"
#include "recline_client.h"
#include "recline_version.h"
#include "vk_texture.h"

namespace {

using namespace recline;

constexpr const char* kOverlayKey = "user.recline.overlay.dashboard";
constexpr const char* kToastKey = "user.recline.overlay.toast";
constexpr int kW = 1024, kH = 760;
constexpr int kToastW = 320, kToastH = 64;

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

bool InitVR(vr::EVRApplicationType type) {
  vr::EVRInitError e = vr::VRInitError_None;
  vr::VR_Init(&e, type);
  if (e != vr::VRInitError_None) {
    Log("VR_Init failed: %s (is SteamVR running?)", vr::VR_GetVRInitErrorAsEnglishDescription(e));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------

class App {
 public:
  App() : canvas_(kW, kH) {}

  int Run() {
    Log("recline_overlay %s", RECLINE_VERSION);
    if (!InitVR(vr::VRApplication_Overlay)) return 1;
    ReloadOptionsIfChanged();
    const char* flip = getenv("RECLINE_MOUSE_FLIP_Y");
    if (flip) flipMouseY_ = atoi(flip) != 0;

    auto* ov = vr::VROverlay();
    {
      std::string err;
      vkOk_ = vk_.Init(kW, kH, &err);
      renderer_ = vkOk_ ? "Vulkan texture" : "SetOverlayRaw (" + err + ")";
      Log("renderer: %s", renderer_.c_str());
    }

    viewPitch_ = MeasureViewPitch(vr::VRSystem());
    Log("lens-centre view pitch: %.2f deg", Deg(viewPitch_));
    SetupInput();
    OpenShmIfNeeded();
    RefreshStatus();
    // Fails if the key is taken, e.g. by a second instance.
    if (opt_.showInDashboard && !CreatePanel()) {
      vr::VR_Shutdown();
      return 1;
    }

    double nextStatus = 0;
    while (!quit_) {
      PumpEvents();
      PollActions();
      const double now = MonotonicSeconds();
      if (now >= nextStatus) {
        nextStatus = now + 0.25;
        ReloadOptionsIfChanged();
        OpenShmIfNeeded();
        RefreshStatus();
        dirty_ = true;
      }
      UpdatePanel(now);
      UpdateToast(now);
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
    const std::string manifest = InstallFile("resources/overlay/actions.json");
    vr::EVRInputError e = in->SetActionManifestPath(manifest.c_str());
    if (e != vr::VRInputError_None) {
      Log("SetActionManifestPath(%s) failed: %d", manifest.c_str(), int(e));
      return;
    }
    in->GetActionSetHandle("/actions/recline", &actionSet_);
    in->GetActionHandle("/actions/recline/in/recenter", &actRecenter_);
    in->GetActionHandle("/actions/recline/in/reset", &actReset_);
    in->GetActionHandle("/actions/recline/in/show_panel", &actShow_);
    in->GetActionHandle("/actions/recline/out/haptic", &actHaptic_);
    in->GetInputSourceHandle("/user/hand/left", &srcLeft_);
    in->GetInputSourceHandle("/user/hand/right", &srcRight_);
    inputOk_ = true;
  }

  // reclinectl edits overlay.conf (panel show / hide): pick up changes.
  void ReloadOptionsIfChanged() {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(OverlayOptionsPath(), ec);
    if (ec || t == optionsTime_) return;
    optionsTime_ = t;
    OverlayOptions o;
    LoadOverlayOptions(&o);
    opt_ = o;
    dirty_ = true;
  }

  // --- dashboard panel ---------------------------------------------------------

  bool HavePanel() const { return main_ != vr::k_ulOverlayHandleInvalid; }

  bool CreatePanel() {
    auto* ov = vr::VROverlay();
    const vr::EVROverlayError e = ov->CreateDashboardOverlay(kOverlayKey, "Recline", &main_, &thumb_);
    if (e != vr::VROverlayError_None) {
      Log("CreateDashboardOverlay: %s", ov->GetOverlayErrorNameFromEnum(e));
      main_ = thumb_ = vr::k_ulOverlayHandleInvalid;
      return false;
    }
    ov->SetOverlayWidthInMeters(main_, 2.0f);
    ov->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{{float(kW), float(kH)}};
    ov->SetOverlayMouseScale(main_, &scale);
    SetThumbnail();
    uploaded_.clear();
    Render();
    return true;
  }

  void DestroyPanel() {
    vr::VROverlay()->DestroyOverlay(thumb_);
    vr::VROverlay()->DestroyOverlay(main_);
    main_ = thumb_ = vr::k_ulOverlayHandleInvalid;
    uploaded_.clear();
  }

  // The dashboard entry exists while it is wanted: always when shown in the
  // dashboard, otherwise from a summon until the dashboard next closes. It is
  // never removed while on screen (the dashboard is open, or the panel was
  // detached from it), so hiding it does not pull the panel out from under the
  // pointer or out of a game.
  void UpdatePanel(double now) {
    auto* ov = vr::VROverlay();
    const bool open = ov->IsDashboardVisible();
    if (dashboardWasOpen_ && !open) summoned_ = false;
    dashboardWasOpen_ = open;
    const bool want = opt_.showInDashboard || summoned_;
    if (want && !HavePanel()) {
      if (now < nextPanelAttempt_) return;
      if (!CreatePanel()) nextPanelAttempt_ = now + 5.0;
    } else if (!want && HavePanel() && !open && !ov->IsOverlayVisible(main_)) {
      DestroyPanel();
    }
  }

  // Opens the dashboard on the panel, creating it first if it is hidden.
  void ShowPanel() {
    if (!opt_.showInDashboard) summoned_ = true;
    if (!HavePanel() && !CreatePanel()) return;
    vr::VROverlay()->ShowDashboard(kOverlayKey);
  }

  void OpenShmIfNeeded() {
    if (shm_) return;
    std::string err;
    refused_ = DriverIdentity{};
    shm_ = OpenShm(/*driver=*/false, &err, kShmName, &refused_);
  }

  // --- acting on the driver's state ---------------------------------------------
  // The driver's state reaches the overlay's actions in three places:
  //   - the automatic-reset counter: HandleAutoReset re-zeroes the seated (and
  //     standing) pose;
  //   - FLAG_TRANSFORM_ACTIVE and the head pose: OnSystemRecenter reclines or
  //     resets, and re-zeroes both poses;
  //   - hmd_index: which device's worn events PumpEvents follows, which decides
  //     when those two act.
  // Zero poses persist in SteamVR's chaperone, beyond this session. Only a
  // driver of exactly this build, loaded from this install, means by its state
  // what this overlay reads into it, so every one of those places asks
  // Trusted() first; a new one must too. Without full trust:
  //   - Cleanup (this build's pair, newer files installed): no new recline and
  //     no answering SteamVR's recenter, but undoing finishes as usual;
  //   - None (any other driver, or none): its state is shown, never acted on.
  //     The driver's own automatic reset (moved away, headset off) still
  //     undoes a recline, and Reset sends a bare CMD_RESET without zero poses.
  // A pair that doesn't match is refused even where it might work: it only
  // arises from an update or a second install, and a restart fixes both.
  // docs/DESIGN.md, "Acting on the driver's state", gives the reasons.
  bool Trusted(Trust needed) const { return int(diag_.trust) >= int(needed); }

  // --- events & input ------------------------------------------------------------

  void PumpEvents() {
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
      if (ev.eventType == vr::VREvent_Quit) {
        vr::VRSystem()->AcknowledgeQuit_Exiting();
        quit_ = true;
      }
      // Driver state: goes through Trusted(). The client HMD index otherwise.
      const uint32_t hmd =
          Trusted(Trust::Cleanup) ? status_.state.hmd_index : vr::k_unTrackedDeviceIndex_Hmd;
      if (ev.trackedDeviceIndex == hmd) {
        if (ev.eventType == vr::VREvent_TrackedDeviceUserInteractionEnded) {
          if (removedSince_ < 0) removedSince_ = MonotonicSeconds();
        } else if (ev.eventType == vr::VREvent_TrackedDeviceUserInteractionStarted) {
          removedSince_ = -1;
        }
      }
      // SteamVR's own recenter (system button or menu) flags its standing
      // reset. So does any app's standing reset, ours included (see
      // ResetZeroPoses); Recline's own seated-only resets send no standing event.
      if (ev.eventType == vr::VREvent_StandingZeroPoseReset &&
          ev.data.seatedZeroPoseReset.bResetBySystemMenu && opt_.systemRecenter)
        OnSystemRecenter();
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
    if (Pressed(actShow_)) ShowPanel();
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
    Log("%s", m.c_str());
  }

  // An error for the status line and, since a shortcut may have asked with the
  // panel out of sight, a toast; returns false for the caller to pass on.
  bool Fail(const std::string& m, const std::string& toast) {
    SetMessage(m, kBad);
    ShowToast(toast, kBad, kToastHoldLong);
    return false;
  }

  // Refused for the driver's state. The status line, drawn right above the
  // message, already says why, so the message doesn't repeat it (the panel
  // has room for 9 lines). `verb`: what the toast says couldn't be done
  // ("recline", "reset").
  bool FailUntrusted(const char* what, const char* verb) {
    return Fail(std::string(what) + " refused (see above)",
                diag_.toast.empty() ? std::string("Can't ") + verb + ": see the Recline VR panel" : diag_.toast);
  }

  // `system`: answering SteamVR's own recenter, so both zero poses are reset
  // afterwards (see ResetZeroPoses).
  bool DoRecenter(bool system = false) {
    OpenShmIfNeeded();
    if (!Trusted(Trust::Full)) return FailUntrusted("Recenter", "recline");
    CheckHeadset(MonotonicSeconds(), /*force=*/true);
    if (headset_.steamLink) return Fail("Recenter refused (see above)", kSteamLinkToast);
    RecenterOptions rc = opt_.rc;
    rc.viewPitch = viewPitch_;  // level what the user sees through the lenses
    Rigid traw;
    std::string err;
    if (!BuildTraw(vr::VRSystem(), rc, &traw, &err)) return Fail(err, "Can't recline: headset not tracking");
    const SendResult r = SendCommand(shm_, CMD_RECENTER, &traw);
    if (r != SendResult::Ok)
      return Fail(std::string("Recenter failed: ") + SendResultText(r), "Recline failed: see the Recline VR panel");
    const bool seated = ResetZeroPoses(system);
    Haptic();
    SetMessage(seated ? "Recentered (seated zero reset too)" : "Recentered", kGood);
    RefreshStatus();
    return true;
  }

  bool DoReset(bool system = false) {
    OpenShmIfNeeded();
    // Untrusted: the bare reset only (the segment's layout is ours, or it
    // would not be open), no zero poses.
    if (!shm_) return FailUntrusted("Reset", "reset");
    const SendResult r = SendCommand(shm_, CMD_RESET, nullptr);
    if (r != SendResult::Ok)
      return Fail(std::string("Reset failed: ") + SendResultText(r), "Reset failed: see the Recline VR panel");
    if (!Trusted(Trust::Cleanup)) {
      Haptic();
      SetMessage("Back to normal (zero poses left alone, see above)", kWarn);
      RefreshStatus();
      return true;
    }
    const bool seated = ResetZeroPoses(system || standingZeroedReclined_);
    standingZeroedReclined_ = false;
    Haptic();
    SetMessage(seated ? "Back to normal (seated zero reset too)" : "Back to normal", kGood);
    RefreshStatus();
    return true;
  }

  // After a recenter / reset, puts the seated zero (if the option is on) or,
  // for SteamVR's own recenter, both zero poses in front of the new head pose.
  // Returns whether anything was reset.
  bool ResetZeroPoses(bool system) {
    if (!system && !opt_.seatedReset) return false;
    // Let the new R reach a couple of frames before SteamVR samples the head.
    std::this_thread::sleep_for(std::chrono::duration<double>(2.0 / DisplayHz()));
    if (system) {
      vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseStanding);
      // SteamVR flags this reset like its own recenter: don't answer it.
      ignoreSystemRecenterUntil_ = MonotonicSeconds() + 1.0;
    }
    vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
    return true;
  }

  // SteamVR has just re-zeroed standing and seated space from the head pose
  // after R (yaw and position only). Tilted over, that yaw is meaningless (the
  // view axis is near vertical) and R is stale: recline, then let SteamVR
  // re-zero from the levelled head. Upright, undo any recline and let it
  // re-zero from the true head. Either way the view ends up facing standing
  // forward, as after SteamVR's own recenter.
  void OnSystemRecenter() {
    if (MonotonicSeconds() < ignoreSystemRecenterUntil_) return;
    // Not on anyone's head (e.g. lying on a table): it would read as tilted
    // over and recline. The proximity sensor flickers briefly while worn.
    if (!HeadsetWorn()) return;
    // Driver state: goes through Trusted(). With this option on, SteamVR's
    // recenter is a recline shortcut, so a refusal says why like one.
    if (!Trusted(Trust::Full)) {
      FailUntrusted("SteamVR recenter", "recline");
      return;
    }
    HmdSnapshot h;
    StateSnapshot st;
    if (!shm_ || !ReadHmd(shm_->hmd, &h) || !ReadState(shm_->state, &st)) return;
    // H is driver-world: gravity-aligned and before R, i.e. the true head.
    const double tilt = Deg(ViewTilt({h.q[0], h.q[1], h.q[2], h.q[3]}, viewPitch_));
    char buf[96];
    if (tilt > opt_.systemRecenterTilt) {
      snprintf(buf, sizeof buf, "SteamVR recenter: reclined (view tilted %.0f deg)", tilt);
      if (DoRecenter(/*system=*/true)) {
        standingZeroedReclined_ = true;
        SetMessage(buf, kGood);
        ShowToast("Reclined");
      }
    } else if (st.flags & FLAG_TRANSFORM_ACTIVE) {
      snprintf(buf, sizeof buf, "SteamVR recenter: back to normal (view tilted %.0f deg)", tilt);
      if (DoReset(/*system=*/true)) {
        SetMessage(buf, kGood);
        ShowToast("Back to normal");
      }
    }
  }

  // --- toast -------------------------------------------------------------------
  // What SteamVR's own recenter did depends on the head pose, so say so
  // briefly in view. SteamVR notifications appear at the edge of view in the
  // Steam friends style, so this is a small head-locked overlay of our own.

  void ShowToast(const std::string& text, Color border = kAccent, double hold = kToastHold) {
    auto* ov = vr::VROverlay();
    if (toast_ == vr::k_ulOverlayHandleInvalid) {
      const vr::EVROverlayError e = ov->CreateOverlay(kToastKey, "Recline", &toast_);
      if (e != vr::VROverlayError_None) {
        Log("toast CreateOverlay: %s", ov->GetOverlayErrorNameFromEnum(e));
        toast_ = vr::k_ulOverlayHandleInvalid;
        return;
      }
      // Above other overlays and nearer than the dashboard, so an open
      // dashboard does not hide it.
      ov->SetOverlaySortOrder(toast_, 200);
      // On the lens-centre view axis, a little below centre, facing the eye.
      Rigid pose;
      pose.q = AxisAngle({1, 0, 0}, viewPitch_ - Rad(6));
      pose.t = Rotate(pose.q, {0, 0, -1.0});
      vr::HmdMatrix34_t m;
      ToMatrix34(pose, &m);
      ov->SetOverlayTransformTrackedDeviceRelative(toast_, vr::k_unTrackedDeviceIndex_Hmd, &m);
    }
    // As wide as the text needs, at a fixed scale (kToastW pixels per 0.22 m).
    const int w = std::max(kToastW, Canvas::TextWidth(text, kBig) + 48);
    Canvas c(w, kToastH);
    c.Clear({24, 26, 32, 200});
    c.StrokeRect({0, 0, w, kToastH}, border, 2);
    c.TextCentered({0, 0, w, kToastH}, text, kBig, kText);
    ov->SetOverlayWidthInMeters(toast_, 0.22f * float(w) / kToastW);
    // Drawn once while hidden, so SetOverlayRaw's flicker on replace is moot.
    ov->SetOverlayRaw(toast_, const_cast<uint8_t*>(c.Data()), w, kToastH, 4);
    ov->SetOverlayAlpha(toast_, 1.0f);
    ov->ShowOverlay(toast_);
    toastShownAt_ = MonotonicSeconds();
    toastHold_ = hold;
  }

  void UpdateToast(double now) {
    if (toastShownAt_ == 0) return;
    const double t = now - toastShownAt_;
    if (t >= toastHold_ + kToastFade) {
      vr::VROverlay()->HideOverlay(toast_);
      toastShownAt_ = 0;
    } else if (t > toastHold_) {
      vr::VROverlay()->SetOverlayAlpha(toast_, float(1.0 - (t - toastHold_) / kToastFade));
    }
  }

  // Bindings SteamVR reports for `action`, also while the dashboard is open. A
  // chord counts as one (unnamed) binding, so the inputs cannot be named. Only
  // connected controllers' bindings are loaded, so 0 may also mean "not yet".
  uint32_t BindingCount(vr::VRActionHandle_t action) {
    vr::InputBindingInfo_t info[4];
    uint32_t count = 0;
    if (!inputOk_ ||
        vr::VRInput()->GetActionBindingInfo(action, info, sizeof info[0], 4, &count) != vr::VRInputError_None)
      return 0;
    return count;
  }

  // "Show the Recline panel" is known to be unbound: a controller is connected,
  // so its bindings are loaded, but the action has none. (Recline and reset have
  // no default binding, so their counts prove nothing.)
  bool ShowPanelUnbound() {
    if (BindingCount(actShow_) > 0) return false;
    auto* sys = vr::VRSystem();
    for (uint32_t d = 0; d < vr::k_unMaxTrackedDeviceCount; ++d)
      if (sys->GetTrackedDeviceClass(d) == vr::TrackedDeviceClass_Controller && sys->IsTrackedDeviceConnected(d))
        return true;
    return false;
  }

  // SteamVR's own route to an overlay app's bindings is buried; open it directly.
  void OpenBindings() {
    const vr::EVRInputError e = vr::VRInput()->OpenBindingUI(
        kOverlayAppKey, actionSet_, vr::k_ulInvalidInputValueHandle, /*bShowOnDesktop=*/false);
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

  bool HeadsetWorn() const {
    return removedSince_ < 0 || MonotonicSeconds() - removedSince_ < AutoResetGuard::kRemovedHold;
  }

  // The driver undid the recline because the user moved away or took the
  // headset off (AutoResetGuard). Say so, and put the zero poses back in front
  // of the user once the headset is on again.
  void HandleAutoReset() {
    const uint32_t a = status_.state.auto_reset;
    if (!status_.shmOk) return;
    if (!autoResetSeen_) {
      autoResetSeen_ = true;
      lastAutoReset_ = a;
    }
    if (a >> 8 != lastAutoReset_ >> 8) {
      lastAutoReset_ = a;
      pendingAutoReset_ = a & 0xffu;
      SetMessage(pendingAutoReset_ == AUTO_RESET_MOVED ? "Back to normal: you moved away"
                                                       : "Back to normal: the headset was taken off",
                 kWarn);
    }
    if (pendingAutoReset_ == AUTO_RESET_NONE || !HeadsetWorn()) return;
    // Driver state: goes through Trusted(). Untrusted, the reason stays on the
    // status line and the zero poses wait: in standby the driver has no poses
    // to show it alive until shortly after the headset is back on.
    if (!Trusted(Trust::Cleanup)) return;
    ResetZeroPoses(standingZeroedReclined_);
    standingZeroedReclined_ = false;
    Haptic();
    ShowToast(pendingAutoReset_ == AUTO_RESET_MOVED ? "Back to normal: you moved"
                                                    : "Back to normal: headset was off");
    pendingAutoReset_ = AUTO_RESET_NONE;
  }

  void RefreshStatus() {
    // Liveness is the pose count advancing between two samples; samples too
    // close together could miss a pose and read as a dead driver.
    const double now = MonotonicSeconds();
    const bool wasAlive = status_.driverAlive;
    uint64_t count = lastPoseCount_;
    status_ = QueryStatus(shm_, &count);
    if (now - lastLivenessSample_ >= kLivenessInterval) {
      lastPoseCount_ = count;
      lastLivenessSample_ = now;
    } else {
      status_.driverAlive = status_.driverAlive || wasAlive;
    }
    diag_ = Diagnose(shm_, refused_, status_, vr::VRSettings());
    if (diag_.line != diagLine_) {  // each change once: bounded with the log
      diagLine_ = diag_.line;
      diagSince_ = now;
      Log("status: %s", diag_.line.c_str());
    }
    HandleAutoReset();
    MaybeToastProblem(now);
    CheckHeadset(now);
  }

  // The headset can change at any time (the Frame starts or stops streaming):
  // checked every few seconds, and again before each recline.
  void CheckHeadset(double now, bool force = false) {
    if (!force && now < nextHeadsetCheck_) return;
    nextHeadsetCheck_ = now + 5.0;
    const Headset h = QueryHeadset(vr::VRSystem());
    if (h.steamLink == headset_.steamLink && h.frame == headset_.frame) return;
    headset_ = h;
    dirty_ = true;
    if (h.steamLink) Log("headset: %s over Steam Link: recline refused", h.frame ? "a Steam Frame" : "streamed");
  }

  // With the panel hidden nothing else shows a problem the user must act on,
  // so say it once per condition and run, in view: once the headset is on and
  // the condition has lasted a few seconds (the driver may still be starting).
  // The panel stays hidden, as the user chose.
  void MaybeToastProblem(double now) {
    if (opt_.showInDashboard || diag_.toast.empty() || now - diagSince_ < kProblemSettle) return;
    if (toastShownAt_ != 0 || toasted_.count(diag_.toast)) return;
    if (vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd) !=
        vr::k_EDeviceActivityLevel_UserInteraction)
      return;
    toasted_.insert(diag_.toast);
    Log("toast: %s", diag_.toast.c_str());
    ShowToast(diag_.toast, kWarn, kToastHoldLong);
  }

  // --- UI ----------------------------------------------------------------------------

  enum Hit {
    kHitNone, kHitRecenter, kHitReset, kHitRotFull, kHitRotLevel, kHitPosHeight, kHitPosKeep,
    kHitHeightMinus, kHitHeightPlus, kHitSeated, kHitBindings,
    kHitHeightSitting, kHitHeightStanding, kHitDashboard, kHitSystem, kHitTiltMinus, kHitTiltPlus
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
        case kHitSystem: opt_.systemRecenter = !opt_.systemRecenter; break;
        case kHitTiltMinus:
          opt_.systemRecenterTilt = std::max(kMinSystemRecenterTilt, opt_.systemRecenterTilt - 5);
          break;
        case kHitTiltPlus:
          opt_.systemRecenterTilt = std::min(kMaxSystemRecenterTilt, opt_.systemRecenterTilt + 5);
          break;
        case kHitDashboard:
          // Hiding needs a way back in the headset: warn once if nothing is
          // bound to bring the panel back; a second click hides anyway.
          if (opt_.showInDashboard && !hideWarned_ && ShowPanelUnbound()) {
            hideWarned_ = true;
            return SetMessage("\"Show the Recline panel\" is unbound - click again to hide anyway", kWarn);
          }
          hideWarned_ = false;
          opt_.showInDashboard = !opt_.showInDashboard;
          // Keep the panel until the dashboard closes.
          if (!opt_.showInDashboard) summoned_ = true;
          SetMessage(opt_.showInDashboard ? "Shown in the dashboard"
                                          : "Removed from the dashboard once it closes", kText);
          break;
        default: return;
      }
      SaveOverlayOptions(opt_);
      dirty_ = true;
      return;
    }
  }

  // `s` in lines no wider than `width`, broken at spaces; a word wider than a
  // line on its own (a path) is broken where it must.
  static std::vector<std::string> WrapText(const std::string& s, const recline_font::FontFace& f, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
      const size_t sp = s.find(' ', i);
      const std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
      i = sp == std::string::npos ? s.size() : sp + 1;
      const std::string joined = cur.empty() ? word : cur + " " + word;
      if (Canvas::TextWidth(joined, f) <= width) {
        cur = joined;
        continue;
      }
      if (!cur.empty()) out.push_back(cur);
      cur = word;
      while (Canvas::TextWidth(cur, f) > width) {
        size_t n = cur.size() - 1;
        while (n > 1 && Canvas::TextWidth(cur.substr(0, n), f) > width) --n;
        out.push_back(cur.substr(0, n));
        cur = cur.substr(n);
      }
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
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
    // On the title's baseline.
    c.Text(32 + Canvas::TextWidth("Recline VR", kBig) + 14, 22 + kBig.ascent - kSmall.ascent, RECLINE_VERSION,
           kSmall, kDim);
    Button({760, 24, 34, 34}, opt_.showInDashboard ? "x" : "", kHitDashboard, opt_.showInDashboard);
    c.Text(808, 30, "Show in dashboard", kSmall, kText);

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
    y += 58;
    Button({32, y + 6, 34, 34}, opt_.systemRecenter ? "x" : "", kHitSystem, opt_.systemRecenter);
    c.Text(80, y + 12, "Recline on SteamVR's recenter when tilted over", kSmall, kText);
    snprintf(buf, sizeof buf, "%.0f deg", opt_.systemRecenterTilt);
    Button({680, y, 50, 46}, "-", kHitTiltMinus, false);
    c.TextCentered({730, y, 130, 46}, buf, kSmall, opt_.systemRecenter ? kText : kDim);
    Button({860, y, 50, 46}, "+", kHitTiltPlus, false);

    // Status panel
    y += 54;
    c.FillRect({32, y, kW - 64, kH - y - 24}, kPanel);
    int ty = y + 12;
    auto line = [&](const std::string& s, Color col) {
      for (const std::string& part : WrapText(s, kSmall, kW - 96)) {
        if (ty + kSmall.cellH > kH - 28) return;
        c.Text(48, ty, part, kSmall, col);
        ty += kSmall.cellH + 3;
      }
    };
    const auto& st = status_.state;
    static const Color kLevelColor[] = {kGood, kText, kWarn, kBad};
    line(diag_.line, kLevelColor[diag_.level]);
    if (headset_.steamLink) line(SteamLinkLine(headset_), kBad);
    // The last action's result, before the informational lines: line() drops
    // whatever no longer fits.
    if (!message_.empty()) line(message_, messageColor_);
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
    if (!opt_.showInDashboard)
      line("Reopen with the \"Show the Recline panel\" shortcut or reclinectl panel show", kDim);

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
      Log("renderer: %s", renderer_.c_str());
    }
    vr::VROverlay()->SetOverlayRaw(main_, uploaded_.data(), kW, kH, 4);
  }

  // Dashboard icon, rendered from packaging/resources/icon.svg.
  void SetThumbnail() {
    const std::string icon = InstallFile("resources/icon.png");
    const vr::EVROverlayError e = vr::VROverlay()->SetOverlayFromFile(thumb_, icon.c_str());
    if (e != vr::VROverlayError_None)
      Log("icon %s: %s", icon.c_str(), vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
  }

  Canvas canvas_;
  OverlayOptions opt_;
  vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid, thumb_ = vr::k_ulOverlayHandleInvalid;
  vr::VRActionSetHandle_t actionSet_ = vr::k_ulInvalidActionSetHandle;
  vr::VRActionHandle_t actRecenter_ = vr::k_ulInvalidActionHandle, actReset_ = vr::k_ulInvalidActionHandle,
                       actShow_ = vr::k_ulInvalidActionHandle, actHaptic_ = vr::k_ulInvalidActionHandle;
  vr::VRInputValueHandle_t srcLeft_ = vr::k_ulInvalidInputValueHandle,
                           srcRight_ = vr::k_ulInvalidInputValueHandle;
  bool inputOk_ = false;
  bool hideWarned_ = false;  // unbound warning shown; the next click hides
  double ignoreSystemRecenterUntil_ = 0;
  double removedSince_ = -1;  // headset reported not worn since then; < 0 while worn
  // SteamVR's standing zero was last taken while reclined (answering its own
  // recenter), so it must be retaken once the recline is undone.
  bool standingZeroedReclined_ = false;
  bool autoResetSeen_ = false;
  uint32_t lastAutoReset_ = 0;
  uint32_t pendingAutoReset_ = AUTO_RESET_NONE;  // reason not yet shown
  vr::VROverlayHandle_t toast_ = vr::k_ulOverlayHandleInvalid;
  double toastShownAt_ = 0;  // 0: hidden
  double toastHold_ = 0;
  static constexpr double kToastHold = 1.2, kToastHoldLong = 3.0, kToastFade = 0.4;  // s
  std::set<std::string> toasted_;  // problems already shown in a toast this run
  static constexpr double kProblemSettle = 3.0;  // s
  ReclineShm* shm_ = nullptr;
  DriverIdentity refused_;  // what OpenShm found when it refused the segment
  DriverStatus status_;
  Diagnosis diag_;
  std::string diagLine_;  // last logged
  double diagSince_ = 0;
  uint64_t lastPoseCount_ = 0;
  double lastLivenessSample_ = 0;
  Headset headset_;
  double nextHeadsetCheck_ = 0;
  static constexpr double kLivenessInterval = 0.2;  // s
  std::vector<std::pair<Rect, Hit>> hits_;
  int mouseX_ = -1, mouseY_ = -1;
  // OpenVR reports overlay mouse coordinates with the origin at the bottom
  // left; our canvas is top-down. Override with RECLINE_MOUSE_FLIP_Y=0.
  bool flipMouseY_ = true;
  bool dirty_ = true, quit_ = false;
  std::filesystem::file_time_type optionsTime_{};
  bool summoned_ = false;          // panel wanted until the dashboard next closes
  bool dashboardWasOpen_ = false;
  double nextPanelAttempt_ = 0;    // CreatePanel retry backoff
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

}  // namespace

int main(int argc, char**) {
  LogOpen();
  if (argc > 1) {
    Log("recline_overlay takes no arguments; `reclinectl register` sets it up to start with SteamVR");
    return 2;
  }
  // Steam enables its pipeline-recording layer (Fossilize) for what it starts,
  // and SteamVR passes that on. The overlay builds no pipelines, so the layer
  // would only add a cache file to SteamVR's and log noise; this is its
  // manifest's disable_environment.
  SetProcessEnv("DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "1");
  return App().Run();
}
