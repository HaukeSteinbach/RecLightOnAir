#pragma once

#include <JuceHeader.h>
#include <memory>

class RecLightBluetoothBridge
{
public:
    virtual ~RecLightBluetoothBridge() = default;

    virtual void rescan() = 0;
    virtual bool isAvailable() const = 0;
    virtual bool isConnected() const = 0;
    virtual juce::String getStatusText() const = 0;
    virtual bool sendCredentials (const juce::String& ssid,
                                  const juce::String& password) = 0;
    virtual bool sendResetCommand() = 0;
    // message is forwarded as-is to the ESP's control characteristic, e.g.
    // "REC:1" / "PLAY:1" / "REC:0" -- keeps Bluetooth in sync with the exact
    // same protocol used over WiFi UDP.
    virtual bool sendControlState (const juce::String& message) = 0;
};

std::unique_ptr<RecLightBluetoothBridge> createRecLightBluetoothBridge();