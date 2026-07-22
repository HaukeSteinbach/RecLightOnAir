// config.h -- RecLight Ableton Link firmware (ESP32-C3, ESP-IDF)
#pragma once

// --- Pins (01Space ESP32-C3 0.42" OLED board) ------------------------------
#define LED_PIN        GPIO_NUM_2   // external REC lamp, active-low
#define OLED_SDA_PIN   GPIO_NUM_5
#define OLED_SCL_PIN   GPIO_NUM_6
#define BOOT_BTN_PIN   GPIO_NUM_9   // onboard BOOT button, active-low w/ pull-up

// Hold BOOT for this long (ms) to force a factory reset -- a reliable
// fallback for when the browser-based reset can't be reached (e.g. macOS's
// restrictive captive-portal assistant window blocking network requests).
#define FACTORY_RESET_HOLD_MS 5000

// --- Networking (must match the JUCE plugin) -------------------------------
#define CONTROL_PORT     4300        // fixed control port (single studio)
#define ANNOUNCE_PORT    4211        // ONAIR_IP broadcast
#define CONFIG_PORT      4212        // provisioning (CFG:1 / RESET)

#define SETUP_AP_SSID    "RecLight Setup"

// --- NVS ------------------------------------------------------------------
#define NVS_NS           "reclight"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "pass"

// --- Timing ---------------------------------------------------------------
#define ANNOUNCE_INTERVAL_MS   3000
#define STA_CONNECT_TIMEOUT_MS 15000
