#define MACH_USER_API 1
#import "OHCIController.h"
#import "OHCIUSBKeyboard.h"
#import "OHCIUSBPointer.h"
#import "OHCIVersion.h"
#import "OHCICompletionWait.h"
#import "OHCIRuntime.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <driverkit/i386/kernelDriver.h>
#import <driverkit/i386/IOPCIDirectDevice.h>
#import <driverkit/i386/IOPCIDeviceDescription.h>
#import <driverkit/i386/PCI.h>
#import <kernserv/prototypes.h>
#import <mach/vm_param.h>
#import <string.h>
#import <stdio.h>

extern void thread_wakeup(int);
extern int hz;
static int pciRead(void *ctx, unsigned offset, ohci_u32 *v)
{
    unsigned long data;
    OHCIController *c = ctx;
    if ([IODirectDevice getPCIConfigData:&data atRegister:offset
        withDeviceDescription:c->_pciDescription] != IO_R_SUCCESS) return 0;
    *v = data; return 1;
}
static int pciWrite(void *ctx, unsigned offset, ohci_u32 v)
{
    OHCIController *c = ctx;
    return [IODirectDevice setPCIConfigData:v atRegister:offset
        withDeviceDescription:c->_pciDescription] == IO_R_SUCCESS;
}
static const OHCIPCIOps pciOps = {pciRead, pciWrite};
/* Raw functions may only be called inside _boundaryLock. They never acquire
 * the event lock or wait for hardware. Observation-time loss is latched by
 * the calling portable policy or PlatformRead before unlocking. */
static ohci_u32 rawRead(OHCIController *c, unsigned offset)
{
    if (c->_interruptState.mmioLost || !c->_state.mmio ||
        (offset & 3) || offset + 4 > c->_state.mmioBytes) return 0xffffffffU;
    return *(volatile ohci_u32 *)(c->_state.mmio + offset);
}
static int rawWrite(OHCIController *c, unsigned offset, ohci_u32 v)
{
    if (c->_interruptState.mmioLost || !c->_state.mmio ||
        (offset & 3) || offset + 4 > c->_state.mmioBytes) return 0;
    *(volatile ohci_u32 *)(c->_state.mmio + offset) = v;
    return 1;
}
static ohci_u32 irqRead(void *ctx, unsigned r)
{ OHCIController *c = ctx; return rawRead(c, r); }
static int irqWrite(void *ctx, unsigned r, ohci_u32 v)
{ OHCIController *c = ctx; return rawWrite(c, r, v); }
static int irqPCIRead(void *ctx, ohci_u32 *v) { return pciRead(ctx, 4, v); }
static int irqPCIWrite(void *ctx, ohci_u32 v) { return pciWrite(ctx, 4, v); }
static int irqRearm(void *ctx) { return [(OHCIController *)ctx enableAllInterrupts] == IO_R_SUCCESS; }
static void irqPublish(void *ctx)
{ thread_wakeup((int)&((OHCIController *)ctx)->_serviceEvent); }
static const OHCIInterruptOps interruptOps = {
    irqRead, irqWrite, irqPCIRead, irqPCIWrite, irqRearm, irqPublish
};
static void completionLock(void *ctx)
{ OHCIController *c = ctx; [c->_eventLock lock]; [c->_boundaryLock lock]; }
static void completionUnlock(void *ctx)
{ OHCIController *c = ctx; [c->_boundaryLock unlock]; [c->_eventLock unlock]; }
static int completionReady(void *ctx)
{
    OHCIController *c = ctx;
    return OHCICompletionWorkReady(c->_startupReady, c->_interruptState.work,
        c->_interruptState.stopping, c->_state.fatal, c->_state.rescan,
        c->_state.scheduleBusy);
}
static void completionPrepare(void *ctx)
{
    OHCIController *c = ctx;
    assert_wait((int)&c->_serviceEvent, FALSE);
    thread_set_timeout(1);
}
static void completionBlock(void *ctx) { (void)ctx; thread_block(); }
static const OHCICompletionWaitOps completionWaitOps = {
    completionLock, completionUnlock, completionReady, completionPrepare, completionBlock
};
static void completionThread(void *ctx) { [(OHCIController *)ctx runCompletionLoop]; IOExitThread(); }
static void managementThread(void *ctx) { [(OHCIController *)ctx runManagementLoop]; IOExitThread(); }
static void retryThread(void *ctx) { [(OHCIController *)ctx runRetryLoop]; IOExitThread(); }
static void keyboardThread(void *ctx) { [(OHCIController *)ctx runInputLoop:0]; IOExitThread(); }
static void pointerThread(void *ctx) { [(OHCIController *)ctx runInputLoop:1]; IOExitThread(); }

@implementation OHCIController
- (IOReturn)getIntValues:(unsigned *)values forParameter:(IOParameterName)name count:(unsigned *)count
{
    unsigned i;
    if (strcmp(name,OHCI_RUNTIME_PARAMETER)) return [super getIntValues:values forParameter:name count:count];
    if (*count<OHCI_RUNTIME_WORDS) return IO_R_INVALID_ARG;
    bzero(values,OHCI_RUNTIME_WORDS*sizeof(*values));
    [_eventLock lock]; [_boundaryLock lock];
    values[0]=1; values[1]=_state.running && !_state.fatal; values[2]=_state.fatal;
    values[3]=_interruptState.mode; values[4]=_state.maxPorts;
    for (i=0;i<OHCI_MAX_DEVICES;i++) if (_state.devices[i].used) values[5]++;
    values[6]=_state.allocations; values[7]=_state.keyboardReports; values[8]=_state.mouseReports;
    values[9]=_state.transferEvents; values[10]=_state.errors; values[11]=_state.timeouts;
    values[12]=_interruptState.callbacks; values[13]=_interruptState.foreign; values[14]=_interruptState.failures;
    values[15]=_interruptState.interruptDebt; values[16]=_interruptState.dmaDebt; values[17]=_interruptState.rearmDebt;
    values[20]=_state.scheduleChanges; values[21]=_state.keyboardSlot; values[22]=_state.pointerSlot;
    values[23]=_state.faultDetail; values[24]=_irq; values[25]=_state.lastCommand; values[26]=_state.lastStatus;
    [_boundaryLock unlock]; [_eventLock unlock];
    [_inputLocks[0] lock]; values[18]=_inputQueues[0].droppedKeyboard; [_inputLocks[0] unlock];
    [_inputLocks[1] lock]; values[19]=_inputQueues[1].droppedPointer; [_inputLocks[1] unlock];
    *count=OHCI_RUNTIME_WORDS; return IO_R_SUCCESS;
}
+ (BOOL)probe:description
{
    OHCIController *c=[[self alloc] initFromDeviceDescription:description];
    if (!c) return NO;
    if (c->_initializationFailed) return YES;
    if (![c registerDevice]) { [c free]; return YES; }
    return YES;
}
- (IOReturn)startIOThread
{ return _deferIOThread ? IO_R_SUCCESS : [super startIOThread]; }
- initFromDeviceDescription:description
{
    IOPCIConfigSpace pci;
    IORange range;
    unsigned address, bytes, i, mode, mapStart, mapBytes, mapOffset, usbInput = 1;
    ohci_u32 routing;
    const char *value;
    IOConfigTable *table = [description configTable];
    _pciDescription = description;
    _state.owner = self;
    _interruptState.ops = &interruptOps; _interruptState.context = self;
    _boundaryLock = [[NXLock alloc] init]; _eventLock = [[NXLock alloc] init];
    if (!_boundaryLock || !_eventLock) return [self failInitializationAt:__LINE__];
    bzero(&pci, sizeof(pci));
    if ([IODirectDevice getPCIConfigSpace:&pci withDeviceDescription:description] != IO_R_SUCCESS ||
        !OHCICoreSupportsPCIClass(pci.ClassCode)) return [self failInitializationAt:__LINE__];
    value = [table valueForStringKey:"Interrupt Mode"];
    mode = OHCIInterruptModeParse(value);
    if (value) [table freeString:value];
    value = [table valueForStringKey:"Polling Only"];
    if (value) { [table freeString:value]; mode = OHCI_MODE_INVALID; }
    if (!mode) { IOLog("OHCI: Interrupt Mode must be INTx or Polling\n"); return [self failInitializationAt:__LINE__]; }
    _interruptState.mode = mode;
    value = [table valueForStringKey:"USB Input"];
    if (value) {
        if (!strcmp(value, "No") || !strcmp(value, "NO")) usbInput = 0;
        else if (strcmp(value, "Yes") && strcmp(value, "YES")) {
            [table freeString:value];
            IOLog("OHCI: USB Input must be Yes or No\n");
            return [self failInitializationAt:__LINE__];
        }
        [table freeString:value];
    }
    if (mode == OHCI_MODE_INTX) {
        value = [table valueForStringKey:"Share IRQ Levels"];
        i = value && (!strcmp(value, "Yes") || !strcmp(value, "YES"));
        if (value) [table freeString:value];
        if (!i || !pciRead(self, 0x3c, &routing) || !(routing & 0xff00) ||
            ((routing >> 8) & 255) > 4 || !(routing & 255) || (routing & 255) >= 16 || (routing & 255) == 2)
            return [self failInitializationAt:__LINE__];
        _irq = routing & 255;
        if ([description setInterruptList:&_irq num:1] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    } else if ([description setInterruptList:0 num:0] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    if (!OHCIPCIBar(&pciOps, self, &address, &bytes)) return [self failInitializationAt:__LINE__];
    /* DriverKit 4.2 maps whole Mach pages and does not add the BAR's page
     * offset. In particular a 4 KiB BAR can start halfway into an 8 KiB page. */
    if (!OHCIPCIMapRange(address, bytes, page_size, &mapStart, &mapBytes, &mapOffset))
        return [self failInitializationAt:__LINE__];
    range.start = mapStart; range.size = mapBytes;
    if ([description setMemoryRangeList:&range num:1] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    if (!IOForkThread(retryThread,self)) return [self failInitializationAt:__LINE__];
    _workersStarted=1; _deferIOThread=YES;
    if (![super initFromDeviceDescription:description]) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    _pciOwned=1;
    i=OHCIPCICommand(&pciOps,self,OHCI_PCI_INT_DISABLE|OHCI_PCI_MEMORY,OHCI_PCI_MASTER) &&
        OHCIPCIDisableMessages(&pciOps,self);
    if (i) _interruptState.fenced=_interruptState.dmaFenced=1;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    _interruptState.participant=mode==OHCI_MODE_INTX;
    _deferIOThread=NO; _ioThreadStarted=YES;
    if ([super startIOThread]!=IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    {
        char name[24]; id existing; unsigned unit=0;
        do { sprintf(name,"ohci%u",unit++); }
        while (IOGetObjectForDeviceName(name,&existing)==IO_R_SUCCESS);
        [self setUnit:unit-1]; [self setName:name]; [self setDeviceKind:"OHCIController"];
    }
    if (mode == OHCI_MODE_INTX) {
        unsigned *list = [[self deviceDescription] interruptList];
        if ([[self deviceDescription] numInterrupts] != 1 || !list || list[0] != _irq ||
            [self interruptPort] == PORT_NULL) return [self failInitializationAt:__LINE__];
    } else if ([[self deviceDescription] numInterrupts]) return [self failInitializationAt:__LINE__];
    _mappedBytes = mapBytes;
    if ([self mapMemoryRange:0 to:&_mappedAddress findSpace:YES cache:IO_CacheOff] != IO_R_SUCCESS ||
        !_mappedAddress) return [self failInitializationAt:__LINE__];
    _state.mmio = (volatile ohci_u8 *)(_mappedAddress + mapOffset); _state.mmioBytes = bytes;
    for (i = 0; i < 2; i++) {
        _inputLocks[i] = [[NXLock alloc] init];
        OHCIInputQueueInitialize(&_inputQueues[i]);
        if (!_inputLocks[i]) return [self failInitializationAt:__LINE__];
    }
    _state.inputEnabled=usbInput;
    [_eventLock lock];
    i = OHCICoreInitialize(&_state);
    [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    if (!IOForkThread(completionThread, self)) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    i = OHCIPCICommand(&pciOps, self, OHCI_PCI_MASTER | OHCI_PCI_MEMORY | OHCI_PCI_INT_DISABLE, 0);
    if (i) _interruptState.dmaFenced = 0;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_eventLock lock]; i = OHCICoreStart(&_state); [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    if (mode == OHCI_MODE_INTX) {
        _interruptState.participant = 1; _interruptState.rearmDebt = 1;
        /* Pending port status may intentionally defer opening to the worker. */
        OHCIInterruptRestore(&_interruptState);
    }
    _startupReady = 1;
    [_boundaryLock unlock];
    irqPublish(self);
    [_eventLock lock]; i = OHCICoreServicePorts(&_state); [_eventLock unlock];
    if (!i || _interruptState.stopping) return [self failInitializationAt:__LINE__];
    if (!IOForkThread(managementThread, self) || !IOForkThread(keyboardThread, self) ||
        !IOForkThread(pointerThread, self)) return [self failInitializationAt:__LINE__];
    if (usbInput) {
        _keyboard = [[OHCIUSBKeyboard alloc] initWithController:self];
        if (!_keyboard || ![OHCIUSBPointer attachToController:self description:description])
            return [self failInitializationAt:__LINE__];
        _pointer = [OHCIUSBPointer activePointerDevice];
    } else IOLog("OHCI: USB input disabled by configuration\n");
    _inputReady = 1;
    IOLog("OHCI %s: %04x:%04x MMIO %x, %u ports, %s%s\n", OHCI_VERSION,
        pci.VendorID, pci.DeviceID, address, _state.maxPorts,
        mode == OHCI_MODE_INTX ? "INTx" : "Polling", mode == OHCI_MODE_INTX ? " shared IRQ" : " 5 ms");
    return self;
}
- failInitialization
{ return [self failInitializationAt:0]; }
- failInitializationAt:(unsigned)line
{
    IOLog("OHCI: initialization failed at line %u fatal=%u lost=%u\n",line,_state.fatal,_interruptState.mmioLost);
    if (_pciOwned) { _initializationFailed=YES; [self free]; return self; }
    [self free]; return nil;
}
- (void)interruptOccurred
{
    /* x86 locked admission precedes any blocking lock acquisition. Pinned
     * lifetime keeps both this token and queued callbacks valid until reboot. */
    __asm__ volatile("lock; incl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock lock];
    OHCIInterruptCallback(&_interruptState);
    __asm__ volatile("lock; decl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock unlock];
}
- (void)runRetryLoop
{
    for (;;) {
        [_boundaryLock lock];
        if (_pciOwned) OHCIInterruptRetry(&_interruptState);
        /* Watchdog detects a stuck asserted source, never services transfers
         * or calls clients. INTx failure goes offline, never silently polls. */
        if (_startupReady)
            OHCIInterruptWatchdog(&_interruptState, OHCIPlatformMilliseconds());
        [_boundaryLock unlock];
        IOSleep(5);
    }
}
- (void)runCompletionLoop
{
    for (;;) {
        unsigned causes = 0, work = 0, fatal = 0;
        [_boundaryLock lock];
        if (_startupReady && !_interruptState.stopping) {
            if (_interruptState.mode == OHCI_MODE_POLLING) OHCIInterruptPoll(&_interruptState);
            work = _interruptState.work;
            if (work) causes = OHCIInterruptTakeWork(&_interruptState);
        }
        [_boundaryLock unlock];
        if (_startupReady) {
            [_eventLock lock];
            if (_interruptState.stopping) {
                OHCICoreFail(&_state, "interrupt stopped", _interruptState.mmioLost);
            } else if (work || _state.rescan) OHCICoreService(&_state, causes);
            fatal = _state.fatal;
            [_eventLock unlock];
            if (fatal) { [self containFatalController]; return; }
            [_boundaryLock lock];
            if (!_state.scheduleBusy && !_interruptState.stopping && !_interruptState.active)
                OHCIInterruptRestore(&_interruptState);
            [_boundaryLock unlock];
        }
        /* Register under both publication locks before blocking. A wake
         * after unlock cancels the registered wait, including before block.
         * The timeout keeps startup/retry progress; idle INTx does not poll. */
        if (_interruptState.mode == OHCI_MODE_POLLING) IOSleep(5);
        else OHCICompletionWait(self, &completionWaitOps);
    }
}
- (void)runManagementLoop
{
    for (;;) {
        unsigned fatal, stopping;
        [_eventLock lock];
        if (!_interruptState.stopping) {
            OHCICoreServicePorts(&_state);
            if (_ledUpdatePending) {
                unsigned leds = _pendingLEDs; _ledUpdatePending = 0;
                OHCICoreSetKeyboardLEDs(&_state, leds);
            }
        }
        fatal = _state.fatal; stopping = _interruptState.stopping;
        [_eventLock unlock];
        if (fatal) [self containFatalController];
        if (fatal || stopping) return;
        IOSleep(10);
    }
}
- (void)containFatalController
{
    unsigned i;
    [_boundaryLock lock];
    if (!_interruptState.stopping) OHCIInterruptStop(&_interruptState);
    else OHCIInterruptRetry(&_interruptState);
    [_boundaryLock unlock];
    [_eventLock lock];
    OHCICoreFail(&_state, "controller containment", _interruptState.mmioLost);
    if (_contained) { [_eventLock unlock]; return; }
    _contained = 1;
    [_eventLock unlock];
    for (i = 0; i < 2; i++) {
        [_inputLocks[i] lock];
        OHCIInputQueueDiscardPending(&_inputQueues[i]); _releasePending[i] = 1;
        [_inputLocks[i] unlock];
    }
}
- (void)handleKeyboardReport:(const unsigned char *)report length:(unsigned)length
{
    [_inputLocks[0] lock];
    if (!OHCIInputQueueEnqueue(&_inputQueues[0], OHCI_INPUT_KEYBOARD, report, length, 0))
        _releasePending[0] = 1;
    [_inputLocks[0] unlock];
}
- (void)handlePointerReport:(const unsigned char *)report length:(unsigned)length
{
    [_inputLocks[1] lock];
    if (!OHCIInputQueueEnqueue(&_inputQueues[1], OHCI_INPUT_POINTER, report, length, 0))
        _releasePending[1] = 1;
    [_inputLocks[1] unlock];
}
- (void)runInputLoop:(unsigned)which
{
    OHCIInputEvent event;
    static const unsigned char released[8] = {0,0,0,0,0,0,0,0};
    for (;;) {
        int have = 0, release = 0;
        [_inputLocks[which] lock];
        if (_inputReady) {
            release = _releasePending[which]; _releasePending[which] = 0;
            if (release) OHCIInputQueueDiscardPending(&_inputQueues[which]);
            else have = OHCIInputQueueDequeue(&_inputQueues[which], &event);
        }
        [_inputLocks[which] unlock];
        if (release || have) {
            if (which) [_pointer consumeReport:release ? released : event.report length:release ? 3 : event.length];
            else [_keyboard consumeReport:release ? released : event.report length:release ? 8 : event.length];
        } else IOSleep(5);
    }
}
- (void)queueKeyboardLEDs:(unsigned char)leds
{ [_eventLock lock]; _pendingLEDs = leds; _ledUpdatePending = 1; [_eventLock unlock]; }
- free
{
    unsigned i;
    if (_boundaryLock && _pciOwned) {
        [_boundaryLock lock];
        OHCIInterruptStop(&_interruptState);
        [_boundaryLock unlock];
    }
    /* IODirectDevice, its I/O task, native providers and queued callbacks
     * have no usable final-drain API in this target. Never release their
     * object, BAR, locks or DMA after superclass publication. */
    if (_ioThreadStarted || _workersStarted || _interruptState.interruptDebt || _interruptState.dmaDebt) {
        IOLog("OHCI: offline resources and retry worker retained until reboot\n");
        return self;
    }
    for (i = 0; i < 2; i++) [_inputLocks[i] free];
    [_eventLock free]; [_boundaryLock free];
    return [super free];
}
@end

ohci_u32 OHCIPlatformRead(OHCIControllerState *s, ohci_u32 offset)
{
    OHCIController *c = s->owner; ohci_u32 v;
    [c->_boundaryLock lock];
    v = c->_interruptState.stopping ? 0xffffffffU : rawRead(c, offset);
    if (v == 0xffffffffU && !c->_interruptState.mmioLost && !c->_interruptState.stopping)
        OHCIInterruptLost(&c->_interruptState);
    [c->_boundaryLock unlock]; return v;
}
int OHCIPlatformWrite(OHCIControllerState *s, ohci_u32 offset, ohci_u32 v)
{
    OHCIController *c = s->owner; int ok;
    [c->_boundaryLock lock];
    ok = !c->_interruptState.stopping && rawWrite(c, offset, v);
    [c->_boundaryLock unlock]; return ok;
}
int OHCIPlatformPCIRead(OHCIControllerState *s, unsigned offset, ohci_u32 *v)
{ OHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciRead(c, offset, v); [c->_boundaryLock unlock]; return ok; }
int OHCIPlatformPCIWrite(OHCIControllerState *s, unsigned offset, ohci_u32 v)
{ OHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciWrite(c, offset, v); [c->_boundaryLock unlock]; return ok; }
void *OHCIPlatformAllocate(ohci_u32 bytes) { return IOMalloc(bytes); }
void OHCIPlatformFree(void *p, ohci_u32 bytes) { IOFree(p, bytes); }
ohci_u32 OHCIPlatformPhysical(void *p)
{
    vm_address_t address = 0;
    if (IOPhysicalFromVirtual(IOVmTaskSelf(), (vm_address_t)p, &address) != IO_R_SUCCESS) return 0;
    return address;
}
ohci_u64 OHCIPlatformMilliseconds(void)
{ ns_time_t now; IOGetTimestamp(&now); return (ohci_u64)now / 1000000ULL; }
void OHCIPlatformPause(OHCIControllerState *s, unsigned ms)
{ OHCIController *c = s->owner; [c->_eventLock unlock]; IOSleep(ms); [c->_eventLock lock]; }
void OHCIPlatformDelay(OHCIControllerState *s, unsigned us)
{ (void)s; IODelay(us); }
void OHCIPlatformWake(void *c) { irqPublish(c); }
void OHCIPlatformKeyboardReport(void *c, const ohci_u8 *b, ohci_u32 n)
{ [(OHCIController *)c handleKeyboardReport:b length:n]; }
void OHCIPlatformPointerReport(void *c, const ohci_u8 *b, ohci_u32 n)
{ [(OHCIController *)c handlePointerReport:b length:n]; }
ohci_u32 OHCIPlatformTakeDone(OHCIControllerState *s)
{
    OHCIController *c=s->owner;
    unsigned head=0,status;
    [c->_boundaryLock lock];
    if (!c->_interruptState.stopping && !c->_interruptState.mmioLost) {
        status=rawRead(c,OHCI_STS);
        if (status==0xffffffffU) OHCIInterruptLost(&c->_interruptState);
        else if (status&OHCI_INT_WDH) {
            OHCIHCCA *h=s->hcca.virtualAddress;
            head=h->done&~1U; h->done=0;
            __asm__ volatile("" : : : "memory");
            rawWrite(c,OHCI_STS,OHCI_INT_WDH);
            if (rawRead(c,OHCI_STS)==0xffffffffU) OHCIInterruptLost(&c->_interruptState);
            c->_interruptState.watchdogPending=0;
        }
    }
    [c->_boundaryLock unlock]; return head;
}
