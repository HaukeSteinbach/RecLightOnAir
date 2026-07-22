// RecLightBLEHelper.mm -- standalone macOS helper process that does the
// actual CoreBluetooth work for "Plugin via Bluetooth" mode.
//
// CoreBluetooth must never run inside the plugin binary itself (macOS TCC can
// crash the DAW host process if a plugin touches Bluetooth in-process). This
// tiny, always-safe executable is bundled inside each plugin format's
// Contents/Resources folder; the plugin (RecLightBluetoothBridge.mm) spawns
// it on demand and talks to it over plain loopback UDP -- see
// RecLightBLEProtocol.h for the wire protocol.
//
// Only one instance ever runs at a time: it binds a fixed loopback UDP port
// as a singleton lock and exits immediately if that port is already taken.
// It also exits on its own after a period with no requests from any plugin
// instance, so it never lingers once every DAW/host has closed.

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "RecLightBLEProtocol.h"

namespace
{
NSString* const kServiceUuid = @"19B10000-E8F2-537E-4F6C-D104768A1214";
NSString* const kSsidUuid    = @"19B10001-E8F2-537E-4F6C-D104768A1214";
NSString* const kPassUuid    = @"19B10002-E8F2-537E-4F6C-D104768A1214";
NSString* const kCommandUuid = @"19B10003-E8F2-537E-4F6C-D104768A1214";
NSString* const kStatusUuid  = @"19B10004-E8F2-537E-4F6C-D104768A1214";
NSString* const kControlUuid = @"19B10005-E8F2-537E-4F6C-D104768A1214";

constexpr int kIdleExitSeconds = 20;

NSString* managerStateDescription (CBManagerState state)
{
    switch (state)
    {
        case CBManagerStatePoweredOn:    return @"Bluetooth ready";
        case CBManagerStatePoweredOff:   return @"Bluetooth is off on this Mac";
        case CBManagerStateResetting:    return @"Bluetooth is restarting";
        case CBManagerStateUnauthorized: return @"Bluetooth not permitted";
        case CBManagerStateUnsupported:  return @"Bluetooth not supported";
        case CBManagerStateUnknown:      return @"Bluetooth initialising";
    }
    return @"Bluetooth unknown";
}
}

@interface RecLightBLECentral : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, strong) CBCentralManager* manager;
@property (nonatomic, strong) CBPeripheral* peripheral;
@property (nonatomic, strong) CBCharacteristic* ssidCharacteristic;
@property (nonatomic, strong) CBCharacteristic* passwordCharacteristic;
@property (nonatomic, strong) CBCharacteristic* commandCharacteristic;
@property (nonatomic, strong) CBCharacteristic* statusCharacteristic;
@property (nonatomic, strong) CBCharacteristic* controlCharacteristic;
@property (nonatomic, assign) BOOL available;
@property (nonatomic, assign) BOOL connected;
@property (nonatomic, strong) NSString* statusText;
- (void) beginScan;
- (BOOL) sendCredentialsWithSsid: (NSString*) ssid password: (NSString*) password;
- (BOOL) sendResetCommand;
- (BOOL) sendControlText: (NSString*) text;
@end

@implementation RecLightBLECentral

- (instancetype) init
{
    self = [super init];
    if (self != nil)
    {
        _statusText = @"Bluetooth initialising";
        _manager = [[CBCentralManager alloc] initWithDelegate: self
                                                          queue: dispatch_get_main_queue()
                                                        options: nil];
    }
    return self;
}

- (void) beginScan
{
    if (self.manager.state != CBManagerStatePoweredOn)
        return;

    if (self.peripheral != nil && self.peripheral.state == CBPeripheralStateConnected)
        return;

    self.statusText = @"Scanning for RecLight";
    [self.manager stopScan];
    [self.manager scanForPeripheralsWithServices: @[ [CBUUID UUIDWithString: kServiceUuid] ]
                                          options: @{ CBCentralManagerScanOptionAllowDuplicatesKey: @NO }];
}

- (BOOL) isReadyForCredentials
{
    return self.peripheral != nil
        && self.peripheral.state == CBPeripheralStateConnected
        && self.ssidCharacteristic != nil
        && self.passwordCharacteristic != nil
        && self.commandCharacteristic != nil;
}

- (BOOL) isReadyForControl
{
    return self.peripheral != nil
        && self.peripheral.state == CBPeripheralStateConnected
        && self.controlCharacteristic != nil;
}

- (BOOL) sendCredentialsWithSsid: (NSString*) ssid password: (NSString*) password
{
    if (! [self isReadyForCredentials])
        return NO;

    [self.peripheral writeValue: [ssid dataUsingEncoding: NSUTF8StringEncoding]
              forCharacteristic: self.ssidCharacteristic
                           type: CBCharacteristicWriteWithResponse];
    [self.peripheral writeValue: [password dataUsingEncoding: NSUTF8StringEncoding]
              forCharacteristic: self.passwordCharacteristic
                           type: CBCharacteristicWriteWithResponse];
    [self.peripheral writeValue: [@"APPLY" dataUsingEncoding: NSUTF8StringEncoding]
              forCharacteristic: self.commandCharacteristic
                           type: CBCharacteristicWriteWithResponse];

    self.statusText = @"WiFi details sent";
    return YES;
}

- (BOOL) sendResetCommand
{
    if (! [self isReadyForCredentials])
        return NO;

    [self.peripheral writeValue: [@"RESET" dataUsingEncoding: NSUTF8StringEncoding]
              forCharacteristic: self.commandCharacteristic
                           type: CBCharacteristicWriteWithResponse];

    self.statusText = @"Network reset sent";
    return YES;
}

- (BOOL) sendControlText: (NSString*) text
{
    if (! [self isReadyForControl])
        return NO;

    auto type = (self.controlCharacteristic.properties & CBCharacteristicPropertyWriteWithoutResponse) != 0
        ? CBCharacteristicWriteWithoutResponse
        : CBCharacteristicWriteWithResponse;

    [self.peripheral writeValue: [text dataUsingEncoding: NSUTF8StringEncoding]
              forCharacteristic: self.controlCharacteristic
                           type: type];
    return YES;
}

- (void) centralManagerDidUpdateState: (CBCentralManager*) central
{
    self.available = (central.state == CBManagerStatePoweredOn);
    self.statusText = managerStateDescription (central.state);

    if (central.state == CBManagerStatePoweredOn)
        [self beginScan];
}

- (void) centralManager: (CBCentralManager*) central
   didDiscoverPeripheral: (CBPeripheral*) discoveredPeripheral
       advertisementData: (NSDictionary<NSString*, id>*) advertisementData
                    RSSI: (NSNumber*) RSSI
{
    (void) central; (void) advertisementData; (void) RSSI;

    if (self.peripheral != nil && self.peripheral != discoveredPeripheral)
        return;

    self.peripheral = discoveredPeripheral;
    self.peripheral.delegate = self;
    self.statusText = @"RecLight found, connecting";
    [self.manager stopScan];
    [self.manager connectPeripheral: self.peripheral options: nil];
}

- (void) centralManager: (CBCentralManager*) central didConnectPeripheral: (CBPeripheral*) connectedPeripheral
{
    (void) central;

    self.peripheral = connectedPeripheral;
    self.peripheral.delegate = self;
    self.connected = YES;
    self.statusText = @"Connected, reading services";
    [self.peripheral discoverServices: @[ [CBUUID UUIDWithString: kServiceUuid] ]];
}

- (void) centralManager: (CBCentralManager*) central
didDisconnectPeripheral: (CBPeripheral*) disconnectedPeripheral
                   error: (NSError*) error
{
    (void) central; (void) disconnectedPeripheral; (void) error;

    self.connected = NO;
    self.statusText = @"Disconnected, searching again";
    self.ssidCharacteristic = nil;
    self.passwordCharacteristic = nil;
    self.commandCharacteristic = nil;
    self.statusCharacteristic = nil;
    self.controlCharacteristic = nil;
    self.peripheral = nil;
    [self beginScan];
}

- (void) peripheral: (CBPeripheral*) discoveredPeripheral didDiscoverServices: (NSError*) error
{
    (void) error;

    for (CBService* service in discoveredPeripheral.services)
        if ([service.UUID isEqual: [CBUUID UUIDWithString: kServiceUuid]])
            [discoveredPeripheral discoverCharacteristics: @[ [CBUUID UUIDWithString: kSsidUuid],
                                                              [CBUUID UUIDWithString: kPassUuid],
                                                              [CBUUID UUIDWithString: kCommandUuid],
                                                              [CBUUID UUIDWithString: kStatusUuid],
                                                              [CBUUID UUIDWithString: kControlUuid] ]
                                                 forService: service];
}

- (void) peripheral: (CBPeripheral*) discoveredPeripheral
didDiscoverCharacteristicsForService: (CBService*) service
              error: (NSError*) error
{
    (void) service; (void) error;

    for (CBCharacteristic* characteristic in service.characteristics)
    {
        NSString* uuid = characteristic.UUID.UUIDString.uppercaseString;

        if ([uuid isEqualToString: kSsidUuid])
            self.ssidCharacteristic = characteristic;
        else if ([uuid isEqualToString: kPassUuid])
            self.passwordCharacteristic = characteristic;
        else if ([uuid isEqualToString: kCommandUuid])
            self.commandCharacteristic = characteristic;
        else if ([uuid isEqualToString: kStatusUuid])
        {
            self.statusCharacteristic = characteristic;
            [discoveredPeripheral setNotifyValue: YES forCharacteristic: characteristic];
            [discoveredPeripheral readValueForCharacteristic: characteristic];
        }
        else if ([uuid isEqualToString: kControlUuid])
            self.controlCharacteristic = characteristic;
    }

    self.statusText = [self isReadyForCredentials] ? @"Ready" : @"Connected, waiting for service";
}

- (void) peripheral: (CBPeripheral*) discoveredPeripheral
didUpdateValueForCharacteristic: (CBCharacteristic*) characteristic
              error: (NSError*) error
{
    (void) discoveredPeripheral; (void) error;

    if (characteristic != self.statusCharacteristic || characteristic.value == nil)
        return;

    NSString* value = [[NSString alloc] initWithData: characteristic.value encoding: NSUTF8StringEncoding];
    self.statusText = value != nil ? value : @"";
}

@end

namespace
{
int g_sock = -1;
RecLightBLECentral* g_central = nil;
time_t g_lastRequest = 0;

void sendReply (const struct sockaddr_in& to, NSString* text)
{
    const char* utf8 = [text UTF8String];
    sendto (g_sock, utf8, strlen (utf8), 0, (const struct sockaddr*) &to, sizeof (to));
}

NSString* base64Decode (NSString* b64)
{
    NSData* data = [[NSData alloc] initWithBase64EncodedString: b64 options: 0];
    if (data == nil)
        return @"";
    return [[NSString alloc] initWithData: data encoding: NSUTF8StringEncoding] ?: @"";
}

// Splits "a:b:c" (max 3 parts) the way the CREDS command needs it.
NSArray<NSString*>* splitOnce (NSString* s, NSInteger maxParts)
{
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    NSString* rest = s;
    while (parts.count + 1 < (NSUInteger) maxParts)
    {
        NSRange r = [rest rangeOfString: @":"];
        if (r.location == NSNotFound)
            break;
        [parts addObject: [rest substringToIndex: r.location]];
        rest = [rest substringFromIndex: r.location + 1];
    }
    [parts addObject: rest];
    return parts;
}

void handleDatagram (const char* data, ssize_t len, const struct sockaddr_in& from)
{
    g_lastRequest = time (nullptr);

    NSString* msg = [[NSString alloc] initWithBytes: data length: (NSUInteger) len encoding: NSUTF8StringEncoding];
    if (msg == nil)
        return;

    if ([msg isEqualToString: @"STATUS?"])
    {
        NSString* reply = [NSString stringWithFormat: @"STATUS:%d|%d|%@",
                            g_central.available ? 1 : 0, g_central.connected ? 1 : 0,
                            g_central.statusText ?: @""];
        sendReply (from, reply);
        return;
    }

    if ([msg hasPrefix: @"CREDS:"])
    {
        NSString* rest = [msg substringFromIndex: 6];
        NSArray<NSString*>* parts = splitOnce (rest, 2);
        if (parts.count == 2)
        {
            NSString* ssid = base64Decode (parts[0]);
            NSString* pass = base64Decode (parts[1]);
            [g_central sendCredentialsWithSsid: ssid password: pass];
        }
        return;
    }

    if ([msg isEqualToString: @"RESET"])
    {
        [g_central sendResetCommand];
        return;
    }

    if ([msg hasPrefix: @"CONTROL:"])
    {
        [g_central sendControlText: [msg substringFromIndex: 8]];
        return;
    }
}
}

int main (int, const char**)
{
    @autoreleasepool
    {
        g_sock = socket (AF_INET, SOCK_DGRAM, 0);
        if (g_sock < 0)
            return 1;

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons ((uint16_t) kRecLightBleHelperPort);
        addr.sin_addr.s_addr = inet_addr ("127.0.0.1");

        // Bind acts as our singleton lock: if another helper instance is
        // already running, this fails and we simply exit -- no harm done.
        if (bind (g_sock, (struct sockaddr*) &addr, sizeof (addr)) != 0)
        {
            close (g_sock);
            return 0;
        }

        int flags = fcntl (g_sock, F_GETFL, 0);
        fcntl (g_sock, F_SETFL, flags | O_NONBLOCK);

        g_central = [[RecLightBLECentral alloc] init];
        g_lastRequest = time (nullptr);

        dispatch_source_t readSource = dispatch_source_create (DISPATCH_SOURCE_TYPE_READ,
                                                                (uintptr_t) g_sock, 0,
                                                                dispatch_get_main_queue());
        dispatch_source_set_event_handler (readSource, ^{
            char buf[256];
            struct sockaddr_in from = {};
            socklen_t fromLen = sizeof (from);
            ssize_t n = recvfrom (g_sock, buf, sizeof (buf) - 1, 0,
                                  (struct sockaddr*) &from, &fromLen);
            if (n > 0)
            {
                buf[n] = '\0';
                handleDatagram (buf, n, from);
            }
        });
        dispatch_resume (readSource);

        // Exit on our own once no plugin instance has asked for anything in
        // a while -- keeps this process from lingering after every DAW quits.
        dispatch_source_t idleTimer = dispatch_source_create (DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                              dispatch_get_main_queue());
        dispatch_source_set_timer (idleTimer, dispatch_time (DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC),
                                   5LL * NSEC_PER_SEC, 1LL * NSEC_PER_SEC);
        dispatch_source_set_event_handler (idleTimer, ^{
            if (time (nullptr) - g_lastRequest > kIdleExitSeconds)
                exit (0);
        });
        dispatch_resume (idleTimer);

        dispatch_main();
    }
}
