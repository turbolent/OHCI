#ifndef OHCI_PCI_H
#define OHCI_PCI_H
#include "OHCIRegs.h"
typedef struct OHCIPCIOps {
    int (*read)(void *, unsigned, ohci_u32 *);
    int (*write)(void *, unsigned, ohci_u32);
} OHCIPCIOps;
int OHCIPCICommand(const OHCIPCIOps *, void *, unsigned set, unsigned clear);
int OHCIPCIDisableMessages(const OHCIPCIOps *, void *);
int OHCIPCIBar(const OHCIPCIOps *, void *, unsigned *address, unsigned *bytes);
int OHCIPCIMapRange(unsigned address, unsigned bytes, unsigned pageSize,
                    unsigned *start, unsigned *length, unsigned *offset);
#endif
