// Text-to-speech via spd-say — see tts.hpp.
#include "tts.hpp"

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <csignal>

namespace pdfgist::tts {
namespace {

GPid g_child = 0;
double g_rate = 1.0;

// 0.5x-3.0x maps onto speech-dispatcher's -100..100 rate range (-r flag).
int rate_flag() {
  return std::clamp(static_cast<int>(std::llround((g_rate - 1.0) * 50.0)),
                    -100, 100);
}

// Child-watch runs on the main loop: process exit (natural end or killed)
// is the end-of-speech signal.
void on_child_exited(GPid pid, gint, gpointer) {
  g_spawn_close_pid(pid);
  if (g_child == pid) g_child = 0;
}

}  // namespace

bool speaking() { return g_child != 0; }

void set_rate(double rate) { g_rate = std::clamp(rate, 0.5, 3.0); }

bool toggle(std::string const& text) {
  if (g_child != 0) {
    stop();
    return true;
  }
  if (text.empty()) return false;
  // -w: wait until playback finished (the child watch turns that into the
  // end-of-speech callback). zh-CN voice for mac parity (AVSpeechUtterance
  // voice "zh-CN"); speech-dispatcher falls back to its default voice when
  // no Chinese one is installed.
  auto rate_text = std::to_string(rate_flag());
  gchar* argv_vector[] = {const_cast<gchar*>("spd-say"),
                          const_cast<gchar*>("-w"),
                          const_cast<gchar*>("-r"),
                          const_cast<gchar*>(rate_text.c_str()),
                          const_cast<gchar*>("-l"),
                          const_cast<gchar*>("zh-CN"),
                          const_cast<gchar*>(text.c_str()),
                          nullptr};
  GError* error = nullptr;
  GPid pid = 0;
  if (!g_spawn_async_with_pipes(
          nullptr, argv_vector, nullptr,
          static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                                   G_SPAWN_DO_NOT_REAP_CHILD |
                                   G_SPAWN_STDOUT_TO_DEV_NULL |
                                   G_SPAWN_STDERR_TO_DEV_NULL),
          nullptr, nullptr, &pid, nullptr, nullptr, nullptr, &error)) {
    if (error != nullptr) g_error_free(error);
    return false;
  }
  g_child = pid;
  g_child_watch_add(pid, on_child_exited, nullptr);
  return true;
}

void stop() {
  if (g_child == 0) return;
  kill(g_child, SIGTERM);
  // The child watch reaps the process and clears g_child.
}

}  // namespace pdfgist::tts
