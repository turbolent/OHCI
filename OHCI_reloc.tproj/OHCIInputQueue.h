#ifndef OHCI_INPUT_QUEUE_H
#define OHCI_INPUT_QUEUE_H

#include "OHCITypes.h"

#define OHCI_INPUT_REPORT_BYTES   8
#define OHCI_INPUT_QUEUE_CAPACITY 128

/* A boot mouse normally reports every 8-10 ms while moving.  Require a
 * sustained stream before classifying a later delay.  A HID endpoint cannot
 * distinguish an actual traffic stall from the user stopping the mouse, so
 * resulting gaps are meaningful only during deliberately continuous motion. */
#define OHCI_INPUT_ACTIVE_NS        25000000ULL
#define OHCI_INPUT_GAP_NS           50000000ULL
#define OHCI_INPUT_GAP_ARM_REPORTS  16U

#define OHCI_INPUT_KEYBOARD 1
#define OHCI_INPUT_POINTER  2

typedef struct OHCIInputEvent {
    ohci_u8 type;
    ohci_u8 length;
    ohci_u8 report[OHCI_INPUT_REPORT_BYTES];
    ohci_u64 timestamp;
} OHCIInputEvent;

typedef struct OHCIInputGapTracker {
    ohci_u64 lastTimestamp;
    ohci_u16 consecutiveActive;
    ohci_u32 detected;
} OHCIInputGapTracker;

typedef struct OHCIInputQueue {
    OHCIInputEvent events[OHCI_INPUT_QUEUE_CAPACITY];
    ohci_u16 head;
    ohci_u16 tail;
    ohci_u16 count;
    ohci_u16 highWater;
    ohci_u32 submitted;
    ohci_u32 enqueued;
    ohci_u32 dequeued;
    ohci_u32 coalesced;
    ohci_u32 droppedKeyboard;
    ohci_u32 droppedPointer;
} OHCIInputQueue;

/* Adjacent pointer reports with the same native button state are accumulated
 * while their signed 8-bit X/Y sum fits one PCPointer event.  A button edge or
 * an overflowing motion sum starts another ordered queue segment. Enqueue
 * returns zero if a report was dropped/replaced: the native caller must
 * discard pending input and release held keys/buttons before dispatching. */
void OHCIInputQueueInitialize(OHCIInputQueue *queue);
int OHCIInputQueueEnqueue(OHCIInputQueue *queue, ohci_u8 type,
                          const ohci_u8 *report, ohci_u32 length,
                          ohci_u64 timestamp);
int OHCIInputQueueDequeue(OHCIInputQueue *queue, OHCIInputEvent *event);
void OHCIInputQueueDiscardPending(OHCIInputQueue *queue);
void OHCIInputGapInitialize(OHCIInputGapTracker *tracker);
int OHCIInputGapObserve(OHCIInputGapTracker *tracker, ohci_u64 timestamp,
                        ohci_u64 *gapNanoseconds);

#endif
