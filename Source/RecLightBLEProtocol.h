#pragma once

// Shared constants for the loopback UDP protocol between the RecLight plugin
// (all wrapper types: VST3, AU, Standalone) and the RecLightBLEHelper process.
//
// CoreBluetooth must never run inside the plugin binary itself -- macOS TCC
// can crash the DAW host process if a plugin touches Bluetooth in-process.
// RecLightBLEHelper is a small always-safe standalone executable, bundled
// inside each plugin format's Contents/Resources, that does the actual
// CoreBluetooth work and exposes it to the plugin over plain loopback UDP.
//
// Commands sent plugin -> helper (each one UDP datagram):
//   "STATUS?"                          -> ask for current status
//   "CREDS:<ssidB64>:<passB64>" -> send WiFi credentials (base64, so
//                                          arbitrary bytes are transport-safe)
//   "RESET"                            -> ask the ESP to factory-reset
//   "CONTROL:<text>"                   -> forward <text> (e.g. "REC:1") as-is
//                                          to the ESP's control characteristic
//
// Replies sent helper -> plugin (only in response to "STATUS?"):
//   "STATUS:<available 0|1>|<connected 0|1>|<free text>"
static constexpr int kRecLightBleHelperPort = 47899;
