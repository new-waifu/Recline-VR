// The overlay's log. SteamVR starts the overlay without a terminal (on Windows
// without any console), so each message goes to overlay.log in the user data
// directory (~/.local/share/recline-vr, %LOCALAPPDATA%\recline-vr) as well as
// to stderr.
#pragma once

#if defined(__GNUC__)
#define RECLINE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define RECLINE_PRINTF(f, a)
#endif

namespace recline {

// Opens the log, first moving a large one to overlay.previous.log. Messages
// before this, or if it fails, go to stderr only.
void LogOpen();

// One line, timestamped in the file.
void Log(const char* fmt, ...) RECLINE_PRINTF(1, 2);

}  // namespace recline
