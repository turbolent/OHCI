#ifndef OHCI_USB_HID_H
#define OHCI_USB_HID_H

#include "USBCore.h"

#define USB_HID_SET_REPORT      0x09
#define USB_HID_SET_IDLE        0x0a
#define USB_HID_SET_PROTOCOL    0x0b

#define USB_CLASS_HID           3
#define USB_HID_PROTOCOL_KEYBOARD 1
#define USB_HID_PROTOCOL_MOUSE  2

typedef struct USBHIDInterface {
    ohci_u8 configurationValue;
    ohci_u8 interfaceNumber;
    ohci_u8 alternateSetting;
    ohci_u8 protocol;
    ohci_u8 endpointAddress;
    ohci_u8 interval;
    ohci_u16 maxPacket;
} USBHIDInterface;

int USBHIDFindBootInterface(const ohci_u8 *bytes, ohci_u16 length,
                            USBHIDInterface *result);
/* Returns keyboard/mouse bits (1/2), or -1 for malformed descriptors. */
int USBHIDFindBootInterfaces(const ohci_u8 *, ohci_u16, unsigned lowSpeed,
                              USBHIDInterface result[2]);
int USBHIDSetBootProtocol(USBCoreDevice *device,
                          ohci_u8 interfaceNumber);
int USBHIDSetIdle(USBCoreDevice *device, ohci_u8 interfaceNumber);
int USBHIDSetKeyboardLEDs(USBCoreDevice *device,
                          ohci_u8 interfaceNumber, ohci_u8 leds);
unsigned USBHIDUsageToPCKey(ohci_u8 usage);
typedef void (*USBHIDKeyChange)(void *, unsigned usage, int down);
void USBHIDKeyboardChanges(ohci_u8 previous[8],const ohci_u8 *,unsigned,
                            USBHIDKeyChange,void *);
int USBHIDBootMouseReportChanged(const ohci_u8 *report, ohci_u32 length,
                                 ohci_u8 previousButtons);

#endif
