#include "spawn_inject.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <spawn.h>
#include <unistd.h>

#include "slot_patch.h"

namespace recline {

namespace {

constexpr const char* kArcturusModule = "libArcturusPerception.so";
constexpr const char* kXRServiceName = "XRService";

std::string g_shimPath;

using SpawnFn = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                        const posix_spawnattr_t*, char* const[], char* const[]);
using ExecvpFn = int (*)(const char*, char* const[]);
std::atomic<SpawnFn> g_origSpawn{nullptr};
std::atomic<SpawnFn> g_origSpawnp{nullptr};
std::atomic<ExecvpFn> g_origExecvp{nullptr};

bool IsXRService(const char* path) {
  return path && PathHasBasename(path, kXRServiceName);
}

// Build a copy of `envp` with LD_PRELOAD augmented to include the shim. The
// returned vector owns the strings; `out` points into it and is null-terminated
// for passing to posix_spawn. Any pre-existing LD_PRELOAD is preserved by
// appending the shim to it.
std::vector<char*> BuildEnvp(char* const envp[], std::vector<std::string>* storage) {
  std::string preload = g_shimPath;
  std::vector<char*> out;
  for (char* const* e = envp; e && *e; ++e) {
    if (strncmp(*e, "LD_PRELOAD=", 11) == 0) {
      const char* existing = *e + 11;
      if (*existing) preload = std::string(existing) + ":" + g_shimPath;
      continue;  // drop; re-added below
    }
    storage->emplace_back(*e);
  }
  storage->emplace_back("LD_PRELOAD=" + preload);
  out.reserve(storage->size() + 1);
  for (auto& s : *storage) out.push_back(const_cast<char*>(s.c_str()));
  out.push_back(nullptr);
  return out;
}

int WrapSpawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa,
              const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
  const SpawnFn orig = g_origSpawn.load(std::memory_order_acquire);
  if (!IsXRService(path)) return orig(pid, path, fa, attr, argv, envp);
  std::vector<std::string> storage;
  std::vector<char*> e = BuildEnvp(envp ? envp : environ, &storage);
  return orig(pid, path, fa, attr, argv, e.data());
}

int WrapSpawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
               const posix_spawnattr_t* attr, char* const argv[], char* const envp[]) {
  const SpawnFn orig = g_origSpawnp.load(std::memory_order_acquire);
  if (!IsXRService(file)) return orig(pid, file, fa, attr, argv, envp);
  std::vector<std::string> storage;
  std::vector<char*> e = BuildEnvp(envp ? envp : environ, &storage);
  return orig(pid, file, fa, attr, argv, e.data());
}

// execvp replaces the (already forked) child, so we augment our own environ
// and let real execvp inherit it. Only when this child is about to become
// XRService; otherwise the environment is left untouched.
int WrapExecvp(const char* file, char* const argv[]) {
  const ExecvpFn orig = g_origExecvp.load(std::memory_order_acquire);
  if (IsXRService(file)) {
    const char* pre = getenv("LD_PRELOAD");
    std::string v = (pre && *pre) ? std::string(pre) + ":" + g_shimPath : g_shimPath;
    setenv("LD_PRELOAD", v.c_str(), 1);
  }
  return orig(file, argv);
}

bool HookOne(const char* symbol, void* wrapper, std::atomic<SpawnFn>* saveSpawn,
             std::atomic<ExecvpFn>* saveExecvp,
             const std::function<void(const std::string&)>& log) {
  // Resolve the genuine libc implementation ourselves rather than trusting the
  // GOT slot's current value: libArcturusPerception is lazily bound, so that
  // value is the PLT resolver trampoline, and calling it would resolve the
  // symbol and write the result back over our wrapper. dlsym gives the real
  // function directly; once the slot points at our wrapper, the resolver for it
  // never runs.
  void* real = dlsym(RTLD_NEXT, symbol);
  if (!real) {
    log(std::string("spawn-inject: dlsym(") + symbol + ") failed");
    return false;
  }
  void** slot;
  void* cur;
  std::string why;
  if (!FindPltGotSlot(kArcturusModule, symbol, &slot, &cur, &why)) {
    log(std::string("spawn-inject: cannot find ") + symbol + ": " + why);
    return false;
  }
  if (cur == wrapper) return true;  // already hooked (re-init)
  if (saveSpawn) saveSpawn->store(reinterpret_cast<SpawnFn>(real), std::memory_order_release);
  if (saveExecvp) saveExecvp->store(reinterpret_cast<ExecvpFn>(real), std::memory_order_release);
  std::string err;
  if (!PatchSlot(slot, cur, wrapper, &err)) {
    log(std::string("spawn-inject: patch of ") + symbol + " failed: " + err);
    return false;
  }
  log(std::string("spawn-inject: hooked ") + symbol + (err.empty() ? "" : ("; " + err)));
  return true;
}

}  // namespace

int InstallSpawnInjection(const std::string& shimPath,
                          const std::function<void(const std::string&)>& log) {
  if (access(shimPath.c_str(), R_OK) != 0) {
    log("spawn-inject: shim not found at " + shimPath + "; passthrough fix disabled");
    return 0;
  }
  g_shimPath = shimPath;
  int n = 0;
  n += HookOne("posix_spawnp", reinterpret_cast<void*>(&WrapSpawnp), &g_origSpawnp, nullptr, log);
  n += HookOne("posix_spawn", reinterpret_cast<void*>(&WrapSpawn), &g_origSpawn, nullptr, log);
  n += HookOne("execvp", reinterpret_cast<void*>(&WrapExecvp), nullptr, &g_origExecvp, log);
  return n;
}

}  // namespace recline
