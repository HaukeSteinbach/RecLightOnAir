// RecLightBluetoothBridge.mm -- macOS implementation of the RecLightBluetoothBridge
// interface. Talks to a separate RecLightBLEHelper process over loopback UDP
// (see RecLightBLEProtocol.h) instead of touching CoreBluetooth directly.
//
// CoreBluetooth must never run inside this plugin binary: macOS TCC can crash
// the DAW host process if a plugin touches Bluetooth in-process. All the
// actual Bluetooth work happens in RecLightBLEHelper, a tiny always-safe
// executable bundled inside Contents/Resources and spawned on demand.

#import <Foundation/Foundation.h>

#include "RecLightBLEProtocol.h"
#include "RecLightBluetoothBridge.h"

#if JUCE_MAC

namespace
{
// Finds this plugin's own bundle regardless of wrapper type (VST3/AU load
// inside a host app, so [NSBundle mainBundle] would return the *host's*
// bundle, not ours -- looking up by our own bundle identifier works for
// every wrapper type, including Standalone).
NSBundle* findOwnBundle()
{
    CFBundleRef ref = CFBundleGetBundleWithIdentifier (CFSTR ("com.steinbach-audio.reclight"));
    if (ref == nullptr)
        return nil;

    NSURL* url = CFBridgingRelease (CFBundleCopyBundleURL (ref));
    return [NSBundle bundleWithURL: url];
}

void launchHelperProcess()
{
    @autoreleasepool
    {
        NSBundle* bundle = findOwnBundle();
        if (bundle == nil)
            return;

        NSString* helperPath = [[bundle.bundleURL URLByAppendingPathComponent: @"Contents/Resources/RecLightBLEHelper"]
                                 path];

        if (! [[NSFileManager defaultManager] isExecutableFileAtPath: helperPath])
            return;

        NSTask* task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath: helperPath];
        task.standardOutput = nil;
        task.standardError  = nil;

        NSError* error = nil;
        [task launchAndReturnError: &error];
        juce::ignoreUnused (error); // helper singleton-guards itself; a launch failure here is harmless
    }
}
}

class RecLightBluetoothBridgeImpl final : public RecLightBluetoothBridge,
                                          private juce::Timer
{
public:
    RecLightBluetoothBridgeImpl()
    {
        socket.bindToPort (0);
        launchHelperProcess();
        lastLaunchAttemptMs = juce::Time::getMillisecondCounter();
        startTimerHz (4);
    }

    ~RecLightBluetoothBridgeImpl() override
    {
        stopTimer();
    }

    void rescan() override
    {
        // The helper always scans on its own; nothing to trigger here.
    }

    bool isAvailable() const override { return available; }
    bool isConnected() const override { return connected; }

    juce::String getStatusText() const override
    {
        const juce::ScopedLock lock (statusLock);
        return statusText;
    }

    bool sendCredentials (const juce::String& ssid, const juce::String& password) override
    {
        if (ssid.trim().isEmpty())
        {
            setStatus ("Bluetooth: SSID missing");
            return false;
        }

        ensureHelperRunning();

        const auto ssidB64 = juce::Base64::toBase64 (ssid.toRawUTF8(), (size_t) ssid.getNumBytesAsUTF8());
        const auto passB64 = juce::Base64::toBase64 (password.toRawUTF8(), (size_t) password.getNumBytesAsUTF8());
        sendCommand ("CREDS:" + ssidB64 + ":" + passB64);
        setStatus ("Bluetooth: WiFi details sent");
        return true;
    }

    bool sendResetCommand() override
    {
        ensureHelperRunning();
        sendCommand ("RESET");
        setStatus ("Bluetooth: network reset sent");
        return true;
    }

    bool sendControlState (const juce::String& message) override
    {
        if (! connected)
            return false;

        sendCommand ("CONTROL:" + message);
        return true;
    }

private:
    void timerCallback() override { poll(); }

    void poll()
    {
        if (socket.waitUntilReady (false, 0) == 1)
        {
            char buf[256] = {};
            juce::String senderIp;
            int senderPort = 0;
            int len = socket.read (buf, (int) sizeof (buf) - 1, false, senderIp, senderPort);
            if (len > 0)
            {
                handleReply (juce::String (buf, (size_t) len));
                lastReplyMs = juce::Time::getMillisecondCounter();
            }
        }

        ensureHelperRunning();
        sendCommand ("STATUS?");
    }

    void ensureHelperRunning()
    {
        const auto now = juce::Time::getMillisecondCounter();
        const bool noRecentReply = (now - lastReplyMs) > 2000;
        const bool longEnoughSinceLaunch = (now - lastLaunchAttemptMs) > 4000;

        if (noRecentReply && longEnoughSinceLaunch)
        {
            launchHelperProcess();
            lastLaunchAttemptMs = now;
        }
    }

    void handleReply (const juce::String& msg)
    {
        if (! msg.startsWith ("STATUS:"))
            return;

        auto rest  = msg.fromFirstOccurrenceOf ("STATUS:", false, false);
        auto avail = rest.upToFirstOccurrenceOf ("|", false, false);
        rest       = rest.fromFirstOccurrenceOf ("|", false, false);
        auto conn  = rest.upToFirstOccurrenceOf ("|", false, false);
        auto text  = rest.fromFirstOccurrenceOf ("|", false, false);

        available = avail == "1";
        connected = conn == "1";
        setStatus (text.isNotEmpty() ? text : juce::String ("Bluetooth"));
    }

    void sendCommand (const juce::String& cmd)
    {
        socket.write ("127.0.0.1", kRecLightBleHelperPort, cmd.toRawUTF8(), (int) cmd.getNumBytesAsUTF8());
    }

    void setStatus (juce::String newStatus)
    {
        const juce::ScopedLock lock (statusLock);
        statusText = std::move (newStatus);
    }

    juce::DatagramSocket socket { true };
    std::atomic<bool> available { false };
    std::atomic<bool> connected { false };
    mutable juce::CriticalSection statusLock;
    juce::String statusText { "Bluetooth starting\u2026" };

    uint32_t lastReplyMs { 0 };
    uint32_t lastLaunchAttemptMs { 0 };
};

std::unique_ptr<RecLightBluetoothBridge> createRecLightBluetoothBridge()
{
    return std::make_unique<RecLightBluetoothBridgeImpl>();
}

#else

std::unique_ptr<RecLightBluetoothBridge> createRecLightBluetoothBridge()
{
    return {};
}

#endif
