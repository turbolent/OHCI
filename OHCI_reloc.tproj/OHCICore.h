#ifndef OHCI_CORE_H
#define OHCI_CORE_H
#include "OHCIRegs.h"
#include "USBHID.h"
#define OHCI_ENDPOINTS 3U
#define OHCI_TDS 4U
typedef struct OHCIEndpoint {
    OHCIDMA descriptors, data, setup;
    OHCIED *ed;
    OHCITD *td;
    unsigned configured, periodic, linked, busy, retiring, role;
    unsigned maxPacket, endpoint, interval, phase, load, dummy;
    unsigned count, completed, actual, expected, generation;
    unsigned tdOwned[OHCI_TDS], tdLength[OHCI_TDS], tdStart[OHCI_TDS];
    unsigned tdStage[OHCI_TDS];
    int result;
} OHCIEndpoint;
typedef struct OHCIDevice {
    unsigned used, ready, generation, address, port, speed, parent, parentPort;
    unsigned hub, hubPorts, hubCharacteristics, powerMA, selfPowerCapable;
    unsigned selfPowered, externalPorts, nonRemovable, hubChanges;
    unsigned portPowerMA[OHCI_MAX_HUB_PORTS+1];
    unsigned blockedPorts, disconnecting, configurationValue, controlBusy;
    unsigned roles, interfaceNumber[2];
    USBHIDInterface hid[2];
    USBCoreDevice usb;
    OHCIEndpoint endpoints[OHCI_ENDPOINTS];
} OHCIDevice;
typedef struct OHCIEnumerationFailure {
    const char *stage;
    unsigned rootPort, hubPort, speed, vendor, product;
} OHCIEnumerationFailure;
typedef struct OHCIControllerState {
    volatile ohci_u8 *mmio;
    unsigned mmioBytes, maxPorts, initialized, running, registersValid;
    unsigned fatal, dmaArmed, haltConfirmed, scheduleBusy, rescan, enumerating;
    unsigned addresses[4], nextGeneration, blockedPorts, disconnected;
    unsigned keyboardSlot, pointerSlot, rootPortsScanned, rootPortStatus[OHCI_MAX_PORTS];
    unsigned frameLoad[32], inputEnabled;
    unsigned keyboardReports, mouseReports, errors, timeouts, transferEvents;
    unsigned scheduleChanges, allocations, lastCommand, lastStatus;
    unsigned waitOffset, waitMask, waitExpected, waitObserved, faultDetail;
    const char *faultReason;
    OHCIDMA hcca, anchors;
    OHCIDevice devices[OHCI_MAX_DEVICES];
    OHCIEnumerationFailure enumerationFailure;
    void *owner;
} OHCIControllerState;
ohci_u32 OHCIPlatformRead(OHCIControllerState *, ohci_u32);
int OHCIPlatformWrite(OHCIControllerState *, ohci_u32, ohci_u32);
ohci_u32 OHCIPlatformTakeDone(OHCIControllerState *);
void *OHCIPlatformAllocate(ohci_u32);
void OHCIPlatformFree(void *, ohci_u32);
ohci_u32 OHCIPlatformPhysical(void *);
ohci_u64 OHCIPlatformMilliseconds(void);
void OHCIPlatformPause(OHCIControllerState *, unsigned);
void OHCIPlatformDelay(OHCIControllerState *, unsigned);
void OHCIPlatformWake(void *);
void OHCIPlatformKeyboardReport(void *, const ohci_u8 *, ohci_u32);
void OHCIPlatformPointerReport(void *, const ohci_u8 *, ohci_u32);
#ifdef KERNEL
#import <driverkit/generalFuncs.h>
#define OHCIPlatformLog IOLog
#else
void OHCIPlatformLog(const char *, ...);
#endif
int OHCICoreSupportsPCIClass(ohci_u32);
int OHCICoreInitialize(OHCIControllerState *);
int OHCICoreStart(OHCIControllerState *);
int OHCICoreService(OHCIControllerState *, unsigned);
int OHCICoreServicePorts(OHCIControllerState *);
int OHCICoreSetKeyboardLEDs(OHCIControllerState *, ohci_u8);
int OHCICoreConfigureEndpoint(OHCIControllerState *, OHCIDevice *, unsigned, unsigned, unsigned, unsigned);
int OHCICoreCancel(OHCIControllerState *, OHCIDevice *, OHCIEndpoint *);
void OHCICoreDisconnect(OHCIControllerState *, unsigned);
void OHCICoreFail(OHCIControllerState *, const char *, unsigned);
int OHCICoreReleaseDMA(OHCIControllerState *, int);
#endif
