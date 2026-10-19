#ifndef USB_CORE_H
#define USB_CORE_H

#include "OHCITypes.h"

#define USB_DIR_IN                  0x80
#define USB_DIR_OUT                 0x00
#define USB_TYPE_STANDARD           0x00
#define USB_TYPE_CLASS              0x20
#define USB_RECIP_DEVICE            0x00
#define USB_RECIP_INTERFACE         0x01

#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_CONFIG          0x09
#define USB_REQ_SET_INTERFACE       0x0b

#define USB_DESC_DEVICE             1
#define USB_DESC_CONFIG             2
#define USB_DESC_INTERFACE          4
#define USB_DESC_ENDPOINT           5

#define USB_DEVICE_DESCRIPTOR_BYTES 18
#define USB_CONFIG_HEADER_BYTES     9
#define USB_MAX_CONFIG_DESCRIPTOR   4096

#define USB_ENUM_STAGE_NONE         0
#define USB_ENUM_STAGE_DEVICE_8     1
#define USB_ENUM_STAGE_EP0_UPDATE   2
#define USB_ENUM_STAGE_DEVICE_FULL  3
#define USB_ENUM_STAGE_CONFIG_9     4
#define USB_ENUM_STAGE_CONFIG_FULL  5
#define USB_ENUM_STAGE_COMPLETE     6

#define USB_ENUM_ERROR_NONE         0
#define USB_ENUM_ERROR_TRANSFER     1
#define USB_ENUM_ERROR_SHORT        2
#define USB_ENUM_ERROR_INVALID      3
#define USB_ENUM_ERROR_EP0_UPDATE   4

#define USB_DESCRIPTOR_END          0
#define USB_DESCRIPTOR_FOUND        1
#define USB_DESCRIPTOR_MALFORMED   -1

typedef struct USBSetupPacket {
    ohci_u8 requestType;
    ohci_u8 request;
    ohci_u16 value;
    ohci_u16 index;
    ohci_u16 length;
} USBSetupPacket;

typedef int (*USBControlTransferFunction)(void *hostContext,
                                           void *deviceContext,
                                           const USBSetupPacket *setup,
                                           void *bytes,
                                           ohci_u16 *actualLength);
typedef int (*USBUpdateEndpointZeroFunction)(void *hostContext,
                                              void *deviceContext,
                                              ohci_u8 maxPacket);

typedef struct USBTransportOperations {
    USBControlTransferFunction controlTransfer;
    USBUpdateEndpointZeroFunction updateEndpointZero;
} USBTransportOperations;

typedef struct USBCoreDevice {
    const USBTransportOperations *operations;
    void *hostContext;
    void *deviceContext;
} USBCoreDevice;

typedef struct USBEnumerationData {
    ohci_u8 deviceDescriptor[USB_DEVICE_DESCRIPTOR_BYTES];
    ohci_u8 configurationDescriptor[USB_MAX_CONFIG_DESCRIPTOR];
    ohci_u16 configurationLength;
    ohci_u16 vendorID;
    ohci_u16 productID;
    ohci_u8 endpointZeroMaxPacket;
    ohci_u8 configurationValue;
    ohci_u8 configurationIndex;
    ohci_u8 numberConfigurations;
    ohci_u8 stage;
    ohci_u8 error;
    ohci_u16 expectedLength;
    ohci_u16 actualLength;
} USBEnumerationData;

typedef struct USBDescriptorIterator {
    const ohci_u8 *bytes;
    ohci_u16 length;
    ohci_u16 offset;
} USBDescriptorIterator;

void USBCoreDeviceInitialize(USBCoreDevice *device,
                             const USBTransportOperations *operations,
                             void *hostContext, void *deviceContext);
int USBCoreControlTransfer(USBCoreDevice *device,
                           const USBSetupPacket *setup, void *bytes,
                           ohci_u16 *actualLength);
int USBCoreGetDescriptor(USBCoreDevice *device, ohci_u8 type,
                         ohci_u8 index, void *bytes, ohci_u16 length,
                         ohci_u16 *actualLength);
int USBCoreSetConfiguration(USBCoreDevice *device, ohci_u8 value);
int USBCoreSetInterface(USBCoreDevice *device, ohci_u8 interfaceNumber,
                        ohci_u8 alternateSetting);
int USBCoreEnumerateDevice(USBCoreDevice *device,
                           USBEnumerationData *result);
int USBCoreReadDevice(USBCoreDevice *, USBEnumerationData *);
int USBCoreReadConfiguration(USBCoreDevice *, USBEnumerationData *, ohci_u8);
/* Called for every successfully fetched configuration; higher positive rank
 * wins, zero means unsupported, negative means malformed. No USB mutations.
 * Return 1 selected, 0 none, -1 incomplete/invalid scan. */
typedef int (*USBConfigurationRank)(void *, const USBEnumerationData *);
int USBCoreSelectConfiguration(USBCoreDevice *, USBEnumerationData *,
                                USBConfigurationRank, void *);
const char *USBCoreEnumerationStageName(ohci_u8 stage);
const char *USBCoreEnumerationErrorName(ohci_u8 error);
ohci_u16 USBCoreReadLE16(const ohci_u8 *bytes);
void USBCoreDescriptorIteratorInitialize(USBDescriptorIterator *iterator,
                                         const ohci_u8 *bytes,
                                         ohci_u16 length);
int USBCoreDescriptorNext(USBDescriptorIterator *iterator,
                          const ohci_u8 **descriptor,
                          ohci_u8 *descriptorLength,
                          ohci_u8 *descriptorType);

#endif
