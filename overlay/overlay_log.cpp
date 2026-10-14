#include "overlay_log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "overlay_options.h"
#include "platform.h"

namespace recline {

namespace {

// Bounded like the XRService shim's log: at startup a log over kRotateBytes
// becomes overlay.previous.log (replacing the older one), and one run writes
// at most kRunMaxBytes, so the two files stay under
// 2 * (kRotateBytes + kRunMaxBytes). Messages come from startup and from user
// actions, so a run rarely gets near the limit.
constexpr std::uintmax_t kRotateBytes = 64 * 1024;
constexpr std::size_t kRunMaxBytes = 64 * 1024;

std::mutex g_mutex;
std::ofstream g_file;
std::size_t g_written = 0;

}  // namespace

void LogOpen() {
  namespace fs = std::filesystem;
  const std::lock_guard<std::mutex> lock(g_mutex);
  const fs::path dir = Utf8Path(UserDataDir()) / "recline-vr";
  const fs::path path = dir / "overlay.log";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::uintmax_t size = fs::file_size(path, ec);
  if (!ec && size >= kRotateBytes) fs::rename(path, dir / "overlay.previous.log", ec);
  g_file.open(path, std::ios::app | std::ios::binary);
}

void Log(const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  const std::lock_guard<std::mutex> lock(g_mutex);
  fprintf(stderr, "%s\n", msg);
  if (!g_file.is_open() || g_written >= kRunMaxBytes) return;
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  char line[sizeof msg + sizeof stamp + 64];
  int n = snprintf(line, sizeof line, "%s %s\n", stamp, msg);
  g_written += std::size_t(n > 0 ? n : 0);
  if (g_written >= kRunMaxBytes)
    n = snprintf(line, sizeof line, "%s log limit reached: further messages go to stderr only\n", stamp);
  if (n <= 0) return;
  g_file.write(line, std::min<std::streamsize>(n, sizeof line - 1));
  g_file.flush();
}

}  // namespace recline
