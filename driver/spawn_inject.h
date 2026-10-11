// Targeted LD_PRELOAD injection of the XRService shim.
//
// The Frame's passthrough mesh and camera poses are computed and published by a
// separate process, XRService, spawned by libArcturusPerception (a driver_cv
// dependency loaded inside vrserver) through posix_spawnp / posix_spawn /
// execvp. To correct those poses for the recline transform we must load a shim
// into XRService — and into nothing else, since loading an unexpected library
// into a game could trip anticheat (docs/DESIGN.md, "Passthrough").
//
// InstallSpawnInjection patches libArcturusPerception's PLT/GOT entries for
// those three libc functions. The wrappers add LD_PRELOAD=<shim> to the
// environment of exactly the spawn whose program basename is "XRService", and
// pass every other spawn through completely unchanged. Steam games are not
// descendants of vrserver at all, so they are never touched; this guard
// additionally protects vrserver's own other spawns (overlays, vrmanifest
// apps). Fails safe: if a GOT entry can't be found or validated, that function
// is left unhooked and the shim simply never loads (passthrough is then
// misaligned while reclined; nothing else is affected).
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
