#include "OHCIPCI.h"
#include "OHCIMemory.h"
int OHCIPCIMapRange(unsigned address, unsigned bytes, unsigned pageSize,
                    unsigned *start, unsigned *length, unsigned *offset)
{
    ohci_u64 end;
    if (!bytes || pageSize < 4096 || (pageSize & (pageSize - 1))) return 0;
    end = ((ohci_u64)address + bytes + pageSize - 1) & ~(ohci_u64)(pageSize - 1);
    *start = address & ~(pageSize - 1); *offset = address - *start;
    if (end > 0x100000000ULL || end - *start > 0xffffffffU) return 0;
    *length = (unsigned)(end - *start);
    return 1;
}
static int readpci(const OHCIPCIOps *o, void *c, unsigned r, ohci_u32 *v)
{ return o->read(c, r, v) && *v != 0xffffffffU; }
int OHCIPCICommand(const OHCIPCIOps *o, void *c, unsigned set, unsigned clear)
{
    ohci_u32 v, after;
    if (!readpci(o, c, 4, &v) || (v & 65535) == 65535) return 0;
    v = ((v & 65535) | set) & ~clear;
    /* Upper half is W1C PCI Status: never echo it. */
    return o->write(c, 4, v) && readpci(o, c, 4, &after) && (after & 65535) == v;
}
int OHCIPCIDisableMessages(const OHCIPCIOps *o, void *c)
{
    ohci_u32 v, after;
    unsigned seen[8], offset, count = 0, msi = 0, msix = 0;
    bzero(seen, sizeof(seen));
    if (!readpci(o, c, 4, &v)) return 0;
    if (!(v & (1U << 20))) return 1;
    if (!readpci(o, c, 0x34, &v)) return 0;
    offset = v & 255;
    while (offset) {
        unsigned next, mask = 0, set = 0;
        if (offset < 0x40 || offset > 0xfc || (offset & 3) || ++count > 48 ||
            (seen[offset / 32] & (1U << (offset % 32)))) return 0;
        seen[offset / 32] |= 1U << (offset % 32);
        if (!readpci(o, c, offset, &v)) return 0;
        next = (v >> 8) & 255;
        if ((v & 255) == 5) { if (msi++) return 0; mask = 1U << 16; }
        if ((v & 255) == 0x11) { if (msix++) return 0; mask = 1U << 31; set = 1U << 30; }
        if ((v & 255) == 1) {
            if (offset > 0xf8 || !readpci(o, c, offset + 4, &after) || (after & 3)) return 0;
        }
        if (mask && (!o->write(c, offset, (v & ~mask) | set) ||
            !readpci(o, c, offset, &after) || (after & mask) || (after & set) != set)) return 0;
        offset = next;
    }
    return 1;
}
/* OHCI's defined register aperture fits 256 bytes, including all 15 ports.
 * Decode it without destructive BAR sizing before DriverKit reserves it. */
int OHCIPCIBar(const OHCIPCIOps *o, void *c, unsigned *address, unsigned *bytes)
{
    ohci_u32 low, high;
    if (!readpci(o,c,0x10,&low) || (low&1) || ((low&6)!=0 && (low&6)!=4) ||
        !(low&~15U) || (low&0xf0U)) return 0;
    if ((low&6)==4 && (!readpci(o,c,0x14,&high) || high)) return 0;
    *address=low&~15U; *bytes=256; return 1;
}
