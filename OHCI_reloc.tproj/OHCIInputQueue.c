/*
 * A fixed queue keeps controller progress independent of OPENSTEP's
 * synchronous EventSrc input callbacks.  Callers provide external locking.
 */
#include "OHCIInputQueue.h"
#include "OHCIMemory.h"

static void
copy_report(OHCIInputEvent *event, ohci_u8 type,
            const ohci_u8 *report, ohci_u32 length, ohci_u64 timestamp)
{
    if (length > OHCI_INPUT_REPORT_BYTES)
        length = OHCI_INPUT_REPORT_BYTES;
    event->type = type;
    event->length = (ohci_u8)length;
    bcopy(report, event->report, length);
    bzero(event->report + length, OHCI_INPUT_REPORT_BYTES - length);
    event->timestamp = timestamp;
}

static OHCIInputEvent *
newest_event(OHCIInputQueue *queue)
{
    ohci_u16 index;
    if (!queue || !queue->count)
        return 0;
    index = (ohci_u16)((queue->tail + OHCI_INPUT_QUEUE_CAPACITY - 1) %
                       OHCI_INPUT_QUEUE_CAPACITY);
    return &queue->events[index];
}

/* Match the historical PS/2 driver: while one pointer event is pending,
 * accumulate subsequent motion instead of replaying every stale packet.
 * A native PCPointerEvent has signed 8-bit deltas, so start another queue
 * segment rather than saturating or wrapping when the sum will not fit.
 * Button changes are queue boundaries and therefore remain ordered. */
static int
merge_pointer_report(OHCIInputEvent *event, const ohci_u8 *report,
                     ohci_u32 length, ohci_u64 timestamp)
{
    int dx;
    int dy;
    if (!event || event->type != OHCI_INPUT_POINTER ||
        event->length < 3 || length < 3)
        return 0;
    if ((event->report[0] & 3) != (report[0] & 3))
        return 0;
    dx = (int)(ohci_s8)event->report[1] + (int)(ohci_s8)report[1];
    dy = (int)(ohci_s8)event->report[2] + (int)(ohci_s8)report[2];
    if (dx < -128 || dx > 127 || dy < -128 || dy > 127)
        return 0;
    event->report[0] = report[0];
    event->report[1] = (ohci_u8)(ohci_s8)dx;
    event->report[2] = (ohci_u8)(ohci_s8)dy;
    event->length = 3;
    event->timestamp = timestamp;
    return 1;
}

static OHCIInputEvent *
newest_type(OHCIInputQueue *queue, ohci_u8 type)
{
    ohci_u16 offset;
    for (offset = 0; offset < queue->count; offset++) {
        ohci_u16 index = (ohci_u16)((queue->tail +
            OHCI_INPUT_QUEUE_CAPACITY - 1 - offset) %
            OHCI_INPUT_QUEUE_CAPACITY);
        if (queue->events[index].type == type)
            return &queue->events[index];
    }
    return 0;
}

void
OHCIInputQueueInitialize(OHCIInputQueue *queue)
{
    if (queue)
        bzero(queue, sizeof(*queue));
}

int
OHCIInputQueueEnqueue(OHCIInputQueue *queue, ohci_u8 type,
                      const ohci_u8 *report, ohci_u32 length,
                      ohci_u64 timestamp)
{
    OHCIInputEvent *event;
    if (!queue || !report || !length ||
        (type != OHCI_INPUT_KEYBOARD && type != OHCI_INPUT_POINTER))
        return 0;

    queue->submitted++;
    if (type == OHCI_INPUT_POINTER) {
        event = newest_event(queue);
        if (merge_pointer_report(event, report, length, timestamp)) {
            queue->coalesced++;
            return 1;
        }
    }
    if (queue->count < OHCI_INPUT_QUEUE_CAPACITY) {
        event = &queue->events[queue->tail];
        copy_report(event, type, report, length, timestamp);
        queue->tail = (ohci_u16)((queue->tail + 1) %
                                 OHCI_INPUT_QUEUE_CAPACITY);
        queue->count++;
        queue->enqueued++;
        if (queue->count > queue->highWater)
            queue->highWater = queue->count;
        return 1;
    }

    /* Pointer and keyboard have separate queues in the controller.  If the
     * pointer queue is pathologically full, discard its oldest segment so
     * the newest button state is retained. */
    if (type == OHCI_INPUT_POINTER) {
        if (queue->events[queue->head].type == OHCI_INPUT_POINTER) {
            queue->head = (ohci_u16)((queue->head + 1) %
                                     OHCI_INPUT_QUEUE_CAPACITY);
            queue->count--;
            queue->droppedPointer++;
            event = &queue->events[queue->tail];
            copy_report(event, type, report, length, timestamp);
            queue->tail = (ohci_u16)((queue->tail + 1) %
                                     OHCI_INPUT_QUEUE_CAPACITY);
            queue->count++;
            queue->enqueued++;
            return 0; /* Lost a transition: native caller must release input. */
        }
        queue->droppedPointer++;
        return 0;
    }

    /* Keyboard reports are complete state snapshots.  Replacing the newest
     * queued snapshot preserves final state, especially key releases. */
    event = newest_type(queue, OHCI_INPUT_KEYBOARD);
    if (event) {
        copy_report(event, type, report, length, timestamp);
        queue->coalesced++;
        queue->droppedKeyboard++;
        return 0;
    }

    /* Keyboard state has priority: a missed release can leave a key held.
     * A pointer report never evicts the only queued keyboard snapshot. */
    if (type == OHCI_INPUT_KEYBOARD) {
        event = &queue->events[(queue->tail +
            OHCI_INPUT_QUEUE_CAPACITY - 1) % OHCI_INPUT_QUEUE_CAPACITY];
        if (event->type == OHCI_INPUT_POINTER)
            queue->droppedPointer++;
        else
            queue->droppedKeyboard++;
        copy_report(event, type, report, length, timestamp);
        queue->coalesced++;
        return 0;
    }
    return 0;
}

int
OHCIInputQueueDequeue(OHCIInputQueue *queue, OHCIInputEvent *event)
{
    if (!queue || !event || !queue->count)
        return 0;
    *event = queue->events[queue->head];
    queue->head = (ohci_u16)((queue->head + 1) %
                             OHCI_INPUT_QUEUE_CAPACITY);
    queue->count--;
    queue->dequeued++;
    return 1;
}

void
OHCIInputQueueDiscardPending(OHCIInputQueue *queue)
{
    if (!queue)
        return;
    queue->head = queue->tail;
    queue->count = 0;
}

void
OHCIInputGapInitialize(OHCIInputGapTracker *tracker)
{
    if (tracker)
        bzero(tracker, sizeof(*tracker));
}

int
OHCIInputGapObserve(OHCIInputGapTracker *tracker, ohci_u64 timestamp,
                    ohci_u64 *gapNanoseconds)
{
    ohci_u64 delta;
    if (gapNanoseconds)
        *gapNanoseconds = 0;
    if (!tracker || !timestamp)
        return 0;
    if (!tracker->lastTimestamp || timestamp <= tracker->lastTimestamp) {
        tracker->lastTimestamp = timestamp;
        tracker->consecutiveActive = 0;
        return 0;
    }
    delta = timestamp - tracker->lastTimestamp;
    tracker->lastTimestamp = timestamp;
    if (delta <= OHCI_INPUT_ACTIVE_NS) {
        if (tracker->consecutiveActive < OHCI_INPUT_GAP_ARM_REPORTS)
            tracker->consecutiveActive++;
        return 0;
    }
    if (delta >= OHCI_INPUT_GAP_NS &&
        tracker->consecutiveActive >= OHCI_INPUT_GAP_ARM_REPORTS) {
        tracker->consecutiveActive = 0;
        tracker->detected++;
        if (gapNanoseconds)
            *gapNanoseconds = delta;
        return 1;
    }
    tracker->consecutiveActive = 0;
    return 0;
}
