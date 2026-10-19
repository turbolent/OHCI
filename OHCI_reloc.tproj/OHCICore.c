/* OHCI 1.0a control and periodic engine. SPDX-License-Identifier: BSD-2-Clause */
#include "OHCICore.h"
#include "OHCIMemory.h"
static void barrier(void) { __asm__ volatile("" : : : "memory"); }
void OHCICoreFail(OHCIControllerState *c, const char *why, unsigned detail)
{
    if (!c->faultReason) {
        c->faultReason=why; c->faultDetail=detail;
        OHCIPlatformLog("OHCI: offline: %s (%x)\n",why,detail);
    }
    c->fatal=1;
}
static unsigned read_op(OHCIControllerState *c, unsigned r)
{
    unsigned v;
    if (c->fatal) return 0xffffffffU;
    v=OHCIPlatformRead(c,r);
    if (v==0xffffffffU) OHCICoreFail(c,"MMIO read",r);
    if (r==OHCI_CONTROL) c->lastCommand=v;
    if (r==OHCI_STS) c->lastStatus=v;
    if (r>=OHCI_PORT(1) && r<=OHCI_PORT(c->maxPorts)) c->rootPortStatus[(r-OHCI_PORT(1))/4]=v;
    return v;
}
static int write_op(OHCIControllerState *c, unsigned r, unsigned v)
{
    if (c->fatal) return 0;
    if (!OHCIPlatformWrite(c,r,v)) { OHCICoreFail(c,"MMIO write",r); return 0; }
    return 1;
}
static int wait_bits(OHCIControllerState *c,unsigned r,unsigned mask,unsigned expected,unsigned ms)
{
    ohci_u64 end=OHCIPlatformMilliseconds()+ms;
    for (;;) {
        unsigned v=read_op(c,r);
        if (c->fatal) return 0;
        if ((v&mask)==expected) return 1;
        if (OHCIPlatformMilliseconds()>=end) {
            c->waitOffset=r; c->waitMask=mask; c->waitExpected=expected; c->waitObserved=v;
            c->timeouts++; return 0;
        }
        OHCIPlatformPause(c,1);
    }
}
static int dma_alloc(OHCIControllerState *c,OHCIDMA *d,unsigned bytes)
{
    size_t aligned;
    if (!bytes || bytes>OHCI_PAGE_SIZE) return 0;
    bzero(d,sizeof(*d)); d->allocationBytes=bytes+OHCI_PAGE_SIZE;
    d->allocation=OHCIPlatformAllocate(d->allocationBytes);
    if (!d->allocation) return 0;
    aligned=((size_t)d->allocation+OHCI_PAGE_SIZE-1)&~(size_t)(OHCI_PAGE_SIZE-1);
    d->virtualAddress=(void *)aligned; d->bytes=bytes;
    d->physicalAddress=OHCIPlatformPhysical(d->virtualAddress);
    if (!d->physicalAddress || (d->physicalAddress&(OHCI_PAGE_SIZE-1)) ||
        d->physicalAddress>0xffffffffU-(bytes-1) ||
        OHCIPlatformPhysical((ohci_u8 *)d->virtualAddress+bytes-1)!=d->physicalAddress+bytes-1) {
        OHCIPlatformFree(d->allocation,d->allocationBytes); bzero(d,sizeof(*d)); return 0;
    }
    bzero(d->virtualAddress,bytes); c->allocations++; return 1;
}
static void dma_free(OHCIControllerState *c,OHCIDMA *d)
{
    if (d->allocation) { OHCIPlatformFree(d->allocation,d->allocationBytes); c->allocations--; }
    bzero(d,sizeof(*d));
}
static void endpoint_free(OHCIControllerState *c,OHCIEndpoint *e)
{
    dma_free(c,&e->setup); dma_free(c,&e->data); dma_free(c,&e->descriptors);
    bzero(e,sizeof(*e));
}
static unsigned td_phys(OHCIEndpoint *e,unsigned n)
{ return e->descriptors.physicalAddress+16+n*16; }
int OHCICoreSupportsPCIClass(ohci_u32 code) { return code==0x0c0310U; }
int OHCICoreInitialize(OHCIControllerState *c)
{
    unsigned ctl,fi,a,i;
    OHCIED *anchors;
    OHCIHCCA *h;
    c->registersValid=1; c->addresses[0]=1;
    c->keyboardSlot=c->pointerSlot=OHCI_MAX_DEVICES;
    if ((read_op(c,OHCI_REVISION)&255)!=0x10) return 0;
    ctl=read_op(c,OHCI_CONTROL);
    if (ctl&OHCI_CTL_IR) {
        if (!write_op(c,OHCI_COMMAND,OHCI_CMD_OCR) ||
            !wait_bits(c,OHCI_CONTROL,OHCI_CTL_IR,0,500)) return 0;
    }
    fi=read_op(c,OHCI_FRAME_INTERVAL)&0x3fff;
    if (fi<10000 || fi>14000) fi=11999;
    if (!write_op(c,OHCI_INTR_DISABLE,0xffffffffU) ||
        !write_op(c,OHCI_CONTROL,0) ||
        !write_op(c,OHCI_COMMAND,OHCI_CMD_RESET) ||
        !wait_bits(c,OHCI_COMMAND,OHCI_CMD_RESET,0,100)) return 0;
    c->haltConfirmed=1;
    a=read_op(c,OHCI_RH_A); c->maxPorts=a&255;
    if (!c->maxPorts || c->maxPorts>OHCI_MAX_PORTS || OHCI_PORT(c->maxPorts)+4>c->mmioBytes) return 0;
    if (!dma_alloc(c,&c->hcca,256) || !dma_alloc(c,&c->anchors,1024)) return 0;
    anchors=c->anchors.virtualAddress; h=c->hcca.virtualAddress;
    /* Binary periodic tree: nodes for intervals 1,2,4,8,16,32. */
    for (i=0;i<64;i++) anchors[i].flags=OHCI_ED_SKIP;
    for (i=2;i<=32;i*=2) {
        unsigned p;
        for (p=0;p<i;p++) anchors[i-1+p].next=c->anchors.physicalAddress+16*(i/2-1+p%(i/2));
    }
    for (i=0;i<32;i++) h->interrupt[i]=c->anchors.physicalAddress+16*(31+i);
    barrier();
    if (!write_op(c,OHCI_HCCA,c->hcca.physicalAddress) || read_op(c,OHCI_HCCA)!=c->hcca.physicalAddress ||
        !write_op(c,OHCI_CONTROL_HEAD,c->anchors.physicalAddress+63*16) ||
        !write_op(c,OHCI_CONTROL_CURRENT,0) || !write_op(c,OHCI_BULK_HEAD,0) ||
        !write_op(c,OHCI_BULK_CURRENT,0) ||
        !write_op(c,OHCI_FRAME_INTERVAL,((read_op(c,OHCI_FRAME_INTERVAL)^0x80000000U)&0x80000000U)|(((fi-210)*6/7)<<16)|fi) ||
        !write_op(c,OHCI_PERIOD_START,fi*9/10) || !write_op(c,OHCI_LS_THRESHOLD,0x628) ||
        !write_op(c,OHCI_STS,OHCI_STS_W1C)) return 0;
    /* Honor the controller's advertised power switching mode. */
    if (!(a&0x200)) {
        if (a&0x100) {
            for (i=1;i<=c->maxPorts;i++) if (!write_op(c,OHCI_PORT(i),OHCI_PORT_POWER)) return 0;
        } else if (!write_op(c,OHCI_RH_STATUS,0x10000)) return 0;
        OHCIPlatformPause(c,((a>>24)&255)*2+20);
    }
    /* Firmware-addressed devices on other enabled ports must not answer
     * tokens while the management worker assigns new addresses. */
    for (i=1;i<=c->maxPorts;i++) {
        if (!write_op(c,OHCI_PORT(i),OHCI_PORT_CONNECT) ||
            !wait_bits(c,OHCI_PORT(i),OHCI_PORT_ENABLE|OHCI_PORT_RESET,0,500)) return 0;
    }
    c->initialized=1; return !c->fatal;
}
int OHCICoreStart(OHCIControllerState *c)
{
    if (!c->initialized || c->fatal) return 0;
    c->dmaArmed=1; c->haltConfirmed=0;
    if (!write_op(c,OHCI_CONTROL,OHCI_CTL_OPERATIONAL|OHCI_CTL_CLE|OHCI_CTL_PLE|3) ||
        !wait_bits(c,OHCI_CONTROL,0xc0,OHCI_CTL_OPERATIONAL,100)) return 0;
    c->running=1; return 1;
}
static int drain_done(OHCIControllerState *c)
{
    unsigned head=OHCIPlatformTakeDone(c), limit=0;
    while (head && !c->fatal) {
        unsigned i,j,n=0,found=0;
        OHCIEndpoint *e=0;
        OHCIDevice *d=0;
        if ((head&15) || ++limit>OHCI_MAX_DEVICES*OHCI_ENDPOINTS*OHCI_TDS) {
            OHCICoreFail(c,"invalid done chain",head); break;
        }
        for (i=0;i<OHCI_MAX_DEVICES && !found;i++) for (j=0;j<OHCI_ENDPOINTS && !found;j++) {
            e=&c->devices[i].endpoints[j];
            if (e->configured && head>=td_phys(e,0) && head<td_phys(e,OHCI_TDS)) {
                n=(head-td_phys(e,0))/16; d=&c->devices[i]; found=1; break;
            }
        }
        if (!found || !e->tdOwned[n] || e->generation!=d->generation) {
            OHCICoreFail(c,"unowned done TD",head); break;
        }
        {
            OHCITD *td=&e->td[n];
            unsigned cc=td->flags>>28, actual=e->tdLength[n], cbp=td->cbp;
            head=td->next; e->tdOwned[n]=0; e->completed++;
            if (cbp) {
                if (cbp<e->tdStart[n] || cbp>e->tdStart[n]+actual) {
                    OHCICoreFail(c,"TD buffer bounds",cbp); break;
                }
                actual=cbp-e->tdStart[n];
            }
            if (cc) { e->result=-(int)cc; c->errors++; }
            if (e->tdStage[n]==1 && !cc) e->actual+=actual;
            if (e->completed==e->count || cc) e->busy=0;
            c->transferEvents++;
        }
    }
    return !c->fatal;
}
/* One mutation owner remains set while Pause drops the native state lock. */
static int schedule_stop(OHCIControllerState *c)
{
    unsigned before=0, advances=0;
    ohci_u64 deadline=OHCIPlatformMilliseconds()+100;
    while (c->scheduleBusy && !c->fatal) OHCIPlatformPause(c,1);
    if (c->fatal) return 0;
    c->scheduleBusy=1;
    if (!write_op(c,OHCI_CONTROL,OHCI_CTL_OPERATIONAL|3) ||
        (read_op(c,OHCI_CONTROL)&0x3c)) goto failed;
    before=read_op(c,OHCI_FRAME_NUMBER)&65535;
    while (!c->fatal) {
        unsigned now;
        OHCIPlatformPause(c,1); now=read_op(c,OHCI_FRAME_NUMBER)&65535;
        if (now!=before) { advances+=(now-before)&65535; before=now; }
        if (!drain_done(c)) return 0;
        /* While WDH is pending the HC can accumulate a second done list.
         * Acknowledge the HCCA list, then wait for that internal list to be
         * written back too. Two frame advances alone do not drain both. */
        if (advances>=2 && !(read_op(c,OHCI_DONE_HEAD)&~15U) &&
            !(read_op(c,OHCI_STS)&OHCI_INT_WDH)) break;
        if (OHCIPlatformMilliseconds()>=deadline) goto failed;
    }
    return !c->fatal;
failed:
    OHCICoreFail(c,"schedule retirement",before); return 0;
}
static int schedule_start(OHCIControllerState *c)
{
    unsigned i,j,period,phase;
    OHCIED *a=c->anchors.virtualAddress;
    a[63].next=0;
    for (period=1;period<=32;period*=2) for (phase=0;phase<period;phase++)
        a[period-1+phase].next=period==1 ? 0 : c->anchors.physicalAddress+16*(period/2-1+phase%(period/2));
    for (i=0;i<OHCI_MAX_DEVICES;i++) for (j=0;j<OHCI_ENDPOINTS;j++) {
        OHCIEndpoint *e=&c->devices[i].endpoints[j];
        unsigned node;
        if (!e->configured || !e->linked) continue;
        node=e->periodic ? e->interval-1+e->phase : 63;
        e->ed->next=a[node].next; a[node].next=e->descriptors.physicalAddress;
    }
    barrier();
    if (!write_op(c,OHCI_CONTROL_CURRENT,0) || !write_op(c,OHCI_PERIOD_CURRENT,0) ||
        !write_op(c,OHCI_CONTROL,OHCI_CTL_OPERATIONAL|OHCI_CTL_CLE|OHCI_CTL_PLE|3) ||
        !write_op(c,OHCI_COMMAND,OHCI_CMD_CLF)) return 0;
    c->scheduleChanges++; c->scheduleBusy=0; c->rescan=1; OHCIPlatformWake(c->owner);
    return 1;
}
int OHCICoreConfigureEndpoint(OHCIControllerState *c,OHCIDevice *d,unsigned index,unsigned ep,unsigned packet,unsigned interval)
{
    OHCIEndpoint *e;
    unsigned flags,p,best=0,bestLoad=~0U,load;
    if (index>=OHCI_ENDPOINTS || !packet || packet>64 || d->speed>OHCI_SPEED_LOW ||
        (d->speed==OHCI_SPEED_LOW && (packet>8 || (index && interval<10))) ||
        (index && (!interval || !(ep&0x80) || !(ep&15) || (ep&0x70)))) return 0;
    if (!index && packet!=8 && packet!=16 && packet!=32 && packet!=64) return 0;
    e=&d->endpoints[index]; if (e->configured) return 0;
    if (!dma_alloc(c,&e->descriptors,128) || !dma_alloc(c,&e->data,4096) ||
        !dma_alloc(c,&e->setup,8)) { endpoint_free(c,e); return 0; }
    e->ed=e->descriptors.virtualAddress; e->td=(OHCITD *)((ohci_u8 *)e->ed+16);
    e->maxPacket=packet; e->endpoint=ep; e->periodic=index!=0; e->role=index;
    e->interval=1;
    if (index) {
        while (e->interval<32 && e->interval*2<=interval) e->interval*=2;
        /* Microseconds, deliberately above worst-case stuffed USB transaction time. */
        load=d->speed==OHCI_SPEED_LOW ? 160+packet*8 : 20+packet;
        for (p=0;p<e->interval;p++) {
            unsigned f,total=0,max=0;
            for (f=p;f<32;f+=e->interval) { total+=c->frameLoad[f]; if (c->frameLoad[f]>max) max=c->frameLoad[f]; }
            if (max+load<=800 && total<bestLoad) { best=p; bestLoad=total; }
        }
        if (bestLoad==~0U) { endpoint_free(c,e); return 0; }
        e->phase=best; e->load=load;
    }
    flags=d->address|((ep&15)<<7)|(packet<<16)|OHCI_ED_SKIP;
    if (d->speed==OHCI_SPEED_LOW) flags|=OHCI_ED_LOW;
    if (index) flags|=0x1000; /* ED direction IN. */
    e->ed->flags=flags; e->ed->head=e->ed->tail=td_phys(e,0);
    if (!schedule_stop(c)) return 0;
    e->configured=e->linked=1; e->generation=d->generation;
    if (index) for (p=e->phase;p<32;p+=e->interval) c->frameLoad[p]+=e->load;
    return schedule_start(c);
}
int OHCICoreCancel(OHCIControllerState *c,OHCIDevice *d,OHCIEndpoint *e)
{
    unsigned p;
    (void)d;
    if (!e->configured || !e->linked) return 1;
    e->retiring=1; e->ed->flags|=OHCI_ED_SKIP; barrier();
    if (!schedule_stop(c)) return 0;
    e->linked=0; e->busy=0; bzero(e->tdOwned,sizeof(e->tdOwned));
    if (e->periodic) for (p=e->phase;p<32;p+=e->interval) c->frameLoad[p]-=e->load;
    return schedule_start(c);
}
static void fill_td(OHCIEndpoint *e,unsigned n,unsigned flags,unsigned address,unsigned length,unsigned next,unsigned stage)
{
    OHCITD *td=&e->td[n];
    td->flags=OHCI_TD_NOT_ACCESSED|flags;
    td->cbp=length ? address : 0; td->end=length ? address+length-1 : 0;
    td->next=td_phys(e,next);
    e->tdOwned[n]=1; e->tdLength[n]=length; e->tdStart[n]=address; e->tdStage[n]=stage;
}
static void interrupt_arm(OHCIEndpoint *e)
{
    unsigned n=e->dummy,next=n^1;
    e->completed=e->actual=0; e->count=1; e->result=1; e->busy=1;
    fill_td(e,n,OHCI_TD_IN|OHCI_TD_ROUND,e->data.physicalAddress,e->maxPacket,next,1);
    barrier(); e->ed->tail=td_phys(e,next); barrier(); e->ed->flags&=~OHCI_ED_SKIP;
    e->dummy=next;
}
static int update_ep0(void *host,void *device,ohci_u8 packet)
{
    OHCIControllerState *c=host; OHCIDevice *d=device; OHCIEndpoint *e=&d->endpoints[0];
    if ((packet!=8 && packet!=16 && packet!=32 && packet!=64) ||
        (d->speed==OHCI_SPEED_LOW && packet!=8)) return 0;
    if (!schedule_stop(c)) return 0;
    e->maxPacket=packet; e->ed->flags=(e->ed->flags&~0x7ff0000U)|(packet<<16);
    return schedule_start(c);
}
static int control_transfer(void *host,void *device,const USBSetupPacket *s,void *bytes,ohci_u16 *actual)
{
    OHCIControllerState *c=host; OHCIDevice *d=device; OHCIEndpoint *e=&d->endpoints[0];
    unsigned status=s->length ? 2 : 1,gen=d->generation;
    ohci_u64 end=OHCIPlatformMilliseconds()+OHCI_TIMEOUT_MS;
    int result=0;
    if (actual) *actual=0;
    if (!d->used || d->disconnecting || c->fatal || s->length>4096 || (s->length && !bytes)) return 0;
    while (d->controlBusy && !c->fatal && d->generation==gen) {
        if (OHCIPlatformMilliseconds()>=end) return 0;
        OHCIPlatformPause(c,1);
    }
    if (c->fatal || d->generation!=gen || d->disconnecting) return 0;
    d->controlBusy=1;
    if (!schedule_stop(c)) goto done;
    e->ed->flags=(e->ed->flags&~127U)|d->address|OHCI_ED_SKIP;
    bcopy(s,e->setup.virtualAddress,8);
    if (s->length && !(s->requestType&0x80)) bcopy(bytes,e->data.virtualAddress,s->length);
    bzero(e->tdOwned,sizeof(e->tdOwned));
    e->generation=gen; e->actual=e->completed=0; e->count=status+1; e->result=1; e->busy=1;
    fill_td(e,0,OHCI_TD_DATA0,e->setup.physicalAddress,8,1,0);
    if (s->length) fill_td(e,1,OHCI_TD_DATA1|OHCI_TD_ROUND|((s->requestType&0x80)?OHCI_TD_IN:OHCI_TD_OUT),
        e->data.physicalAddress,s->length,2,1);
    fill_td(e,status,OHCI_TD_DATA1|((s->length && (s->requestType&0x80))?OHCI_TD_OUT:OHCI_TD_IN),0,0,3,2);
    e->ed->head=td_phys(e,0); e->ed->tail=td_phys(e,3); barrier(); e->ed->flags&=~OHCI_ED_SKIP;
    if (!schedule_start(c)) goto done;
    while (e->busy && !c->fatal && d->generation==gen) {
        unsigned port=read_op(c,OHCI_PORT(d->port));
        if (!(port&OHCI_PORT_CONNECT) || (port&OHCI_PORT_CSC)) { e->result=-21; break; }
        if (OHCIPlatformMilliseconds()>=end) { c->timeouts++; e->result=-20; break; }
        OHCIPlatformPause(c,1);
    }
    if (c->fatal || d->generation!=gen) goto done;
    e->ed->flags|=OHCI_ED_SKIP; barrier();
    if (!schedule_stop(c)) goto done;
    result=e->result>0 && !e->busy;
    if (result && s->length && (s->requestType&0x80)) bcopy(e->data.virtualAddress,bytes,e->actual);
    if (actual && result) *actual=e->actual;
    e->busy=0; bzero(e->tdOwned,sizeof(e->tdOwned)); e->ed->head=e->ed->tail=td_phys(e,3);
    if (!schedule_start(c)) result=0;
done:
    d->controlBusy=0; return result;
}
static const USBTransportOperations usb_ops={control_transfer,update_ep0};
int OHCICoreService(OHCIControllerState *c,unsigned causes)
{
    unsigned i,j;
    if (c->fatal) return 0;
    if (causes&OHCI_INT_UE) { OHCICoreFail(c,"unrecoverable controller error",causes); return 0; }
    if (c->scheduleBusy) { c->rescan=1; return 1; }
    c->rescan=0;
    if (!drain_done(c)) return 0;
    for (i=0;i<OHCI_MAX_DEVICES;i++) {
        OHCIDevice *d=&c->devices[i];
        for (j=1;j<OHCI_ENDPOINTS;j++) {
            OHCIEndpoint *e=&d->endpoints[j];
            if (!d->ready || d->disconnecting || !e->configured || e->retiring || e->busy) continue;
            if (e->result<0) { c->disconnected|=1U<<i; continue; }
            if (e->completed) {
                if (j==1) { c->keyboardReports++; OHCIPlatformKeyboardReport(c->owner,e->data.virtualAddress,e->actual); }
                else { c->mouseReports++; OHCIPlatformPointerReport(c->owner,e->data.virtualAddress,e->actual); }
            }
            interrupt_arm(e);
        }
    }
    return !c->fatal;
}
int OHCICoreReleaseDMA(OHCIControllerState *c,int busMasterDisabled)
{
    unsigned i,j;
    if (c->dmaArmed && !c->haltConfirmed && !busMasterDisabled) return 0;
    for (i=0;i<OHCI_MAX_DEVICES;i++) for (j=0;j<OHCI_ENDPOINTS;j++) endpoint_free(c,&c->devices[i].endpoints[j]);
    dma_free(c,&c->hcca); dma_free(c,&c->anchors); return 1;
}
#include "OHCIEnumeration.inc"
int OHCICoreSetKeyboardLEDs(OHCIControllerState *c,ohci_u8 leds)
{
    OHCIDevice *d;
    if (c->keyboardSlot>=OHCI_MAX_DEVICES) return 0;
    d=&c->devices[c->keyboardSlot];
    return d->ready && USBHIDSetKeyboardLEDs(&d->usb,d->interfaceNumber[0],leds);
}
