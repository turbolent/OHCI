#ifndef OHCI_USB_POINTER_H
#define OHCI_USB_POINTER_H

#import "OpenStepInputCompat.h"

@class OHCIController;

@interface OHCIUSBPointer : PCPointer
{
@private
    OHCIController *_controller;
}

+ (BOOL)attachToController:(OHCIController *)controller
               description:deviceDescription;
- (BOOL)mouseInit:deviceDescription;
- (BOOL)setEventTarget:eventTarget;
- (void)consumeReport:(const unsigned char *)report length:(unsigned)length;

@end

#endif
