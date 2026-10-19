#include "OHCIInterruptState.h"
#include <string.h>
unsigned OHCIInterruptModeParse(const char *s)
{
    if (!s || !strcmp(s, "INTx")) return OHCI_MODE_INTX;
    if (!strcmp(s, "Polling")) return OHCI_MODE_POLLING;
    return OHCI_MODE_INVALID;
}
void OHCIInterruptLost(OHCIInterruptState *s)
{
    if (!s->mmioLost) s->generation++;
    s->mmioLost = 1; s->stopping = 1; s->active = 0;
    s->interruptDebt = s->dmaDebt = 1;
    s->fenced = s->dmaFenced = 0;
    s->work = 1; s->ops->publish(s->context);
}
static ohci_u32 rd(OHCIInterruptState *s, unsigned reg)
{
    ohci_u32 v;
    if (s->mmioLost) return 0xffffffffU;
    v = s->ops->read(s->context, reg);
    if (v == 0xffffffffU) OHCIInterruptLost(s);
    return v;
}
static int wr(OHCIInterruptState *s, unsigned reg, ohci_u32 v)
{
    if (s->mmioLost) return 0;
    if (!s->ops->write(s->context, reg, v)) { OHCIInterruptLost(s); return 0; }
    return 1;
}
static void acknowledge(OHCIInterruptState *s, unsigned causes)
{
    causes &= ~OHCI_INT_WDH;
    if (wr(s, OHCI_STS, causes) && rd(s, OHCI_STS) != 0xffffffffU) {
        /* A new pending bit may appear before the next watchdog sample.
         * Completing an acknowledgement is progress even if that sample
         * never observes the brief quiet/fenced interval between IRQs. */
        s->watchdogPending = 0;
    }
}
int OHCIInterruptFence(OHCIInterruptState *s, int dma)
{
    ohci_u32 before, desired, after;
    s->interruptDebt = 1; s->fenced = 0;
    if (dma) { s->dmaDebt = 1; s->dmaFenced = 0; }
    if (!s->ops->pciRead(s->context, &before) || (before & 0xffff) == 0xffff) return 0;
    desired = (before & 0xffff) | OHCI_PCI_INT_DISABLE;
    if (dma) desired &= ~OHCI_PCI_MASTER;
    if (!s->ops->pciWrite(s->context, desired) ||
        !s->ops->pciRead(s->context, &after) || (after & 0xffff) == 0xffff ||
        (after & 0xffff) != desired) return 0;
    s->fenced = 1; s->interruptDebt = 0; s->active = 0;
    if (dma) { s->dmaDebt = 0; s->dmaFenced = 1; }
    return 1;
}
static int rearm(OHCIInterruptState *s, int freshForeign)
{
    if (!s->participant || !s->rearmDebt) return 1;
    if ((!freshForeign || s->mmioLost || s->stopping) && !s->fenced) return 0;
    if (!s->ops->rearm(s->context)) { s->failures++; return 0; }
    s->rearmDebt = 0;
    return 1;
}
static void publish(OHCIInterruptState *s, unsigned cause)
{
    s->pending |= cause; s->work = 1;
    s->ops->publish(s->context);
}
static void examine(OHCIInterruptState *s)
{
    unsigned status, enabled, causes;
    if (s->stopping || s->mmioLost) {
        /* Resolve IRQ proof independently: a BM-clear failure must not hold
         * an otherwise contained shared interrupt vote forever. */
        OHCIInterruptFence(s, 0);
        rearm(s, 0);
        return;
    }
    if (s->fenced) { rearm(s, 0); return; }
    status = rd(s, OHCI_STS); enabled = rd(s, OHCI_INTR);
    if (s->mmioLost) { OHCIInterruptFence(s, 0); rearm(s, 0); return; }
    causes = (enabled & OHCI_INT_MIE) ? status & enabled & OHCI_STS_W1C : 0;
    if (!causes) { s->foreign++; rearm(s, 1); return; }
    s->owned++;
    /* A verified function fence precedes acknowledgement and shared IRQ rearm.
     * A verified PCI fence is the proof; W1C acknowledges observed bits only. */
    OHCIInterruptFence(s, 0);
    wr(s, OHCI_INTR_DISABLE, 0xffffffffU);
    acknowledge(s, causes);
    publish(s, causes);
    if (s->mmioLost && !s->fenced) OHCIInterruptFence(s, 0);
    rearm(s, 0);
}
void OHCIInterruptCallback(OHCIInterruptState *s)
{
    if (!s->participant) return;
    s->callbacks++; s->rearmDebt = 1;
    examine(s);
}
void OHCIInterruptRetry(OHCIInterruptState *s)
{
    s->retries++;
    if (s->interruptDebt) OHCIInterruptFence(s, 0);
    if (s->rearmDebt) examine(s);
    if (s->dmaDebt) {
        /* Keep the previously established INTx proof if the independent DMA
         * transition fails. Re-establish it from current PCI readback. */
        if (!OHCIInterruptFence(s, 1)) OHCIInterruptFence(s, 0);
    }
}
unsigned OHCIInterruptTakeWork(OHCIInterruptState *s)
{
    unsigned pending = s->pending;
    s->pending = 0; s->work = 0; s->services++;
    return pending;
}
void OHCIInterruptWatchdog(OHCIInterruptState *s, ohci_u64 now)
{
    unsigned status;
    if (s->mode != OHCI_MODE_INTX || !s->active || s->stopping) {
        s->watchdogPending = 0; return;
    }
    status = rd(s, OHCI_STS);
    if (s->mmioLost) return;
    if (!(status & OHCI_INTR_DEFAULT)) { s->watchdogPending = 0; return; }
    if (!s->watchdogPending || now < s->watchdogSince) {
        s->watchdogSince = now; s->watchdogPending = 1;
    } else if (now - s->watchdogSince >= 100) OHCIInterruptStop(s);
}
int OHCIInterruptRestore(OHCIInterruptState *s)
{
    ohci_u32 before, after, desired;
    unsigned status, generation = s->generation;
    if (s->mode != OHCI_MODE_INTX || !s->participant || s->stopping ||
        s->mmioLost || s->admitted || s->work || s->interruptDebt || s->dmaDebt) return 0;
    if (!s->fenced && !OHCIInterruptFence(s, 0)) return 0;
    if (!rearm(s, 0)) return 0;
    status = rd(s, OHCI_STS);
    if (s->mmioLost) return 0;
    if (status & OHCI_INTR_DEFAULT) {
        acknowledge(s, status & OHCI_INTR_DEFAULT);
        publish(s, status & OHCI_INTR_DEFAULT); return 0;
    }
    if (!wr(s, OHCI_INTR, OHCI_INTR_DEFAULT|OHCI_INT_MIE) || rd(s, OHCI_INTR) != (OHCI_INTR_DEFAULT|OHCI_INT_MIE) ||
        generation != s->generation || s->mmioLost) goto failed;
    if (!s->ops->pciRead(s->context, &before) || (before & 0xffff) == 0xffff) goto failed;
    desired = (before & 0xffff) & ~OHCI_PCI_INT_DISABLE;
    if (!(desired & OHCI_PCI_MASTER) || !(desired & OHCI_PCI_MEMORY) ||
        !s->ops->pciWrite(s->context, desired) || !s->ops->pciRead(s->context, &after) ||
        (after & 0xffff) != desired || generation != s->generation) goto failed;
    s->active = 1; s->fenced = 0;
    return 1;
failed:
    s->stopping = 1; s->dmaDebt = 1;
    OHCIInterruptFence(s, 0); publish(s, 0);
    return 0;
}
void OHCIInterruptPoll(OHCIInterruptState *s)
{
    unsigned status;
    if (s->mode != OHCI_MODE_POLLING || s->stopping || s->mmioLost) return;
    status = rd(s, OHCI_STS);
    if (s->mmioLost) return;
    if (status & OHCI_STS_W1C) {
        acknowledge(s, status & OHCI_STS_W1C);
    }
    publish(s, status & OHCI_INTR_DEFAULT);
}
void OHCIInterruptStop(OHCIInterruptState *s)
{
    s->stopping = 1; s->active = 0; s->generation++;
    s->interruptDebt = s->dmaDebt = 1;
    OHCIInterruptFence(s, 0); rearm(s, 0);
    if (!OHCIInterruptFence(s, 1)) OHCIInterruptFence(s, 0);
    publish(s, 0);
}
