// main.cpp -- RecLight firmware for the ESP32-C3 (ESP-IDF).
//
// The device always does both, at the same time, over the one WiFi STA
// connection set up once in the web setup portal (and persisted in NVS):
//   - Plugin (VST3/AU/Standalone) control over UDP.
//   - Ableton Link, so the lamp also follows Live's transport directly.
//
// Whatever sends the start/stop edge (plugin over WiFi, or Ableton Link)
// only ever reports "playing"/"recording" state -- see lamp_control.h. The
// ESP alone decides how the lamp actually blinks.
//
// NOTE: In Ableton Live, "Start Stop Sync" must be enabled (a separate toggle
// next to "Link"); otherwise Link's isPlaying() is always false.

#include <atomic>
#include <cstdio>
#include <cstring>

#include "config.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <ableton/Link.hpp>

#include "lamp_control.h"
#include "oled.h"

static const char* TAG = "reclight";

// The ESP32-C3 is rv32imc (no atomic extension); IDF emulates atomics with
// spinlocks but doesn't provide __atomic_is_lock_free. Link asserts
// mState.is_lock_free() on a std::atomic<uint32_t>; satisfy it (<= int == ok).
extern "C" bool __atomic_is_lock_free(std::size_t size, const volatile void*) {
  return size <= sizeof(int);
}

// --- Shared state ----------------------------------------------------------
// g_pluginRec / g_pluginPlay live in lamp_control.cpp (shared by WiFi + BLE).
static std::atomic<bool> g_linkPlaying{false};  // from Ableton Link
static std::atomic<bool> g_wifiConnected{false};
static std::atomic<bool> g_linkReenable{false}; // signal Link re-init after WiFi connect
// Written by lamp_task, read by app_main only for the HUD log/OLED (so their
// idea of the lamp mode matches what's actually being displayed on the LED).
static std::atomic<int64_t> g_lastActiveUs{-1LL};
static char g_ssid[33] = {0};
static char g_pass[65] = {0};
static bool g_configured = false;                // true once an SSID has ever been saved

static ableton::Link* g_link = nullptr;

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;

// --- helpers ---------------------------------------------------------------
static void set_lamp(bool on) {
  // active-low: LOW = lamp on
  gpio_set_level(LED_PIN, on ? 0 : 1);
}

// ---------------------------------------------------------------------------
// Dedicated lamp task.
//
// This used to live inline in app_main()'s own loop, which runs at the
// default main-task priority (1) -- the LOWEST priority of any task in this
// firmware other than idle. Ableton Link's background asio task runs at
// priority 2 (see lib/link's esp32 Context.hpp), and the WiFi control/
// provisioning tasks run at 4-5. As long as Link had no peers (peers=0) it
// stayed mostly idle and this didn't matter, but once Link actually
// discovers a peer and starts exchanging timeline/measurement messages, it
// generates a lot more scheduling activity on this single-core chip -- which
// could delay app_main's loop just enough to make the 5 Hz post-stop fast
// blink look uneven (a toggle occasionally missed or stretched).
//
// Isolating the blink decision + gpio_set_level() call in its own task at a
// priority above Link's (and matching the other app tasks) keeps the LED
// timing smooth regardless of what Link/WiFi/HTTP are doing.
static void lamp_task(void*) {
  bool lastOn = false;
  bool blinkSearch = false;
  int64_t lastBlink = 0;
  int64_t last_active_us = -1LL;

  for (;;) {
    const int64_t now = esp_timer_get_time();

    // Priority: PLAY/Link (solid) > REC (slow 1 Hz blink) > just-stopped (fast 5 Hz blink, 10 s) > not-connected (search blink)
    const bool lampSolid     = g_pluginPlay.load() || g_linkPlaying.load();
    const bool lampSlowBlink = !lampSolid && g_pluginRec.load();

    // Track when an active lamp state last ended to drive the 10-second post-stop fast blink.
    const bool anyActive = lampSolid || lampSlowBlink;
    if (anyActive) last_active_us = now;
    g_lastActiveUs = last_active_us;
    const bool lampFastBlink = !anyActive && (last_active_us >= 0LL) &&
                                (now - last_active_us < 10000000LL);

    // "Not connected yet": waiting for the STA WiFi join.
    const bool notConnected = !g_wifiConnected.load();

    bool wantLamp;
    if (lampSolid) {
      wantLamp = true;
    } else if (lampSlowBlink) {
      wantLamp = ((now / 500000LL) % 2LL) == 0;   // 1 Hz: 500 ms on / 500 ms off
    } else if (lampFastBlink) {
      wantLamp = ((now / 100000LL) % 2LL) == 0;   // 5 Hz: 100 ms on / 100 ms off
    } else if (notConnected) {
      if (now - lastBlink > 120000LL) {
        blinkSearch = ((now / 1000LL) % 2000LL) < 100LL;  // 100 ms on every 2 s
        lastBlink = now;
      }
      wantLamp = blinkSearch;
    } else {
      wantLamp = false;
    }

    if (wantLamp != lastOn) {
      set_lamp(wantLamp);
      lastOn = wantLamp;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- NVS credential storage ------------------------------------------------
// Returns true if the device has a saved SSID and is ready to connect right
// away. Returns false only on a brand-new/reset device that has never been
// through the web setup portal -- that's the only case that should show the
// setup guide instead of operating normally.
static bool creds_load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sl = sizeof(g_ssid), pl = sizeof(g_pass);
  esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, g_ssid, &sl);
  esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, g_pass, &pl);
  nvs_close(h);
  if (e2 != ESP_OK) g_pass[0] = '\0';  // open network is valid

  return e1 == ESP_OK && g_ssid[0] != '\0';
}

static void creds_save(const char* ssid, const char* pass) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_KEY_SSID, ssid ? ssid : "");
  nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
  nvs_commit(h);
  nvs_close(h);
}

static void creds_clear() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

// Extract the value after a "KEY:" prefix within one line of `msg`.
static bool field_value(const char* msg, const char* key, char* out, size_t cap) {
  const char* p = strstr(msg, key);
  if (!p) return false;
  p += strlen(key);
  size_t i = 0;
  while (*p && *p != '\n' && *p != '\r' && i + 1 < cap) out[i++] = *p++;
  out[i] = '\0';
  // trim trailing spaces
  while (i > 0 && out[i - 1] == ' ') out[--i] = '\0';
  return true;
}

// --- WiFi ------------------------------------------------------------------
static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifiConnected = false;
    if (g_ssid[0] != '\0')   // only if credentials are present
      esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
    g_wifiConnected = true;
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    g_linkReenable = true;  // trigger Link enable/re-enable in main loop
  }
}

// Start WiFi in AP+STA: the SoftAP "RecLight Setup" stays available so the
// plugin can (re)provision at any time; STA joins the configured network.
static void wifi_start() {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

  // SoftAP (open) for setup -- stays available in every mode so the device
  // can always be (re)configured from the web portal at 192.168.4.1.
  wifi_config_t ap = {};
  std::snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", SETUP_AP_SSID);
  ap.ap.ssid_len = strlen(SETUP_AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_OPEN;

  const bool haveCreds = creds_load();
  g_configured = haveCreds;
  const bool wantsStaConnect = haveCreds;

  // No usable STA config -> clear the driver's cached SSID so it doesn't try
  // to auto-connect with stale data. Must happen before set_mode/set_config.
  if (!wantsStaConnect) esp_wifi_restore();

  wifi_config_t sta = {};
  if (wantsStaConnect) {
    strlcpy(reinterpret_cast<char*>(sta.sta.ssid), g_ssid, sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta.sta.password), g_pass, sizeof(sta.sta.password));
    // Many modern APs (incl. iPhone Personal Hotspot without "Maximize
    // Compatibility", and WPA2/WPA3-transition home routers) require the
    // station to be PMF-capable, or they silently drop the client right
    // after association completes (assoc -> run -> init loop, looks just
    // like a wrong password but isn't). Advertise PMF support without
    // requiring it, so both plain-WPA2 and PMF-required APs work.
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  if (wantsStaConnect)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Disable modem-sleep power save: the default WIFI_PS_MIN_MODEM lets the
  // radio doze between DTIM beacons, which is a classic cause of dropped/
  // delayed multicast (Ableton Link's peer discovery is UDP multicast) even
  // on a perfectly good WiFi link. Costs some power, but this device is
  // USB-powered.
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (wantsStaConnect) {
    ESP_LOGI(TAG, "connecting to \"%s\"", g_ssid);
    esp_wifi_connect();
  } else {
    ESP_LOGW(TAG, "not set up yet -- join \"%s\" and open http://192.168.4.1", SETUP_AP_SSID);
  }
}

// --- Provisioning listener (UDP port 4212) ---------------------------------
static void provisioning_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(CONFIG_PORT);
  bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

  char buf[256];
  while (true) {
    sockaddr_in src = {};
    socklen_t sl = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) continue;
    buf[n] = '\0';

    if (strncmp(buf, "RESET", 5) == 0) {
      const char* reply = "CFG:RESET";
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      ESP_LOGW(TAG, "RESET received -- clearing creds, restarting");
      creds_clear();
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_restart();
    }

    // PING → sofort mit aktueller IP antworten (Plugin-Discovery ohne Broadcast)
    if (strncmp(buf, "PING", 4) == 0) {
      char ip_str[16] = "192.168.4.1";
      if (g_wifiConnected.load()) {
        esp_netif_t* sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (sta_if && esp_netif_get_ip_info(sta_if, &info) == ESP_OK && info.ip.addr != 0)
          std::snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&info.ip));
      }
      char reply[64];
      std::snprintf(reply, sizeof(reply), "ONAIR_IP:%s", ip_str);
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    if (strncmp(buf, "CFG:1", 5) != 0) continue;

    char ssid[33] = {0}, pass[65] = {0};
    field_value(buf, "SSID:", ssid, sizeof(ssid));
    field_value(buf, "PASS:", pass, sizeof(pass));

    if (ssid[0] == '\0') {
      const char* reply = "CFG:ERR SSID";
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    ESP_LOGI(TAG, "provisioning: ssid=\"%s\"", ssid);
    creds_save(ssid, pass);
    const char* reply = "CFG:OK SAVED";
    sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();  // reboot and join the new network
  }
}

// --- Control listener (UDP control port) ------------------------------------
static void control_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(CONTROL_PORT);
  bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ESP_LOGI(TAG, "control UDP on port %d", CONTROL_PORT);

  char buf[64];
  while (true) {
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    if (n <= 0) continue;
    buf[n] = '\0';
    lamp_control_apply(buf);
  }
}

// --- IP announce (UDP broadcast to port 4211) ------------------------------
static int ap_client_count();  // forward declaration
static void announce_task(void*) {
  // Strategy:
  //  a) STA connected: announce the STA IP on BOTH interfaces.
  //     - AP broadcast (192.168.4.255): Mac on the setup WiFi learns the STA IP.
  //     - STA subnet broadcast: Mac on the home network learns the STA IP.
  //  b) No STA: AP broadcast only, with 192.168.4.1 (so provisioning stays reachable).
  // Always announce the STA IP (never the AP IP 192.168.4.1) once STA exists -- avoids a stale target.
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  { int on = 1; setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)); }

  while (true) {
    if (g_wifiConnected.load()) {
      esp_netif_t* sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      esp_netif_ip_info_t info = {};
      if (sta_if && esp_netif_get_ip_info(sta_if, &info) == ESP_OK && info.ip.addr != 0) {
        char msg[64];
        int len = std::snprintf(msg, sizeof(msg), "ONAIR_IP:" IPSTR, IP2STR(&info.ip));

        // (1) STA-IP an AP-Clients: Mac die noch im Setup-WLAN sind erfahren STA-IP
        {
          sockaddr_in dst = {};
          dst.sin_family = AF_INET;
          inet_aton("192.168.4.255", &dst.sin_addr);
          dst.sin_port = htons(ANNOUNCE_PORT);
          sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }

        // (2) STA IP to home-network clients via subnet broadcast
        {
          uint32_t bcast = info.ip.addr | ~info.netmask.addr;
          sockaddr_in dst = {};
          dst.sin_family      = AF_INET;
          dst.sin_addr.s_addr = bcast;
          dst.sin_port        = htons(ANNOUNCE_PORT);
          sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
      }
    } else {
      // No home network yet: announce the AP IP so the plugin can reply while on the setup WiFi.
      char msg[64];
      int len = std::snprintf(msg, sizeof(msg), "ONAIR_IP:192.168.4.1");
      sockaddr_in dst = {};
      dst.sin_family = AF_INET;
      inet_aton("192.168.4.255", &dst.sin_addr);
      dst.sin_port = htons(ANNOUNCE_PORT);
      sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

    vTaskDelay(pdMS_TO_TICKS(ANNOUNCE_INTERVAL_MS));
  }
}

// --- GPIO ------------------------------------------------------------------
static void gpio_setup() {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << LED_PIN;
  io.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io);
  set_lamp(false);

  // Onboard BOOT button, used as a reliable hardware factory-reset fallback
  // (see FACTORY_RESET_HOLD_MS below) -- doesn't depend on the browser or
  // WiFi at all, so it always works even if the captive-portal reset page
  // can't be reached.
  gpio_config_t btn = {};
  btn.pin_bit_mask = 1ULL << BOOT_BTN_PIN;
  btn.mode = GPIO_MODE_INPUT;
  btn.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&btn);
}

// Number of clients joined to the SoftAP (used to advance the setup guide).
static int ap_client_count() {
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  return list.num;
}

// Redraw the OLED only when the shown content changes (avoids flicker/I2C load).
static void update_display(bool lampOn, unsigned peers) {
  static char last[80] = {0};
  char sig[80];

  if (lampOn) {
    std::snprintf(sig, sizeof(sig), "REC");
    if (strcmp(sig, last) != 0) { strcpy(last, sig); oled_show_big("REC"); }
    return;
  }

  if (!g_wifiConnected.load()) {
    if (ap_client_count() > 0) {
      std::snprintf(sig, sizeof(sig), "S2");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_lines("Step 2", "Open browser:", "192.168.4.1", "set WiFi");
      }
    } else {
      std::snprintf(sig, sizeof(sig), "S1");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_lines("Step 1", "Join WiFi:", "RecLight", "Setup");
      }
    }
    return;
  }

  // WiFi connected.
  const int64_t now = esp_timer_get_time();

  // Step 3: show an 8-second reminder on first WiFi connect (open the DAW / load the plugin).
  static bool step3_shown = false;
  static int64_t step3_start = 0;
  if (!step3_shown) {
    if (step3_start == 0) step3_start = now;
    if (now - step3_start < 8000000LL) {
      std::snprintf(sig, sizeof(sig), "S3");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_lines("Step 3", "Open DAW &", "load plugin", "");
      }
      return;
    }
    step3_shown = true;
  }

  // Connected & idle: READY screen. Auto-blank after 60 s (OLED burn-in protection).
  static bool idle_initialized = false;
  static unsigned shown_peers = 0;
  static int64_t idle_shown_at = 0;
  static bool idle_blanked = false;

  char peerLine[24];
  std::snprintf(peerLine, sizeof(peerLine), "Link: %u", peers);

  const bool meaningful = !idle_initialized || peers != shown_peers;

  if (meaningful) {
    oled_show_lines("READY", peerLine, "", "");
    idle_initialized = true;
    shown_peers = peers;
    idle_shown_at = now;
    idle_blanked = false;
    strcpy(last, "IDLE");
  } else if (strcmp(last, "IDLE") != 0) {
    // Returned from another screen (e.g. REC) with no peer change: stay blank.
    oled_clear();
    oled_flush();
    idle_blanked = true;
    strcpy(last, "IDLE");
  } else if (!idle_blanked && now - idle_shown_at > 60000000LL) {
    oled_clear();
    oled_flush();
    idle_blanked = true;
  }
}

// --- Captive portal ---------------------------------------------------------
// HTTP server (port 80) + DNS forwarder (port 53) on the SoftAP interface.
// Any client that joins "RecLight Setup" automatically gets the setup page
// shown in-browser (iOS/macOS captive portal detection). The page is a full
// step-by-step guide with a mode picker and WiFi form.

// --- HTML building blocks (kept in flash) -----------------------------

// Head + CSS + step 1 (always checked off, since you're on this page at all).
static const char P_HEAD[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight Setup</title>"
  "<style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
  "background:#0d0f11;color:#ddd;min-height:100vh;"
  "display:flex;align-items:center;justify-content:center;padding:20px}"
  ".c{background:#16181a;border:1px solid #2c3342;border-radius:16px;"
  "padding:28px 24px;max-width:400px;width:100%}"
  ".top{display:flex;justify-content:space-between;align-items:baseline;"
  "margin-bottom:20px}"
  "h1{font-size:17px;font-weight:700;color:#fff}"
  ".sub{font-size:12px;color:#555;margin-top:3px}"
  "a{color:#40a8ff;text-decoration:none}"
  ".os{font-size:11px}"
  ".row{display:flex;align-items:flex-start;gap:14px;margin-bottom:20px}"
  ".ico{width:32px;height:32px;border-radius:50%;display:flex;"
  "align-items:center;justify-content:center;font-size:13px;"
  "font-weight:700;flex-shrink:0}"
  ".dn{background:#1b4332;color:#52b788}"
  ".ac{background:#1a3a5c;color:#40a8ff}"
  ".wt{background:#1a1d22;color:#444;border:1px solid #2c3342}"
  ".t{font-size:14px;font-weight:600;color:#ddd;margin-bottom:4px}"
  ".d{font-size:12px;color:#666;line-height:1.5}"
  "label{display:block;font-size:11px;color:#666;margin-bottom:4px;"
  "margin-top:12px}"
  "input{width:100%;background:#1a1d22;border:1px solid #2c3342;"
  "border-radius:7px;padding:9px 11px;color:#ddd;font-size:14px;outline:none}"
  "input:focus{border-color:#40a8ff}"
  "input[type=radio]{width:auto;display:inline-block;margin:0 8px 0 0;"
  "vertical-align:middle}"
  ".modeRow{display:block;font-size:13px;color:#ddd;padding:10px 12px;"
  "background:#1a1d22;border:1px solid #2c3342;border-radius:7px;"
  "margin-top:8px;cursor:pointer}"
  ".modeRow b{color:#52b788}"
  ".modeRow .mdesc{display:block;font-size:11px;color:#666;margin:2px 0 0 22px}"
  "button{width:100%;background:#40a8ff;color:#000;border:none;"
  "border-radius:7px;padding:10px;font-size:14px;font-weight:600;"
  "cursor:pointer;margin-top:14px}"
  ".note{background:#13151a;border:1px solid #2c3342;border-radius:7px;"
  "padding:9px 11px;font-size:11px;color:#666;margin-top:10px;line-height:1.5}"
  ".rst{text-align:center;margin-top:20px;font-size:11px}"
  ".rst a{color:#444}"
  ".rst button{width:auto;background:none;border:none;color:#444;"
  "font-size:11px;padding:0;margin:0;cursor:pointer;text-decoration:underline;"
  "font-family:inherit}"
  "</style></head><body><div class=c>"
  // Title row with an "Open in Safari" link (helps with captive-portal quirks).
  "<div class=top>"
  "<div><h1>&#9679;&nbsp;Steinbach RecLight</h1>"
  "<p class=sub>Setup &mdash; 3 easy steps</p></div>"
  "<a class=os href='http://192.168.4.1/'>Open in Safari &#8599;</a>"
  "</div>"
  // Step 1: always done.
  "<div class=row><div class='ico dn'>&#10003;</div>"
  "<div><p class=t>1&nbsp;&middot;&nbsp;Connected to the controller</p>"
  "<p class=d>You're connected to the RecLight setup network.</p>"
  "</div></div>";

// Step 2: WiFi form (not configured yet). No mode picker -- the plugin's
// UDP control and Ableton Link both always run together over WiFi.
static const char P_FORM[] =
  "<div class=row><div class='ico ac'>2</div>"
  "<div style=flex:1>"
  "<p class=t>2&nbsp;&middot;&nbsp;Connect to your WiFi</p>"
  "<form method=get action='http://192.168.4.1/configure' id=f>"
  "<label>WiFi network name (SSID)</label>"
  "<input type=text name=ssid placeholder='Your WiFi network' autocomplete=off required>"
  "<label>Password</label>"
  "<input type=password name=pass placeholder='WiFi password'"
  " autocomplete=new-password>"
  "<button type=submit>Connect &#8594;</button>"
  "</form>"
  "<p class=note>&#8505;&nbsp;The RecLight plugin connects automatically over"
  " WiFi, and Ableton Live's transport (via Link) drives the lamp too"
  " &mdash; both work at the same time, no need to choose.</p>"
  "</div></div>";

// Step 3: grayed out (setup not finished yet).
static const char P_S3_WAIT[] =
  "<div class=row><div class='ico wt'>3</div>"
  "<div><p class=t>3&nbsp;&middot;&nbsp;Open the plugin</p>"
  "<p class=d>Finish step 2 above first, then come back here.</p>"
  "</div></div>"
  "</div></body></html>";

// Step 2: done (mode + network configured).
static const char P_S2_DONE_HEAD[] =
  "<div class=row><div class='ico dn'>&#10003;</div>"
  "<div><p class=t>2&nbsp;&middot;&nbsp;Connection configured</p>";
static const char P_S2_DONE_CONNECTED[] =
  "<p class=d style=color:#52b788>Connected &#10003;</p>";
static const char P_S2_DONE_CONNECTING[] =
  "<p class=d>Connecting to your WiFi network&hellip;</p>";
static const char P_S2_DONE_TAIL[] =
  "</div></div>"
  // Plain <a>/<form> navigation gets intercepted (and blocked) by macOS's
  // restrictive captive-portal browser once a network looks resolved, so
  // "Start over" fires a background fetch() instead of a real page nav.
  "<script>"
  "function rlReset(){"
  "fetch('http://192.168.4.1/reset').catch(function(){});"
  "var e=document.getElementById('rst');"
  "if(e)e.innerHTML='<p class=d>Resetting&hellip; reconnect to <b>RecLight"
  " Setup</b> and set it up again.</p>';"
  "}"
  "</script>";

// Step 3: active -- covers both the plugin (WiFi) and Ableton Link, since
// both always run together now.
static const char P_S3_OK[] =
  "<div class=row><div class='ico ac'>3</div>"
  "<div><p class=t>3&nbsp;&middot;&nbsp;Open the plugin &amp; turn on Link</p>"
  "<p class=d>Open your DAW (Ableton, Logic, Reaper&nbsp;&hellip;) and load"
  " the RecLight plugin &mdash; it connects automatically over WiFi.<br><br>"
  "In Ableton Live, also turn on <b>Link</b> and <b>Start Stop Sync</b>"
  " (the toggle next to Link) &mdash; the lamp follows the transport"
  " directly too. Both work at the same time.<br><br>"
  "Plugin not installed yet? Switch to a network with internet access"
  " &rarr; <a href='https://haukesteinbach.de'>haukesteinbach.de</a>"
  " &rarr; then come back here.</p>"
  "</div></div>"
  "<div class=rst id=rst>"
  "<button onclick=rlReset()>Start over&nbsp;&#8635;</button>"
  "<br>Doesn't work? Hold the device's BOOT button for 5s."
  "</div>"
  "</div></body></html>";

// Confirmation page shown right after saving (head/tail are shared; the
// middle sentence is mode-specific, picked at request time).
static const char PAGE_OK_HEAD[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight</title>"
  "<style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:-apple-system,sans-serif;background:#0d0f11;color:#ddd;"
  "min-height:100vh;display:flex;align-items:center;"
  "justify-content:center;padding:20px}"
  ".c{background:#16181a;border:1px solid #2c3342;border-radius:16px;"
  "padding:32px 24px;max-width:400px;width:100%;text-align:center}"
  ".ok{font-size:52px;color:#52b788;margin-bottom:14px}"
  "h2{font-size:18px;font-weight:700;color:#52b788;margin-bottom:10px}"
  "p{font-size:13px;color:#777;line-height:1.7}"
  "</style></head><body><div class=c>"
  "<div class=ok>&#10003;</div>"
  "<h2>Saved!</h2>"
  "<p>Your RecLight controller is restarting now (about 5 seconds).<br><br>";

static const char PAGE_OK_MID[] =
  "Next: open your DAW and load the RecLight plugin &mdash; it connects"
  " automatically over WiFi. In Ableton Live, also turn on Link and Start"
  " Stop Sync &mdash; both work at the same time.&nbsp;&#10003;";

static const char PAGE_OK_TAIL[] =
  "</p></div></body></html>";

// Page shown after a factory reset.
static const char PAGE_RESET[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight</title>"
  "<style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:-apple-system,sans-serif;background:#0d0f11;color:#ddd;"
  "min-height:100vh;display:flex;align-items:center;"
  "justify-content:center;padding:20px}"
  ".c{background:#16181a;border:1px solid #2c3342;border-radius:16px;"
  "padding:32px 24px;max-width:400px;width:100%;text-align:center}"
  ".ic{font-size:52px;color:#40a8ff;margin-bottom:14px}"
  "h2{font-size:18px;font-weight:700;color:#ddd;margin-bottom:10px}"
  "p{font-size:13px;color:#777;line-height:1.7}"
  "</style></head><body><div class=c>"
  "<div class=ic>&#8635;</div>"
  "<h2>Resetting&hellip;</h2>"
  "<p>The controller is restarting. Afterwards, reconnect to the"
  " <strong>RecLight Setup</strong> WiFi network and set it up again.</p>"
  "</div></body></html>";

// --- HTTP form helpers -------------------------------------------------

// Simple hex-digit check (no ctype.h needed).
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}


// URL-Dekodierung in-place (application/x-www-form-urlencoded)
static void url_decode(char* s) {
    char* w = s;
    while (*s) {
        if (*s == '%') {
            int hi = hex_nibble(s[1]);
            int lo = hex_nibble(s[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char)((hi << 4) | lo); s += 3; continue; }
        }
        *w++ = (*s == '+') ? ' ' : *s;
        s++;
    }
    *w = '\0';
}

// Einzelnes Feld aus URL-enkodiertem Formular-Body extrahieren
static void form_field(const char* body, const char* key, char* out, size_t cap) {
    char search[40];
    std::snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    size_t n = 0;
    while (*p && *p != '&' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
}

// --- HTTP handlers -------------------------------------------------------

static esp_err_t portal_get(httpd_req_t* req)
{
    // "Done" just means WiFi creds were saved -- NOT that the STA radio has
    // already finished joining the home network. Gating this on live
    // g_wifiConnected() caused a real bug: right after the post-configure
    // reboot, the portal briefly (sometimes for several seconds) reports
    // step2Done=false again, which re-shows the "enter WiFi data" form even
    // though credentials are already safely stored -- very confusing, and it
    // also hides the step-3/reset UI behind a form that doesn't need to be
    // resubmitted.
    const bool step2Done = g_configured;
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_sendstr_chunk(req, P_HEAD);
    if (step2Done) {
        httpd_resp_sendstr_chunk(req, P_S2_DONE_HEAD);
        httpd_resp_sendstr_chunk(req, g_wifiConnected.load() ? P_S2_DONE_CONNECTED : P_S2_DONE_CONNECTING);
        httpd_resp_sendstr_chunk(req, P_S2_DONE_TAIL);
        httpd_resp_sendstr_chunk(req, P_S3_OK);
    } else {
        httpd_resp_sendstr_chunk(req, P_FORM);
        httpd_resp_sendstr_chunk(req, P_S3_WAIT);
    }
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

// GET /configure?ssid=...&pass=...  -> save + restart.
static esp_err_t configure_get(httpd_req_t* req)
{
    char query[320] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK || query[0] == '\0') {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    char ssid[33] = {}, pass[65] = {};
    httpd_query_key_value(query, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(query, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    ESP_LOGI("portal", "provision: ssid=\"%s\"", ssid);
    creds_save(ssid, pass);

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_sendstr_chunk(req, PAGE_OK_HEAD);
    httpd_resp_sendstr_chunk(req, PAGE_OK_MID);
    httpd_resp_sendstr_chunk(req, PAGE_OK_TAIL);
    httpd_resp_sendstr_chunk(req, nullptr);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// GET /reset  -> clear settings + restart.
static esp_err_t reset_get(httpd_req_t* req)
{
    ESP_LOGW("portal", "HTTP reset requested");
    creds_clear();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_sendstr(req, PAGE_RESET);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void http_server_start()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 6144;
    cfg.task_priority    = 4;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;
    // Default is 7 -- the captive portal doesn't need that many concurrent
    // connections, and every socket it reserves is one fewer available for
    // Ableton Link's discovery sockets out of the shared LWIP socket pool.
    cfg.max_open_sockets = 4;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGW("portal", "HTTP server start failed");
        return;
    }

    // Register specific routes first, then the wildcard catch-all.
    static const httpd_uri_t u_reset     = { .uri="/reset",     .method=HTTP_GET,
        .handler=reset_get,      .user_ctx=nullptr };
    static const httpd_uri_t u_configure = { .uri="/configure", .method=HTTP_GET,
        .handler=configure_get,  .user_ctx=nullptr };
    static const httpd_uri_t u_catchall  = { .uri="/*",         .method=HTTP_GET,
        .handler=portal_get,     .user_ctx=nullptr };

    httpd_register_uri_handler(server, &u_reset);
    httpd_register_uri_handler(server, &u_configure);
    httpd_register_uri_handler(server, &u_catchall);
    ESP_LOGI("portal", "HTTP captive portal up on :80");
}

// Minimal DNS server: answers every A query with 192.168.4.1 so the client's
// browser automatically points at our HTTP setup portal.
static void dns_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(nullptr); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(53);
    addr.sin_addr.s_addr = inet_addr("192.168.4.1"); // AP interface only

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGW("portal", "DNS bind failed (port 53 in use?)");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI("portal", "DNS forwarder up on :53");

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&src, &src_len);
        if (n < 12) continue;

        // Reply: copy the query packet, set the flags to "Response +
        // Authoritative", ANCOUNT=1, then append an A record with 192.168.4.1.
        uint8_t resp[512];
        int rlen = (n < (int)sizeof(resp) - 20) ? n : (int)sizeof(resp) - 20;
        memcpy(resp, buf, rlen);

        resp[2] = 0x81; // QR=1, AA=1, RD=1
        resp[3] = 0x80; // RA=1, RCODE=0
        resp[6] = 0;  resp[7] = 1; // ANCOUNT = 1
        resp[8] = 0;  resp[9] = 0; // NSCOUNT = 0
        resp[10] = 0; resp[11] = 0; // ARCOUNT = 0

        // Antwort-RR: Name-Pointer auf Offset 12 (Frage-QNAME)
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // Type A
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // Class IN
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C; // TTL 60 s
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; // RDLENGTH 4
        resp[rlen++] = 192;  resp[rlen++] = 168;
        resp[rlen++] = 4;    resp[rlen++] = 1;    // 192.168.4.1

        sendto(sock, resp, rlen, 0,
               (struct sockaddr*)&src, src_len);
    }
}

extern "C" void app_main() {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  gpio_setup();

  if (oled_init(OLED_SDA_PIN, OLED_SCL_PIN)) {
    oled_show_lines("RecLight", "starting...", "", "");
  }

  // boot blink
  set_lamp(true);  vTaskDelay(pdMS_TO_TICKS(150));
  set_lamp(false); vTaskDelay(pdMS_TO_TICKS(120));

  wifi_start();  // also runs creds_load()

  // Captive portal: setup page for devices joining the "RecLight Setup" WiFi.
  // Always available so the network can be (re)configured at any time.
  http_server_start();
  xTaskCreate(dns_task, "dns", 3072, nullptr, 4, nullptr);

  // UDP services (plugin control + backward-compatible provisioning).
  xTaskCreate(provisioning_task, "prov", 4096, nullptr, 5, nullptr);
  xTaskCreate(control_task, "ctrl", 4096, nullptr, 5, nullptr);
  xTaskCreate(announce_task, "announce", 4096, nullptr, 4, nullptr);

  // Ableton Link -- always enabled alongside the plugin's WiFi control;
  // enabling happens after WiFi connects (see g_linkReenable in the main
  // loop) so it binds the right STA interface.
  static ableton::Link link(120.0);
  link.enableStartStopSync(true);
  g_link = &link;

  // Priority 5 matches the WiFi control/provisioning tasks and is above
  // Link's background task (2) -- see the comment on lamp_task() above.
  xTaskCreate(lamp_task, "lamp", 2048, nullptr, 5, nullptr);

  bool linkEnabled = false;
  int64_t lastHud = 0;
  int64_t lastDisp = 0;
  int64_t btnHeldSince = -1LL;

  while (true) {
    // ---- BOOT-button hold-to-reset (hardware fallback for the browser reset) --
    const int64_t nowBtn = esp_timer_get_time();
    if (gpio_get_level(BOOT_BTN_PIN) == 0) {
      if (btnHeldSince < 0LL) btnHeldSince = nowBtn;
      if (nowBtn - btnHeldSince >= FACTORY_RESET_HOLD_MS * 1000LL) {
        ESP_LOGW(TAG, "BOOT button held -- factory reset");
        oled_show_lines("Resetting...", "Release the", "button and", "rejoin setup");
        creds_clear();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      }
    } else {
      btnHeldSince = -1LL;
    }

    // ---- Link enable / re-enable after WiFi connect -------------------------
    if (g_linkReenable.exchange(false)) {
      if (!linkEnabled) {
        link.enable(true);
        linkEnabled = true;
        ESP_LOGI(TAG, "Link enabled on first WiFi connect");
      } else {
        // Reconnect: cycle Link so it re-discovers the interface.
        link.enable(false);
        vTaskDelay(pdMS_TO_TICKS(200));
        link.enable(true);
        ESP_LOGI(TAG, "Link re-enabled after WiFi reconnect");
      }
    }

    auto state = link.captureAppSessionState();
    g_linkPlaying = linkEnabled && state.isPlaying();

    // ---- Lamp state (for HUD/OLED only -- the actual GPIO is driven by
    // lamp_task(), see above) ------------------------------------------------
    const int64_t now = esp_timer_get_time();

    const bool lampSolid     = g_pluginPlay.load() || g_linkPlaying.load();
    const bool lampSlowBlink = !lampSolid && g_pluginRec.load();
    const bool anyActive = lampSolid || lampSlowBlink;
    const int64_t last_active_us = g_lastActiveUs.load();
    const bool lampFastBlink = !anyActive && (last_active_us >= 0LL) &&
                                (now - last_active_us < 10000000LL);

    if (now - lastHud > 2000000) {
      lastHud = now;
      const char* lampMode = lampSolid ? "solid" : lampSlowBlink ? "slow-blink" :
                         lampFastBlink ? "fast-blink" : "off";
      ESP_LOGI(TAG, "peers=%u tempo=%.1f linkPlaying=%d pluginRec=%d pluginPlay=%d wifi=%d lamp=%s",
               (unsigned)link.numPeers(), state.tempo(),
               (int)g_linkPlaying.load(), (int)g_pluginRec.load(), (int)g_pluginPlay.load(),
               (int)g_wifiConnected.load(), lampMode);
    }

    if (now - lastDisp > 300000) {  // refresh OLED ~3x/s (only redraws on change)
      lastDisp = now;
      update_display(anyActive, (unsigned)link.numPeers());
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
