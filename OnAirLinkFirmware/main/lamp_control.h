// lamp_control.h -- shared "start/stop" state for the REC lamp.
//
// The ESP alone decides HOW the lamp blinks (solid / slow blink / fast
// blink / off) -- see the priority logic in app_main(). Every control
// transport (WiFi UDP control task, the BLE control characteristic, and
// Ableton Link) only ever reports a start/stop edge into this shared state;
// none of them may drive the LED pattern directly.
#pragma once
#include <atomic>

extern std::atomic<bool> g_pluginRec;   // true while the plugin reports "recording"
extern std::atomic<bool> g_pluginPlay;  // true while the plugin reports "playing"

// Parses one of "REC:1" / "REC:0" / "PLAY:1" / "PLAY:0" and updates the
// shared atomics above. Used by both the WiFi control task and the BLE
// control characteristic so every transport shares identical semantics.
void lamp_control_apply(const char* msg);
