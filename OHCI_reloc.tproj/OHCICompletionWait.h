#ifndef OHCI_COMPLETION_WAIT_H
#define OHCI_COMPLETION_WAIT_H

/* lock covers both publishers, in event-lock then boundary-lock order.
 * prepare registers a one-tick wait; block tolerates wake-before-block. */
typedef struct OHCICompletionWaitOps {
    void (*lock)(void *);
    void (*unlock)(void *);
    int (*ready)(void *);
    void (*prepare)(void *);
    void (*block)(void *);
} OHCICompletionWaitOps;

int OHCICompletionWorkReady(unsigned started, unsigned work, unsigned stopping,
    unsigned fatal, unsigned rescan, unsigned scheduleBusy);
void OHCICompletionWait(void *, const OHCICompletionWaitOps *);

#endif
