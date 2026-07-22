#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "RecLightBluetoothBridge.h"

class OnAirAudioProcessor : public juce::AudioProcessor,
                            private juce::Timer
{
public:
    enum class LampState { Off, Playing, Recording };

    OnAirAudioProcessor();
    ~OnAirAudioProcessor() override { stopTimer(); sendLampState (LampState::Off); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OnAir Recording Light"; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Operating network of the ESP - found automatically via ONAIR_IP broadcast.
    // Manual entry only needed as a fallback.
    juce::String targetIp;
    int          targetPort { 4300 };

    // First-time setup: connect the Mac to the ESP's "RecLight Setup" WiFi,
    // then send the SSID/password here.
    juce::String wifiSsid;
    juce::String wifiPassword;
    juce::String setupIp   { "192.168.4.1" };
    int          setupPort { 4212 };

    std::atomic<bool> lastIsPlaying   { false };
    std::atomic<bool> lastIsRecording { false };
    std::atomic<int64_t> lastProcessBlockMs { 0 }; // 0 = never called yet
    std::atomic<bool> manualOn { false };         // Toggle: LED permanently on

    // ESP reachability (updated via ONAIR_IP broadcast every 10s)
    std::atomic<int64_t> lastEspContactMs { 0 }; // ms timestamp of last contact
    std::atomic<bool>    espReachable      { false };

    void sendLampState (LampState state);
    void sendProvisioning();
    void resetNetwork();
    const juce::String& getSetupStatus() const noexcept { return setupStatus; }
    bool isEspUnreachableWarning() const noexcept { return espLostWarningShown; }
    juce::String getBluetoothStatus() const;
    bool isBluetoothConnected() const;
    void rescanBluetooth();

private:
    void timerCallback() override;

    void updateSetupStatus (juce::String newStatus);
    void loadFromDisk();   // Loads saved settings from the PropertiesFile
    void saveToDisk();     // Persists current settings

    enum class LedMode { Normal, ManualOn };
    LedMode ledMode      { LedMode::Normal };
    LampState prevLampState { LampState::Off };
    juce::String setupStatus { "Setup: connect this Mac to the 'RecLight Setup' WiFi" };
    std::unique_ptr<RecLightBluetoothBridge> bluetoothBridge;

    juce::DatagramSocket socket          { true };
    juce::DatagramSocket discoverySocket { true }; // listens on port 4211 for ONAIR_IP broadcasts

    int64_t constructedAtMs     { 0 };    // set in the ctor and in setStateInformation
    bool    espLostWarningShown { false }; // only accessed from the timer thread
    static constexpr int64_t kEspTimeoutMs = 35000LL; // 3.5x the broadcast interval (10s)

    LampState lastSentLampState { LampState::Off }; // last state sent to the ESP
    int  heartbeatTick  { 0 };     // counter for the 5s heartbeat


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnAirAudioProcessor)
};
