/* Included by OHCICore.c. All management runs on one sleepable worker. */
static int device_request(OHCIDevice *d, unsigned type, unsigned request,
                           unsigned value, unsigned index, void *data, unsigned size)
{
    USBSetupPacket s;
    ohci_u16 actual = 0;
    s.requestType = type; s.request = request; s.value = value;
    s.index = index; s.length = size;
    return USBCoreControlTransfer(&d->usb, &s, data, &actual) && actual == size;
}
static unsigned new_generation(OHCIControllerState *c)
{
    if (c->nextGeneration == 0xffffffffU) { OHCICoreFail(c, "generation exhausted", 0); return 0; }
    return ++c->nextGeneration;
}
static unsigned allocate_address(OHCIControllerState *c)
{
    unsigned a;
    for (a = 1; a < 128; a++) if (!(c->addresses[a / 32] & (1U << (a % 32)))) {
        c->addresses[a / 32] |= 1U << (a % 32); return a;
    }
    return 0;
}
static int child_slot(OHCIControllerState *c, unsigned parent, unsigned port)
{
    unsigned i;
    for (i = 0; i < OHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == parent &&
            (parent ? c->devices[i].parentPort : c->devices[i].port) == port) return (int)i;
    return -1;
}

/* An addressed but unsupported/offline device still answers USB tokens.
 * Isolate its physical port before releasing the address. Failure escalates
 * to controller containment and retains all ownership/DMA for that device. */
static int root_isolated(OHCIControllerState *c, unsigned rootPort)
{
    ohci_u32 v;
    if (!rootPort || rootPort > c->maxPorts) return 0;
    v = read_op(c, OHCI_PORT(rootPort));
    return !c->fatal && (!(v & OHCI_PORT_CONNECT) ||
                        !(v & (OHCI_PORT_ENABLE | OHCI_PORT_RESET)));
}

static int isolate_port(OHCIControllerState *c, unsigned rootPort,
                        unsigned parent, unsigned parentPort)
{
    OHCIDevice *hub;
    ohci_u32 v;
    ohci_u8 status[4];
    if (!parent) {
        if (!rootPort || rootPort > c->maxPorts) return 0;
        v = read_op(c, OHCI_PORT(rootPort));
        if (c->fatal) return 0;
        if (!(v & OHCI_PORT_CONNECT)) return 1;
        c->blockedPorts |= 1U << rootPort;
        return write_op(c, OHCI_PORT(rootPort),
                        OHCI_PORT_CONNECT) &&
               wait_bits(c, OHCI_PORT(rootPort), OHCI_PORT_ENABLE | OHCI_PORT_RESET, 0, 100);
    }
    if (parent > OHCI_MAX_DEVICES || !parentPort ||
        parentPort > OHCI_MAX_HUB_PORTS) return 0;
    /* Child transfer errors can be serviced before the root unplug event.
     * A disabled/disconnected upstream port needs no request to the dead hub. */
    if (root_isolated(c, rootPort)) return 1;
    if (c->fatal) return 0;
    hub = &c->devices[parent - 1];
    if (!hub->used || !hub->ready || !hub->hub || hub->disconnecting ||
        !device_request(hub, 0xa3, 0, 0, parentPort, status, 4))
        return root_isolated(c, rootPort);
    if (!(USBCoreReadLE16(status) & 1)) return 1;
    hub->blockedPorts |= 1U << parentPort;
    if (device_request(hub, 0x23, 1, 1, parentPort, 0, 0) &&
        device_request(hub, 0xa3, 0, 0, parentPort, status, 4) &&
        !(USBCoreReadLE16(status) & 0x12)) return 1;
    /* Hub control sleeps release the lock; unplug may occur during them. */
    return root_isolated(c, rootPort);
}

static void disconnect_device(OHCIControllerState *c, unsigned slot, int isolated)
{
    OHCIDevice *d;
    unsigned i, generation;
    if (slot >= OHCI_MAX_DEVICES || !c->devices[slot].used) return;
    d = &c->devices[slot];
    d->disconnecting = 1; d->ready = 0;
    {
        static const ohci_u8 released[8]={0,0,0,0,0,0,0,0};
        if (c->keyboardSlot==slot) { c->keyboardSlot=OHCI_MAX_DEVICES; OHCIPlatformKeyboardReport(c->owner,released,8); }
        if (c->pointerSlot==slot) { c->pointerSlot=OHCI_MAX_DEVICES; OHCIPlatformPointerReport(c->owner,released,3); }
    }
    if (!isolated && !isolate_port(c, d->port, d->parent, d->parentPort)) {
        OHCICoreFail(c, "disconnect isolation", slot); return;
    }
    /* Isolating an upstream hub also isolates all its children; its EP0
     * cannot be used after this point to disable downstream ports. */
    for (i = 0; i < OHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == slot + 1)
            disconnect_device(c, i, 1);
    /* Invalidate clients before dropping the lock for frame retirement. */
    for (i=0;i<OHCI_ENDPOINTS;i++)
        if (d->endpoints[i].configured && !OHCICoreCancel(c,d,&d->endpoints[i])) return;
    /* All child QHs remain unlinked while other schedule users run. */
    generation = new_generation(c);
    c->addresses[d->address / 32] &= ~(1U << (d->address % 32));
    c->addresses[0] |= 1;
    for (i = 0; i < OHCI_ENDPOINTS; i++) endpoint_free(c,&d->endpoints[i]);
    bzero(d, sizeof(*d)); d->generation = generation;
    c->disconnected &= ~(1U << slot);
}

void OHCICoreDisconnect(OHCIControllerState *c, unsigned slot)
{
    disconnect_device(c, slot, 0);
}

/* USB 2.0 sections 7.2.1 and 11.13: descriptor capability alone does not
 * establish the current supply. Reserve one unit for every external port. */
static int hub_power(OHCIDevice *d, unsigned *selfPowered)
{
    ohci_u8 deviceStatus[2], hubStatus[4];
    if (!device_request(d, 0x80, 0, 0, 0, deviceStatus, 2) ||
        !device_request(d, 0xa0, 0, 0, 0, hubStatus, 4) ||
        (hubStatus[0] & 2)) return 0;
    *selfPowered = d->selfPowerCapable && (deviceStatus[0] & 1) && !(hubStatus[0] & 1);
    if (*selfPowered) return d->powerMA <= 100;
    return d->powerMA && d->externalPorts <= 4 &&
           d->powerMA + d->externalPorts * 100 <= 500 &&
           !(d->hubCharacteristics & 2);
}

static void hub_power_off(OHCIControllerState *c, OHCIDevice *d)
{
    unsigned p;
    ohci_u8 status[4];
    /* Disable the entire subtree on loss/uncertainty of its supply. A USB
     * port-disable alone would leave configured high-current loads powered. */
    c->disconnected |= 1U << (d - c->devices);
    for (p = 1; p <= d->hubPorts && !c->fatal; p++) {
        if (!device_request(d, 0x23, 1, 8, p, 0, 0) ||
            !device_request(d, 0xa3, 0, 0, p, status, 4) ||
            (USBCoreReadLE16(status) & 0x100)) {
            if (!root_isolated(c, d->port)) OHCICoreFail(c, "hub power isolation", p);
            return;
        }
        d->portPowerMA[p] = 0;
    }
}

static int configuration_power(OHCIControllerState *c, OHCIDevice *d)
{
    OHCIDevice *hub;
    unsigned selfPowered, i, total;
    if (d->powerMA > 500 || (!d->powerMA && !d->selfPowerCapable)) return 0;
    if (!d->parent) return 1;
    hub = &c->devices[d->parent - 1];
    if (!hub->ready || hub->disconnecting ||
        (c->disconnected & (1U << (d->parent - 1)))) return 0;
    if (!hub_power(hub, &selfPowered) || (hub->selfPowered && !selfPowered)) {
        hub_power_off(c, hub); return 0;
    }
    hub->selfPowered = selfPowered;
    if (!selfPowered && (hub->nonRemovable & (1U << d->parentPort))) {
        total = hub->powerMA + hub->externalPorts * 100 + d->powerMA;
        for (i = 1; i <= hub->hubPorts; i++)
            if (i != d->parentPort) total += hub->portPowerMA[i];
        if (total > 500) return 0;
        /* Reserve before SET_CONFIGURATION: a failed status stage can still
         * leave the function configured. PORT_ENABLE clear does not release
         * its supply; retain this charge independently of the device slot. */
        hub->portPowerMA[d->parentPort] = d->powerMA;
        return 1;
    }
    return d->powerMA <= (selfPowered ? 500U : 100U);
}

static int configure_hub(OHCIControllerState *c, OHCIDevice *d, USBEnumerationData *info)
{
    USBDescriptorIterator it;
    const ohci_u8 *desc;
    ohci_u8 len, type, hubdesc[16];
    unsigned p, delay, descriptorLength;
    int active=0, found=0, rc;
    if (d->parent || d->speed!=OHCI_SPEED_FULL) return 0;
    USBCoreDescriptorIteratorInitialize(&it,info->configurationDescriptor,info->configurationLength);
    while ((rc=USBCoreDescriptorNext(&it,&desc,&len,&type))>0) {
        if (type==USB_DESC_INTERFACE) {
            if (len<9) return 0;
            active=desc[3]==0 && desc[5]==9 && desc[6]==0 && desc[7]==0;
        } else if (active && type==USB_DESC_ENDPOINT) {
            if (len<7 || desc[3]!=3 || !(desc[2]&0x80) || !(desc[2]&15) ||
                (desc[2]&0x70) || !USBCoreReadLE16(desc+4) || USBCoreReadLE16(desc+4)>64 || !desc[6]) return 0;
            found++;
        }
    }
    if (rc<0 || found!=1 || !configuration_power(c,d) ||
        !USBCoreSetConfiguration(&d->usb,info->configurationValue)) return 0;
    bzero(hubdesc, sizeof(hubdesc));
    if (!device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, 7) ||
        hubdesc[0] < 7 || hubdesc[1] != 0x29 || !hubdesc[2] || hubdesc[2] > OHCI_MAX_HUB_PORTS)
        return 0;
    d->hub = 1; d->hubPorts = hubdesc[2];
    d->hubCharacteristics = USBCoreReadLE16(hubdesc + 3);
    descriptorLength = 7 + 2 * ((d->hubPorts + 8) / 8);
    if (hubdesc[0] < descriptorLength ||
        !device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, descriptorLength) ||
        hubdesc[1] != 0x29 || hubdesc[2] != d->hubPorts) return 0;
    for (p = 1; p <= d->hubPorts; p++) {
        if (hubdesc[7 + p / 8] & (1U << (p % 8))) d->nonRemovable |= 1U << p;
        else d->externalPorts++;
    }
    if (!hub_power(d, &d->selfPowered)) return 0;
    for (p = 1; p <= d->hubPorts; p++)
        if (!device_request(d, 0x23, 3, 8, p, 0, 0)) return 0;
    delay = (unsigned)hubdesc[5] * 2 + 20;
    OHCIPlatformPause(c, delay);
    d->hubChanges = (1U << (d->hubPorts + 1)) - 1;
    return 1;
}

static int attach_device(OHCIControllerState *,unsigned,unsigned,unsigned,unsigned);

static int hub_status(OHCIDevice *d, unsigned port, unsigned *status, unsigned *change)
{
    ohci_u8 b[4];
    if (!device_request(d, port ? 0xa3 : 0xa0, 0, 0, port, b, 4)) return 0;
    *status = USBCoreReadLE16(b); *change = USBCoreReadLE16(b + 2);
    return 1;
}
static int debounce_port(OHCIControllerState *c, OHCIDevice *hub,
                          unsigned port, unsigned *status)
{
    unsigned change;
    ohci_u64 end = OHCIPlatformMilliseconds() + 1500;
    /* Connection-change bits are sticky and only this management worker
     * acknowledges them. Any bounce restarts the full 100 ms interval. */
    do {
        OHCIPlatformPause(c, 100);
        if (hub) {
            if (!hub_status(hub, port, status, &change) ||
                !(*status & 1) || (*status & 8)) return 0;
            if (!(change & 1)) return 1;
            if (!device_request(hub, 0x23, 1, 16, port, 0, 0)) return 0;
        } else {
            *status = read_op(c, OHCI_PORT(port));
            if (c->fatal || !(*status & OHCI_PORT_CONNECT) ||
                (*status & OHCI_PORT_OC)) return 0;
            if (!(*status & OHCI_PORT_CSC)) return 1;
            if (!write_op(c, OHCI_PORT(port),
                OHCI_PORT_CSC)) return 0;
        }
    } while (OHCIPlatformMilliseconds() < end && !c->fatal);
    return 0;
}
static void service_hub(OHCIControllerState *c, unsigned slot)
{
    OHCIDevice *d = &c->devices[slot];
    unsigned p, status, change, changes = (1U << (d->hubPorts+1))-1;
    d->hubChanges = 0;
    if (changes & 1) {
        unsigned selfPowered;
        if (!hub_status(d, 0, &status, &change) ||
            !hub_power(d, &selfPowered) || (d->selfPowered && !selfPowered)) {
            hub_power_off(c, d); return;
        }
        d->selfPowered = selfPowered;
        if (change & 1) device_request(d, 0x20, 1, 0, 0, 0, 0);
        if (change & 2) device_request(d, 0x20, 1, 1, 0, 0, 0);
        if (status & 2) { c->disconnected |= 1U << slot; return; }
    }
    for (p = 1; p <= d->hubPorts && !c->fatal &&
        !(c->disconnected & (1U << slot)); p++) if (changes & (1U << p)) {
        int child;
        unsigned bit;
        ohci_u64 end;
        if (!hub_status(d, p, &status, &change)) { c->disconnected |= 1U << slot; return; }
        if (!(status & 1)) d->portPowerMA[p] = 0;
        for (bit = 0; bit < 5; bit++) if (change & (1U << bit))
            if (!device_request(d, 0x23, 1, 16 + bit, p, 0, 0)) return;
        child = child_slot(c, slot + 1, p);
        if (child >= 0 && (!(status & 1) || !(status & 2) || (change & 1) || (status & 8))) {
            OHCICoreDisconnect(c, child); child = -1;
        }
        if ((change & 1) || !(status & 1)) d->blockedPorts &= ~(1U << p);
        if (c->fatal || !(status & 1) || (status & 8) || child >= 0 ||
            (d->blockedPorts & (1U << p))) continue;
        if (!debounce_port(c, d, p, &status)) goto reject_port;
        if (!device_request(d, 0x23, 3, 4, p, 0, 0)) goto reject_port;
        end = OHCIPlatformMilliseconds() + 500;
        do {
            OHCIPlatformPause(c, 10);
            if (!hub_status(d, p, &status, &change)) goto reject_port;
            if (!(status & 0x10)) break;
        } while (OHCIPlatformMilliseconds() < end);
        if ((status & 0x13) != 3) goto reject_port;
        if (!device_request(d, 0x23, 1, 20, p, 0, 0)) goto reject_port;
        /* A completed USB reset returns the function to default power. */
        d->portPowerMA[p] = 0;
        OHCIPlatformPause(c, 10);
        attach_device(c, d->port, slot + 1, p, (status & 0x400) ? OHCI_SPEED_HIGH :
            (status & 0x200) ? OHCI_SPEED_LOW : OHCI_SPEED_FULL);
        continue;
reject_port:
        /* A reset may have enabled an unaddressed device even if its control
         * status/acknowledgement failed. Do not expose a second address zero. */
        if (!isolate_port(c, d->port, slot + 1, p)) OHCICoreFail(c, "hub port isolation", p);
    }
}

static int configuration_rank(void *context,const USBEnumerationData *info)
{
    OHCIDevice *d=context;
    OHCIControllerState *c=d->usb.hostContext;
    USBHIDInterface hid[2];
    int mask;
    if (info->error) return -1;
    if (info->deviceDescriptor[4]==9) return !d->parent && d->speed==OHCI_SPEED_FULL;
    mask=USBHIDFindBootInterfaces(info->configurationDescriptor,info->configurationLength,d->speed==OHCI_SPEED_LOW,hid);
    if (mask<0) return -1;
    if (!c->inputEnabled) return 0;
    return ((mask&1) && c->keyboardSlot==OHCI_MAX_DEVICES)+
           ((mask&2) && c->pointerSlot==OHCI_MAX_DEVICES);
}
static int attach_device(OHCIControllerState *c,unsigned rootPort,unsigned parent,unsigned parentPort,unsigned speed)
{
    OHCIDevice *d;
    USBEnumerationData *info=0;
    unsigned slot,address,i;
    ohci_u8 first[8];
    ohci_u16 actual=0;
    int mask;
    const char *stage="endpoint zero";
    if (speed>OHCI_SPEED_LOW) {
        if (!isolate_port(c,rootPort,parent,parentPort)) OHCICoreFail(c,"unsupported speed isolation",rootPort);
        return 0;
    }
    for (slot=0;slot<OHCI_MAX_DEVICES && c->devices[slot].used;slot++) {}
    if (slot==OHCI_MAX_DEVICES || !(address=allocate_address(c))) {
        if (!isolate_port(c,rootPort,parent,parentPort)) OHCICoreFail(c,"slot isolation",rootPort);
        return 0;
    }
    d=&c->devices[slot]; bzero(d,sizeof(*d));
    d->used=1; d->generation=new_generation(c); d->port=rootPort;
    d->parent=parent; d->parentPort=parentPort; d->speed=speed;
    info=OHCIPlatformAllocate(sizeof(*info));
    if (!info) goto fail;
    bzero(info,sizeof(*info));
    USBCoreDeviceInitialize(&d->usb,&usb_ops,c,d);
    if (!OHCICoreConfigureEndpoint(c,d,0,0,8,0)) goto fail;
    stage="initial descriptor";
    if (!USBCoreGetDescriptor(&d->usb,1,0,first,8,&actual) || actual!=8 ||
        first[0]<18 || first[1]!=1 || !update_ep0(c,d,first[7])) goto fail;
    stage="set address";
    if (!device_request(d,0,5,address,0,0,0)) goto fail;
    d->address=address; OHCIPlatformPause(c,2);
    stage="configuration descriptors";
    if (!USBCoreReadDevice(&d->usb,info) ||
        USBCoreSelectConfiguration(&d->usb,info,configuration_rank,d)!=1) goto fail;
    d->configurationValue=info->configurationValue;
    d->powerMA=info->configurationDescriptor[8]*2;
    d->selfPowerCapable=(info->configurationDescriptor[7]&0x40)!=0;
    if (info->deviceDescriptor[4]==9) {
        stage="hub configuration";
        if (!configure_hub(c,d,info)) goto fail;
    } else {
        mask=USBHIDFindBootInterfaces(info->configurationDescriptor,info->configurationLength,speed==OHCI_SPEED_LOW,d->hid);
        if (mask<=0) goto fail;
        if (c->keyboardSlot!=OHCI_MAX_DEVICES) mask&=~1;
        if (c->pointerSlot!=OHCI_MAX_DEVICES) mask&=~2;
        stage="HID configuration";
        if (!mask || !configuration_power(c,d) || !USBCoreSetConfiguration(&d->usb,d->configurationValue)) goto fail;
        for (i=0;i<2;i++) if (mask&(1U<<i)) {
            USBHIDInterface *h=&d->hid[i];
            stage="boot protocol";
            if (!USBHIDSetBootProtocol(&d->usb,h->interfaceNumber)) goto fail;
            USBHIDSetIdle(&d->usb,h->interfaceNumber);
            stage="interrupt endpoint";
            if (!OHCICoreConfigureEndpoint(c,d,i+1,h->endpointAddress,h->maxPacket,h->interval)) goto fail;
            d->interfaceNumber[i]=h->interfaceNumber; d->roles|=1U<<i;
        }
        if (d->roles&1) c->keyboardSlot=slot;
        if (d->roles&2) c->pointerSlot=slot;
    }
    d->ready=1; c->rescan=1; OHCIPlatformWake(c->owner);
    OHCIPlatformLog("OHCI: root %u hub %u address %u %04x:%04x speed %u roles %u hub-ports %u ready\n",
        rootPort,parentPort,address,info->vendorID,info->productID,speed,d->roles,d->hubPorts);
    OHCIPlatformFree(info,sizeof(*info)); return 1;
fail:
    c->enumerationFailure.stage=stage; c->enumerationFailure.rootPort=rootPort;
    c->enumerationFailure.hubPort=parentPort; c->enumerationFailure.speed=speed;
    c->enumerationFailure.vendor=info ? info->vendorID : 0;
    c->enumerationFailure.product=info ? info->productID : 0;
    OHCIPlatformLog("OHCI: root %u hub %u address %u rejected at %s\n",rootPort,parentPort,address,stage);
    d->address=address; OHCICoreDisconnect(c,slot);
    if (info) OHCIPlatformFree(info,sizeof(*info));
    return 0;
}
int OHCICoreServicePorts(OHCIControllerState *c)
{
    unsigned p,i;
    if (!c->running || c->fatal || c->enumerating) return 0;
    c->enumerating=1;
    for (i=0;i<OHCI_MAX_DEVICES && !c->fatal;i++) if (c->disconnected&(1U<<i)) OHCICoreDisconnect(c,i);
    for (p=1;p<=c->maxPorts && !c->fatal;p++) {
        unsigned v=read_op(c,OHCI_PORT(p));
        int child=child_slot(c,0,p);
        const char *stage="debounce";
        if (c->fatal) break;
        if (!(c->rootPortsScanned&(1U<<p)) || (v&OHCI_PORT_CSC))
            OHCIPlatformLog("OHCI: root %u status %08x\n",p,v);
        c->rootPortsScanned|=1U<<p;
        if (!write_op(c,OHCI_PORT(p),v&OHCI_PORT_CHANGES)) break;
        if (child>=0 && (!(v&OHCI_PORT_CONNECT) || (v&OHCI_PORT_CSC) || !(v&OHCI_PORT_ENABLE) || (v&OHCI_PORT_OC))) {
            OHCICoreDisconnect(c,child); child=-1;
        }
        if ((v&OHCI_PORT_CSC) || !(v&OHCI_PORT_CONNECT)) c->blockedPorts&=~(1U<<p);
        if (c->fatal || !(v&OHCI_PORT_CONNECT) || child>=0 || (c->blockedPorts&(1U<<p))) continue;
        if ((v&OHCI_PORT_OC) || !debounce_port(c,0,p,&v)) goto reject;
        stage="reset";
        if (!write_op(c,OHCI_PORT(p),OHCI_PORT_RESET) || !wait_bits(c,OHCI_PORT(p),OHCI_PORT_RESET,0,500)) goto reject;
        v=read_op(c,OHCI_PORT(p));
        if ((v&(OHCI_PORT_CONNECT|OHCI_PORT_ENABLE|OHCI_PORT_OC))!=(OHCI_PORT_CONNECT|OHCI_PORT_ENABLE)) goto reject;
        if (!write_op(c,OHCI_PORT(p),v&OHCI_PORT_CHANGES)) break;
        OHCIPlatformPause(c,10);
        attach_device(c,p,0,0,(v&OHCI_PORT_LOW)?OHCI_SPEED_LOW:OHCI_SPEED_FULL);
        continue;
reject:
        OHCIPlatformLog("OHCI: root %u rejected at %s status %08x\n",p,stage,v);
        if (!c->fatal && !isolate_port(c,p,0,0)) OHCICoreFail(c,"root isolation",p);
    }
    for (i=0;i<OHCI_MAX_DEVICES && !c->fatal;i++) if (c->devices[i].ready && c->devices[i].hub) service_hub(c,i);
    if (!c->fatal) write_op(c,OHCI_STS,OHCI_INT_RHSC);
    c->enumerating=0; return !c->fatal;
}

