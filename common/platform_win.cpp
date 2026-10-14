#include "platform.h"

#include <cstdlib>
#include <map>
#include <mutex>

#include <windows.h>

namespace recline {

namespace {

std::string ToUtf8(const wchar_t* w, int len = -1) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
  if (len < 0) s.pop_back();  // the converted terminator
  return s;
}

std::string LastErrorText(const char* what) {
  return std::string(what) + ": error " + std::to_string(GetLastError());
}

std::string DirName(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::wstring ModuleFileName(HMODULE m) {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(m, buf.data(), DWORD(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) return buf.substr(0, n);
    buf.resize(buf.size() * 2);
  }
}

// A named mapping loses its name once its last handle closes, even while views
// keep it alive, so each view's handle stays open until it is unmapped.
std::mutex g_mappingsLock;
std::map<void*, HANDLE> g_mappings;

HMODULE ModuleOf(const void* addr, DWORD extraFlags) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | extraFlags,
                          static_cast<LPCWSTR>(addr), &m))
    return nullptr;
  return m;
}

}  // namespace

double MonotonicSeconds() {
  static const double period = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / double(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * period;
}

void* MapSharedMemory(const char* name, size_t size, bool* created, std::string* err) {
  // Session-local namespace; POSIX-style names start with '/'.
  const std::string kname = std::string("Local\\") + (name[0] == '/' ? name + 1 : name);
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(size),
                                kname.c_str());
  if (!h) {
    *err = LastErrorText("CreateFileMapping");
    return nullptr;
  }
  *created = GetLastError() != ERROR_ALREADY_EXISTS;  // a new mapping is zero-filled
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    *err = LastErrorText("MapViewOfFile");
    CloseHandle(h);
    return nullptr;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.RegionSize < size) {
    *err = std::string("existing ") + kname + " has the wrong size";
    UnmapViewOfFile(p);
    CloseHandle(h);
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mappingsLock);
  g_mappings[p] = h;
  return p;
}

void UnmapSharedMemory(void* p, size_t) {
  UnmapViewOfFile(p);
  std::lock_guard<std::mutex> lock(g_mappingsLock);
  const auto it = g_mappings.find(p);
  if (it == g_mappings.end()) return;
  CloseHandle(it->second);
  g_mappings.erase(it);
}

void RemoveSharedMemory(const char*) {}

int ProcessId() { return int(GetCurrentProcessId()); }

std::string ExecutableDir() {
  const std::wstring path = ModuleFileName(nullptr);
  return path.empty() ? "." : DirName(ToUtf8(path.c_str()));
}

std::string UserConfigDir() {
  const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
  return local && *local ? ToUtf8(local) : ".";
}

std::string UserDataDir() { return UserConfigDir(); }

void SetProcessEnv(const char* name, const char* value) {
  // The process environment (GetEnvironmentVariable, as the Vulkan loader
  // reads it) and this CRT's copy (getenv) are separate.
  SetEnvironmentVariableA(name, value);
  _putenv_s(name, value);
}

std::string ModulePathOf(const void* addr) {
  HMODULE m = ModuleOf(addr, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
  return m ? ToUtf8(ModuleFileName(m).c_str()) : std::string();
}

bool PinModuleOf(const void* addr, std::string* err) {
  if (!ModuleOf(addr, GET_MODULE_HANDLE_EX_FLAG_PIN)) {
    *err = LastErrorText("GetModuleHandleEx");
    return false;
  }
  return true;
}

void* OpenLibrary(const char* name) { return LoadLibraryA(name); }

void* LibrarySymbol(void* lib, const char* symbol) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), symbol));
}

const char* const kVulkanLoaderName = "vulkan-1.dll";

}  // namespace recline
