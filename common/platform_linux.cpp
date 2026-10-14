#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recline {

double MonotonicSeconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

void* MapSharedMemory(const char* name, size_t size, bool* created, std::string* err) {
  *created = false;
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    *created = true;
    if (ftruncate(fd, off_t(size)) != 0) {  // zero-fills
      *err = std::string("ftruncate: ") + strerror(errno);
      close(fd);
      shm_unlink(name);
      return nullptr;
    }
  } else if (errno == EEXIST) {
    fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
      *err = std::string("shm_open existing: ") + strerror(errno);
      return nullptr;
    }
    struct stat st {};
    // A concurrent creator may not have run ftruncate yet: wait briefly.
    for (int i = 0; i < 100; ++i) {
      if (fstat(fd, &st) == 0 && size_t(st.st_size) >= size) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (size_t(st.st_size) < size) {
      *err = std::string("existing /dev/shm") + name + " has the wrong size";
      close(fd);
      return nullptr;
    }
  } else {
    *err = std::string("shm_open: ") + strerror(errno);
    return nullptr;
  }

  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    *err = std::string("mmap: ") + strerror(errno);
    return nullptr;
  }
  return p;
}

void UnmapSharedMemory(void* p, size_t size) { munmap(p, size); }

void RemoveSharedMemory(const char* name) { shm_unlink(name); }

int ProcessId() { return int(getpid()); }

std::string ExecutableDir() {
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string s(buf);
  const size_t slash = s.rfind('/');
  return slash == std::string::npos ? "." : s.substr(0, slash);
}

std::string UserConfigDir() {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return xdg;
  const char* home = getenv("HOME");
  return std::string(home ? home : ".") + "/.config";
}

std::string UserDataDir() {
  const char* xdg = getenv("XDG_DATA_HOME");
  if (xdg && *xdg) return xdg;
  const char* home = getenv("HOME");
  return std::string(home ? home : ".") + "/.local/share";
}

void SetProcessEnv(const char* name, const char* value) { setenv(name, value, 1); }

std::string ModulePathOf(const void* addr) {
  Dl_info info{};
  if (!dladdr(addr, &info) || !info.dli_fname) return {};
  return info.dli_fname;
}

bool PinModuleOf(const void* addr, std::string* err) {
  const std::string path = ModulePathOf(addr);
  if (path.empty()) {
    *err = "dladdr failed";
    return false;
  }
  if (!dlopen(path.c_str(), RTLD_NOW | RTLD_NODELETE)) {
    *err = dlerror();
    return false;
  }
  return true;
}

void* OpenLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }

void* LibrarySymbol(void* lib, const char* symbol) { return dlsym(lib, symbol); }

const char* const kVulkanLoaderName = "libvulkan.so.1";

}  // namespace recline
