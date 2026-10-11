// Targeted LD_PRELOAD injection of the XRService shim (docs/DESIGN.md,
// "Passthrough"). Patches libArcturusPerception's PLT/GOT entries for
// posix_spawnp / posix_spawn / execvp; the wrappers add LD_PRELOAD=<shim> only
// to a spawn whose program basename is "XRService" and pass every other spawn
// through unchanged. A GOT entry that can't be found or validated is left
// unhooked, and the shim then simply never loads.
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
