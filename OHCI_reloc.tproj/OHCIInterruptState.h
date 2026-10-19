#ifndef OHCI_INTERRUPT_STATE_H
#define OHCI_INTERRUPT_STATE_H
#include "OHCIRegs.h"
#define OHCI_MODE_INVALID 0U
#define OHCI_MODE_INTX 1U
#define OHCI_MODE_POLLING 2U
typedef struct OHCIInterruptOps {
    ohci_u32 (*read)(void *, unsigned);
    int (*write)(void *, unsigned, ohci_u32);
    int (*pciRead)(void *, ohci_u32 *);
    int (*pciWrite)(void *, ohci_u32);
    int (*rearm)(void *);
    void (*publish)(void *);
} OHCIInterruptOps;
typedef struct OHCIInterruptState {
    unsigned mode, participant, active, generation;
    volatile unsigned stopping, admitted;
    unsigned mmioLost, interruptDebt, dmaDebt, rearmDebt;
    unsigned fenced, dmaFenced, pending, work;
    unsigned callbacks, foreign, owned, retries, failures, services;
    unsigned watchdogPending;
    ohci_u64 watchdogSince;
    const OHCIInterruptOps *ops;
    void *context;
} OHCIInterruptState;
unsigned OHCIInterruptModeParse(const char *);
/* All functions below require the native short MMIO/PCI/rearm lock. */
void OHCIInterruptLost(OHCIInterruptState *);
int OHCIInterruptFence(OHCIInterruptState *, int dma);
void OHCIInterruptCallback(OHCIInterruptState *);
void OHCIInterruptRetry(OHCIInterruptState *);
void OHCIInterruptWatchdog(OHCIInterruptState *, ohci_u64 now);
unsigned OHCIInterruptTakeWork(OHCIInterruptState *);
int OHCIInterruptRestore(OHCIInterruptState *);
void OHCIInterruptPoll(OHCIInterruptState *);
void OHCIInterruptStop(OHCIInterruptState *);
#endif
