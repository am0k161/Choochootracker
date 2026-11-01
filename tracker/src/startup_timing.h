#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>

inline const auto startupEpoch = std::chrono::steady_clock::now();
inline void startupFirstTitlePresent() {
  if (std::getenv("CCT_STARTUP_TIMING"))
    std::fprintf(stderr, "[startup] first title present: %.3f ms\n",
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startupEpoch).count());
}

// Opt-in diagnostics, never used in the audio callback.
class StartupTiming {
  using Clock = std::chrono::steady_clock;
  const char* name;
  Clock::time_point started = Clock::now();
public:
  explicit StartupTiming(const char* label) : name(label) {}
  ~StartupTiming() {
    if (std::getenv("CCT_STARTUP_TIMING"))
      std::fprintf(stderr, "[startup] %s: %.3f ms\n", name,
        std::chrono::duration<double, std::milli>(Clock::now() - started).count());
  }
};
