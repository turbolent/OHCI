#ifndef OHCI_TYPES_H
#define OHCI_TYPES_H
#include "OHCIUSBNames.h"
typedef unsigned char ohci_u8;
typedef unsigned short ohci_u16;
typedef unsigned int ohci_u32;
typedef unsigned long long ohci_u64;
typedef signed char ohci_s8;
typedef char ohci_word_is_32_bits[sizeof(ohci_u32) == 4 ? 1 : -1];
#define OHCI_PAGE_SIZE 4096U
#define OHCI_MAX_DEVICES 16U
#define OHCI_MAX_PORTS 15U
#define OHCI_MAX_HUB_PORTS 15U
#define OHCI_TIMEOUT_MS 5000U
typedef struct OHCIDMA {
    void *allocation;
    void *virtualAddress;
    ohci_u32 physicalAddress;
    ohci_u32 allocationBytes;
    ohci_u32 bytes;
} OHCIDMA;
#endif
