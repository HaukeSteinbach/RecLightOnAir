#include "lamp_control.h"
#include <cstring>

std::atomic<bool> g_pluginRec{false};
std::atomic<bool> g_pluginPlay{false};

void lamp_control_apply(const char* msg) {
  if (strncmp(msg, "REC:1", 5) == 0) {
    g_pluginRec = true;
    g_pluginPlay = false;
  } else if (strncmp(msg, "PLAY:1", 6) == 0) {
    g_pluginPlay = true;
    g_pluginRec = false;
  } else if (strncmp(msg, "REC:0", 5) == 0 || strncmp(msg, "PLAY:0", 6) == 0) {
    g_pluginRec = false;
    g_pluginPlay = false;
  }
}
