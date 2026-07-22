#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// U8g2/Wire fest einbinden: bei bedingtem __has_include erkennt arduino-cli die
// Bibliothek oft nicht und kompiliert den OLED-Code gar nicht mit.
#include <Wire.h>
#include <U8g2lib.h>
#define REC_HAS_OLED 1

namespace
{
const bool USE_STATIC_IP = false;
IPAddress LOCAL_IP (192, 168, 2, 118);
IPAddress GATEWAY  (192, 168, 2,   1);
IPAddress SUBNET   (255, 255, 255,  0);
IPAddress DNS1     (8, 8, 8, 8);

const int CONTROL_PORT = 4210;   // Fallback / Basis (unbenutzt bei Studio-Ports)
const int ANNOUNCE_PORT = 4211;
const int CONFIG_PORT = 4212;

// Mehrere Studios im selben Netz: Studio 1-20 -> eigener Steuer-Port
const int STUDIO_PORT_BASE = 4300;
const int STUDIO_MIN = 1;
const int STUDIO_MAX = 20;

const int LED_PIN = 2;
// 01Space ESP32-C3 0.42" OLED-Board: OLED haengt an GPIO5 (SDA) / GPIO6 (SCL).
// (GPIO8 = onboard RGB-LED, deshalb bleibt das Display bei 8/9 schwarz.)
const int OLED_SDA_PIN = 5;
const int OLED_SCL_PIN = 6;

const char* BLE_DEVICE_NAME = "RecLight";
const char* BLE_SERVICE_UUID = "19B10000-E8F2-537E-4F6C-D104768A1214";
const char* BLE_SSID_UUID = "19B10001-E8F2-537E-4F6C-D104768A1214";
const char* BLE_PASS_UUID = "19B10002-E8F2-537E-4F6C-D104768A1214";
const char* BLE_COMMAND_UUID = "19B10003-E8F2-537E-4F6C-D104768A1214";
const char* BLE_STATUS_UUID = "19B10004-E8F2-537E-4F6C-D104768A1214";
const char* BLE_CONTROL_UUID = "19B10005-E8F2-537E-4F6C-D104768A1214";

const char* PREF_NAMESPACE = "reclight";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";
const char* PREF_STUDIO = "studio";

const char* SETUP_AP_SSID = "RecLight Setup";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long ACTIVE_BLINK_INTERVAL_MS = 220;
const unsigned long HOLD_DURATION_MS = 10000;
const unsigned long HOLD_BLINK_INTERVAL_MS = 900;
const unsigned long ANNOUNCE_INTERVAL_MS = 10000;
}

enum class LampMode
{
    Idle,
    Recording,
    Hold
};

Preferences preferences;
WiFiUDP controlUdp;
WiFiUDP announceUdp;
WiFiUDP configUdp;

String wifiSsid;
String wifiPassword;
int studioNumber = 1;
bool wifiConnected = false;
LampMode lampMode = LampMode::Idle;
bool setupMode = false;
bool lampState = false;
unsigned long holdUntilMs = 0;
unsigned long lastBlinkToggleMs = 0;
unsigned long lastAnnounceMs = 0;
bool bleClientConnected = false;
String pendingBleSsid;
String pendingBlePassword;
int pendingBleStudio = 1;

BLEServer* bleServer = nullptr;
BLECharacteristic* bleStatusCharacteristic = nullptr;

#if REC_HAS_OLED
// Pins direkt im Konstruktor uebergeben: display.begin() setzt Wire sonst auf die
// Standard-Pins zurueck -> Display bliebe schwarz.
U8G2_SSD1306_72X40_ER_F_HW_I2C display (U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);
#endif

void updateBleStatus (const String& message);
void startBleAdvertising();
bool applyProvisionedCredentials (const String& ssid, const String& password, int studio);
void applyControlMessage (const String& message);
void resetNetworkSettings();

int clampStudio (int n)
{
    if (n < STUDIO_MIN) return STUDIO_MIN;
    if (n > STUDIO_MAX) return STUDIO_MAX;
    return n;
}

int controlPort()
{
    return STUDIO_PORT_BASE + (studioNumber - 1);
}

void setLamp (bool on)
{
    lampState = on;
    digitalWrite (LED_PIN, on ? LOW : HIGH);
}

void showDisplayMessage (const char* text)
{
#if REC_HAS_OLED
    display.clearBuffer();
    display.setFont (u8g2_font_6x13B_tf);

    const int displayWidth = 72;
    const int textWidth = display.getStrWidth (text);
    const int x = (displayWidth - textWidth) / 2;
    display.drawStr (x < 0 ? 0 : x, 24, text);
    display.sendBuffer();
#else
    (void) text;
#endif
}

void clearDisplayOff()
{
#if REC_HAS_OLED
    display.clearBuffer();
    display.sendBuffer();
#endif
}

void showBigMessage (const char* text)
{
#if REC_HAS_OLED
    display.clearBuffer();
    display.setFont (u8g2_font_fub20_tr);   // grosse, fette Schrift

    const int textWidth = display.getStrWidth (text);
    int x = (72 - textWidth) / 2;
    if (x < 0) x = 0;

    const int y = (40 + display.getAscent()) / 2; // vertikal zentriert
    display.drawStr (x, y, text);
    display.sendBuffer();
#else
    (void) text;
#endif
}

void showLines (const char* const* lines, int count)
{
#if REC_HAS_OLED
    display.clearBuffer();
    display.setFont (u8g2_font_5x7_tf);

    const int lineH = 10;                          // Zeilenhoehe fuer 5x7-Font
    const int block = count * lineH;
    int y = (40 - block) / 2 + 8;                  // vertikal zentrierter Block
    if (y < 8) y = 8;

    for (int i = 0; i < count; ++i)
    {
        int x = (72 - display.getStrWidth (lines[i])) / 2;
        display.drawStr (x < 0 ? 0 : x, y + i * lineH, lines[i]);
    }
    display.sendBuffer();
#else
    (void) lines; (void) count;
#endif
}

// Schritt 1: Kunde soll sich mit dem Setup-WLAN verbinden.
void showStep1Connect()
{
    static const char* lines[3] = { "Step 1", "Join WiFi:", "RecLight Setup" };
    showLines (lines, 3);
}

// Schritt 2: Setup-WLAN verbunden -> Zugangsdaten im Plugin eintragen.
void showStep2EnterCredentials()
{
    static const char* lines[4] = { "Step 2", "Enter WiFi name", "& password", "in the plugin" };
    showLines (lines, 4);
}

// Schritt 3: Daten empfangen -> Neustart.
void showStep3Received()
{
    static const char* lines[3] = { "Step 3", "Data received", "Restarting..." };
    showLines (lines, 3);
}

void showIdleDisplay()
{
    // Ruhezustand: nicht verbunden -> Schritt-fuer-Schritt-Anleitung,
    // verbunden & keine Aufnahme -> Display aus (auch bei Hold).
    if (wifiConnected)
    {
        clearDisplayOff();
        return;
    }

    // Setup: Schritt 1 (noch kein Geraet im Setup-WLAN) oder Schritt 2 (verbunden).
    if (setupMode && WiFi.softAPgetStationNum() > 0)
        showStep2EnterCredentials();
    else
        showStep1Connect();
}

void setLampMode (LampMode newMode)
{
    lampMode = newMode;
    lastBlinkToggleMs = millis();

    if (newMode == LampMode::Idle)
    {
        holdUntilMs = 0;
        setLamp (false);
        showIdleDisplay();
        return;
    }

    setLamp (true);

    if (newMode == LampMode::Recording)
        showBigMessage ("REC");
    else
        clearDisplayOff();   // Hold: LED blinkt entspannt, Display bleibt aus
}

void scanI2C()
{
    Wire.begin (OLED_SDA_PIN, OLED_SCL_PIN);
    Serial.printf ("I2C-Scan (SDA=%d, SCL=%d):\n", OLED_SDA_PIN, OLED_SCL_PIN);
    int found = 0;
    for (uint8_t addr = 1; addr < 127; ++addr)
    {
        Wire.beginTransmission (addr);
        if (Wire.endTransmission() == 0)
        {
            Serial.printf ("  Geraet gefunden: 0x%02X\n", addr);
            ++found;
        }
    }
    if (found == 0)
        Serial.println ("  Kein I2C-Geraet gefunden - Pins/Verkabelung pruefen!");
}

void initDisplay()
{
#if REC_HAS_OLED
    display.setBusClock (400000);
    display.begin();
    display.setContrast (255);
#endif
    showIdleDisplay();
}

class RecLightBleServerCallbacks : public BLEServerCallbacks
{
public:
    void onConnect (BLEServer*) override
    {
        bleClientConnected = true;
        updateBleStatus ("BLE READY");
    }

    void onDisconnect (BLEServer*) override
    {
        bleClientConnected = false;
        startBleAdvertising();
        updateBleStatus (setupMode ? "BLE SETUP" : "BLE IDLE");
    }
};

class RecLightBleSsidCallbacks : public BLECharacteristicCallbacks
{
public:
    void onWrite (BLECharacteristic* characteristic) override
    {
        pendingBleSsid = characteristic->getValue();
        pendingBleSsid.trim();
        updateBleStatus (pendingBleSsid.isEmpty() ? "BLE SSID?" : "BLE SSID OK");
    }
};

class RecLightBlePassCallbacks : public BLECharacteristicCallbacks
{
public:
    void onWrite (BLECharacteristic* characteristic) override
    {
        pendingBlePassword = characteristic->getValue();
        updateBleStatus ("BLE PASS OK");
    }
};

class RecLightBleCommandCallbacks : public BLECharacteristicCallbacks
{
public:
    void onWrite (BLECharacteristic* characteristic) override
    {
        String command = characteristic->getValue();
        command.trim();

        if (command.equalsIgnoreCase ("RESET"))
        {
            resetNetworkSettings();
        }
        else if (command.startsWith ("APPLY"))
        {
            int studio = pendingBleStudio;
            const int colon = command.indexOf (':');
            if (colon >= 0)
                studio = clampStudio (command.substring (colon + 1).toInt());

            applyProvisionedCredentials (pendingBleSsid, pendingBlePassword, studio);
        }
    }
};

class RecLightBleControlCallbacks : public BLECharacteristicCallbacks
{
public:
    void onWrite (BLECharacteristic* characteristic) override
    {
        String message = characteristic->getValue();
        message.trim();
        applyControlMessage (message);
    }
};

void initBle()
{
    BLEDevice::init (BLE_DEVICE_NAME);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks (new RecLightBleServerCallbacks());

    BLEService* service = bleServer->createService (BLE_SERVICE_UUID);

    BLECharacteristic* ssidCharacteristic = service->createCharacteristic (
        BLE_SSID_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    ssidCharacteristic->setCallbacks (new RecLightBleSsidCallbacks());
    ssidCharacteristic->setValue (wifiSsid);

    BLECharacteristic* passCharacteristic = service->createCharacteristic (
        BLE_PASS_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    passCharacteristic->setCallbacks (new RecLightBlePassCallbacks());

    BLECharacteristic* commandCharacteristic = service->createCharacteristic (
        BLE_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    commandCharacteristic->setCallbacks (new RecLightBleCommandCallbacks());

    bleStatusCharacteristic = service->createCharacteristic (
        BLE_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    bleStatusCharacteristic->addDescriptor (new BLE2902());
    bleStatusCharacteristic->setValue ("BOOT");

    BLECharacteristic* controlCharacteristic = service->createCharacteristic (
        BLE_CONTROL_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    controlCharacteristic->setCallbacks (new RecLightBleControlCallbacks());

    service->start();
    startBleAdvertising();
}

void startBleAdvertising()
{
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->stop();
    advertising->addServiceUUID (BLE_SERVICE_UUID);
    advertising->setScanResponse (true);
    BLEDevice::startAdvertising();
}

void updateBleStatus (const String& message)
{
    if (bleStatusCharacteristic == nullptr)
        return;

    bleStatusCharacteristic->setValue (message);
    if (bleClientConnected)
        bleStatusCharacteristic->notify();
}

void beginConfigSocket()
{
    configUdp.stop();
    configUdp.begin (CONFIG_PORT);
}

void beginStationSockets()
{
    controlUdp.stop();
    announceUdp.stop();
    controlUdp.begin (controlPort());
    announceUdp.begin (ANNOUNCE_PORT);
    beginConfigSocket();
}

void announceIP()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    String msg = "ONAIR_IP:" + WiFi.localIP().toString() + ":" + String (studioNumber);
    announceUdp.beginPacket (IPAddress (255, 255, 255, 255), ANNOUNCE_PORT);
    announceUdp.write ((const uint8_t*) msg.c_str(), msg.length());
    announceUdp.endPacket();
}

void sendConfigReply (const String& message)
{
    configUdp.beginPacket (configUdp.remoteIP(), configUdp.remotePort());
    configUdp.write ((const uint8_t*) message.c_str(), message.length());
    configUdp.endPacket();
}

void sendConfigReplyTo (IPAddress address, uint16_t port, const String& message)
{
    configUdp.beginPacket (address, port);
    configUdp.write ((const uint8_t*) message.c_str(), message.length());
    configUdp.endPacket();
}

void loadCredentials()
{
    preferences.begin (PREF_NAMESPACE, false);
    wifiSsid = preferences.getString (PREF_SSID, "");
    wifiPassword = preferences.getString (PREF_PASS, "");
    studioNumber = clampStudio (preferences.getInt (PREF_STUDIO, 1));
}

void saveCredentials (const String& ssid, const String& password, int studio)
{
    wifiSsid = ssid;
    wifiPassword = password;
    studioNumber = clampStudio (studio);
    preferences.putString (PREF_SSID, ssid);
    preferences.putString (PREF_PASS, password);
    preferences.putInt (PREF_STUDIO, studioNumber);
    updateBleStatus ("CFG SAVED");
}

String configValue (const String& message, const String& key)
{
    const String prefix = key + ':';
    const int start = message.indexOf (prefix);

    if (start < 0)
        return {};

    const int valueStart = start + prefix.length();
    const int valueEnd = message.indexOf ('\n', valueStart);

    String value = valueEnd >= 0 ? message.substring (valueStart, valueEnd)
                                 : message.substring (valueStart);
    value.trim();
    return value;
}

void startSetupAccessPoint()
{
    WiFi.disconnect();
    WiFi.mode (WIFI_AP_STA);
    WiFi.softAP (SETUP_AP_SSID);

    controlUdp.stop();
    announceUdp.stop();
    beginConfigSocket();

    setupMode = true;
    wifiConnected = false;
    setLampMode (LampMode::Idle);
    updateBleStatus ("SETUP AP");
}

void resetNetworkSettings()
{
    preferences.remove (PREF_SSID);
    preferences.remove (PREF_PASS);
    preferences.remove (PREF_STUDIO);

    wifiSsid = "";
    wifiPassword = "";
    studioNumber = 1;
    pendingBleSsid = "";
    pendingBlePassword = "";
    pendingBleStudio = 1;
    wifiConnected = false;

    updateBleStatus ("CFG RESET");
    showDisplayMessage ("RESET");
    delay (700);
    startSetupAccessPoint();
}

bool connectToConfiguredWiFi()
{
    if (wifiSsid.isEmpty())
        return false;

    wifiConnected = false;
    WiFi.disconnect();
    WiFi.mode (setupMode ? WIFI_AP_STA : WIFI_STA);

    if (USE_STATIC_IP)
        WiFi.config (LOCAL_IP, GATEWAY, SUBNET, DNS1);

    WiFi.begin (wifiSsid.c_str(), wifiPassword.c_str());
    showDisplayMessage ("Connecting");
    updateBleStatus ("WIFI...");

    bool blink = false;
    const unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)
    {
        blink = !blink;
        setLamp (blink);
        delay (150);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        wifiConnected = false;
        setLamp (false);
        updateBleStatus ("CFG ERR WIFI");
        return false;
    }

    WiFi.softAPdisconnect (true);
    WiFi.mode (WIFI_STA);

    setupMode = false;
    wifiConnected = true;
    beginStationSockets();
    announceIP();
    lastAnnounceMs = millis();
    setLampMode (LampMode::Idle);
    updateBleStatus ("CFG OK " + WiFi.localIP().toString());
    return true;
}

bool applyProvisionedCredentials (const String& ssid, const String& password, int studio)
{
    if (ssid.isEmpty())
    {
        updateBleStatus ("CFG ERR SSID");
        return false;
    }

    pendingBleSsid = ssid;
    pendingBlePassword = password;
    saveCredentials (ssid, password, studio);
    return connectToConfiguredWiFi();
}

void handleProvisioningPacket()
{
    const int packetSize = configUdp.parsePacket();
    if (packetSize <= 0)
        return;

    const IPAddress replyAddress = configUdp.remoteIP();
    const uint16_t replyPort = configUdp.remotePort();

    char incoming[160] = {};
    const int length = configUdp.read (incoming, sizeof (incoming) - 1);
    if (length <= 0)
        return;

    String message (incoming);
    message.trim();

    if (message.startsWith ("RESET"))
    {
        sendConfigReplyTo (replyAddress, replyPort, "CFG:RESET");
        resetNetworkSettings();
        return;
    }

    if (! message.startsWith ("CFG:1"))
        return;

    const String ssid = configValue (message, "SSID");
    const String password = configValue (message, "PASS");
    const String studioStr = configValue (message, "PORT");
    const int studio = studioStr.length() > 0 ? clampStudio (studioStr.toInt())
                                              : studioNumber;

    if (ssid.isEmpty())
    {
        sendConfigReply ("CFG:ERR SSID");
        return;
    }

    saveCredentials (ssid, password, studio);
    sendConfigReply ("CFG:OK SAVED");

    // Schritt 3: Daten empfangen -> sauberer Neustart, verbindet dann selbst.
    showStep3Received();
    updateBleStatus ("CFG OK RESTART");
    delay (1500);
    ESP.restart();
}

void applyControlMessage (const String& message)
{
    if (message == "REC:1")
    {
        setLampMode (LampMode::Recording);
        updateBleStatus ("REC");
    }
    else if (message == "REC:0")
    {
        if (lampMode == LampMode::Recording || lampMode == LampMode::Hold)
        {
            holdUntilMs = millis() + HOLD_DURATION_MS;
            setLampMode (LampMode::Hold);
        }
        else
        {
            setLampMode (LampMode::Idle);
        }

        updateBleStatus ("HOLD");
    }
}

void handleControlPacket()
{
    const int packetSize = controlUdp.parsePacket();
    if (packetSize <= 0)
        return;

    char incoming[64] = {};
    const int length = controlUdp.read (incoming, sizeof (incoming) - 1);
    if (length <= 0)
        return;

    String message (incoming);
    message.trim();
    applyControlMessage (message);
}

void updateBlinking()
{
    const unsigned long now = millis();

    if (lampMode == LampMode::Idle)
    {
        // WLAN-Status sichtbar machen:
        //  - nicht verbunden / Setup: kurzes Suchblinken (~alle 1.5s ein Blitz)
        //  - verbunden & bereit:      LED aus
        if (! wifiConnected)
        {
            const unsigned long cycle  = 1500;
            const unsigned long onTime = 120;
            setLamp ((now % cycle) < onTime);
        }
        else if (lampState)
        {
            setLamp (false);
        }
        return;
    }

    if (lampMode == LampMode::Hold && holdUntilMs > 0 && now >= holdUntilMs)
    {
        setLampMode (LampMode::Idle);
        return;
    }

    const unsigned long interval = lampMode == LampMode::Recording
        ? ACTIVE_BLINK_INTERVAL_MS
        : HOLD_BLINK_INTERVAL_MS;

    if (now - lastBlinkToggleMs >= interval)
    {
        lastBlinkToggleMs = now;
        setLamp (! lampState);
    }
}

void setup()
{
    Serial.begin (115200);
    delay (200);

    pinMode (LED_PIN, OUTPUT);
    setLamp (false);

    scanI2C();
    initDisplay();

    setLamp (true);  delay (180);
    setLamp (false); delay (120);
    setLamp (true);  delay (180);
    setLamp (false);

    loadCredentials();
    initBle();

    if (! connectToConfiguredWiFi())
        startSetupAccessPoint();
}

void loop()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (millis() - lastAnnounceMs >= ANNOUNCE_INTERVAL_MS)
        {
            announceIP();
            lastAnnounceMs = millis();
        }

        handleControlPacket();
    }
    else if (! setupMode)
    {
        startSetupAccessPoint();
    }

    // Setup: Anzeige zwischen Schritt 1 und Schritt 2 wechseln, sobald sich ein
    // Geraet mit dem Setup-WLAN verbindet bzw. wieder trennt.
    if (setupMode && lampMode == LampMode::Idle)
    {
        static int lastApStations = -1;
        const int stations = (int) WiFi.softAPgetStationNum();
        if (stations != lastApStations)
        {
            lastApStations = stations;
            showIdleDisplay();
        }
    }

    handleProvisioningPacket();
    updateBlinking();
}
