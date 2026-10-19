#ifndef OHCI_CONTROLLER_H
#define OHCI_CONTROLLER_H
#import <driverkit/IODirectDevice.h>
#import <machkit/NXLock.h>
#include "OHCICore.h"
#include "OHCIInterruptState.h"
#include "OHCIPCI.h"
#include "OHCIInputQueue.h"
@class OHCIUSBKeyboard;
@class OHCIUSBPointer;
@interface OHCIController : IODirectDevice
{
@public
    OHCIControllerState _state;
    OHCIInterruptState _interruptState;
    id _pciDescription;
    NXLock *_eventLock,*_boundaryLock,*_inputLocks[2];
    vm_address_t _mappedAddress;
    unsigned _mappedBytes,_irq,_pciOwned,_contained,_workersStarted,_startupReady;
    BOOL _deferIOThread,_ioThreadStarted,_initializationFailed;
    unsigned _serviceEvent,_inputReady,_releasePending[2];
    unsigned _pendingLEDs,_ledUpdatePending;
    OHCIInputQueue _inputQueues[2];
    OHCIUSBKeyboard *_keyboard;
    OHCIUSBPointer *_pointer;
}
+ (BOOL)probe:description;
- initFromDeviceDescription:description;
- failInitializationAt:(unsigned)line;
- (void)runCompletionLoop;
- (void)runManagementLoop;
- (void)runRetryLoop;
- (void)runInputLoop:(unsigned)which;
- (void)containFatalController;
- (void)handleKeyboardReport:(const unsigned char *)report length:(unsigned)length;
- (void)handlePointerReport:(const unsigned char *)report length:(unsigned)length;
- (void)queueKeyboardLEDs:(unsigned char)leds;
@end
#endif
