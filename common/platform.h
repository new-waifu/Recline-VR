// OS services, with one implementation per platform (platform_linux.cpp,
// platform_win.cpp). Paths are UTF-8.
#pragma once

#include <cstddef>
#include <string>

namespace recline {

// Seconds on a system-wide monotonic clock: comparable across processes
// (HmdBlock::time is), so named explicitly rather than relying on which clock
// std::chrono::steady_clock uses.
double MonotonicSeconds();

// Maps the named shared-memory segment `size` bytes long, creating it if absent.
// `*created` tells whether this call created it; a new segment is zero-filled.
// An existing segment smaller than `size` is refused. Returns nullptr with `err`
// set on failure.
void* MapSharedMemory(const char* name, size_t size, bool* created, std::string* err);
void UnmapSharedMemory(void* p, size_t size);
// Deletes the name where segments outlive their users (POSIX shm); a no-op
// where the last unmap frees it.
void RemoveSharedMemory(const char* name);

int ProcessId();

// Directory of the running executable.
std::string ExecutableDir();
// Per-user configuration directory ($XDG_CONFIG_HOME or ~/.config; %LOCALAPPDATA%).
std::string UserConfigDir();

// Path of the loaded module (.so / .dll) containing `addr`; empty if unknown.
std::string ModulePathOf(const void* addr);
// Keeps the module containing `addr` loaded for the life of the process.
bool PinModuleOf(const void* addr, std::string* err);

// Dynamic libraries, by file name.
void* OpenLibrary(const char* name);
void* LibrarySymbol(void* lib, const char* symbol);
// File name of the Vulkan loader.
extern const char* const kVulkanLoaderName;

}  // namespace recline
