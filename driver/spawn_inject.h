// Targeted LD_PRELOAD injection of the XRService shim.
//
// The Frame's passthrough room-view mesh pose is computed and written by a
// separate process, XRService, spawned by libArcturusPerception (a driver_cv
// dependency loaded inside vrserver) through posix_spawnp / posix_spawn /
// execvp. To correct that pose for the recline transform we must load a shim
// into XRService — and into nothing else, since loading an unexpected library
// into a game would trip anticheat.
//
// InstallSpawnInjection patches libArcturusPerception's PLT/GOT entries for
// those three libc functions. The wrappers add LD_PRELOAD=<shim> to the
// environment of exactly the spawn whose program basename is "XRService", and
// pass every other spawn through completely unchanged. Steam games are not
// descendants of vrserver at all, so they are never touched; this guard
// additionally protects vrserver's own other spawns (overlays, vrmanifest
// apps). Fails safe: if a GOT entry can't be found or validated, that function
// is left unhooked and the shim simply never loads (the overlay then falls
// back to disabling the room view while reclined).
#pragma once

#include <functional>
#include <string>

namespace recline {

// `shimPath` is the absolute path to librecline_xrshim.so. `log` receives
// human-readable progress/warnings. Returns the number of spawn functions
// successfully hooked (0 means injection is unavailable; not fatal).
int InstallSpawnInjection(const std::string& shimPath,
                          const std::function<void(const std::string&)>& log);

}  // namespace recline
