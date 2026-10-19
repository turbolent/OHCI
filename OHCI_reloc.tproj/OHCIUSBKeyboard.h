#ifndef OHCI_USB_KEYBOARD_H
#define OHCI_USB_KEYBOARD_H

#import <driverkit/IODevice.h>
#import "OpenStepInputCompat.h"

@class OHCIController;

@interface OHCIUSBKeyboard : IODevice
{
@private
    OHCIController *_controller;
    id _owner;
    id _desiredOwner;
    id _ownerLock;
    unsigned char _previous[8];
}

- initWithController:(OHCIController *)controller;
- (void)consumeReport:(const unsigned char *)report length:(unsigned)length;
- (void)setAlphaLockFeedback:(BOOL)locked;
- (int)interfaceId;
- (int)handlerId;
- (IOReturn)becomeOwner:client;
- (IOReturn)relinquishOwnership:client;
- (IOReturn)desireOwnership:client;

@end

#endif
