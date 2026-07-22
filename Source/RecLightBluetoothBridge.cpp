#include "RecLightBluetoothBridge.h"

namespace
{
class NullBluetoothBridge final : public RecLightBluetoothBridge
{
public:
    void rescan() override {}

    bool isAvailable() const override
    {
        return false;
    }

    bool isConnected() const override
    {
        return false;
    }

    juce::String getStatusText() const override
    {
        return "Bluetooth is not available on this platform";
    }

    bool sendCredentials (const juce::String&, const juce::String&) override
    {
        return false;
    }

    bool sendResetCommand() override
    {
        return false;
    }

    bool sendControlState (const juce::String&) override
    {
        return false;
    }
};
}

std::unique_ptr<RecLightBluetoothBridge> createRecLightBluetoothBridge()
{
    return std::make_unique<NullBluetoothBridge>();
}