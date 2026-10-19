#ifndef OHCI_REGS_H
#define OHCI_REGS_H
#include "OHCITypes.h"
#define OHCI_REVISION 0x00
#define OHCI_CONTROL 0x04
#define OHCI_COMMAND 0x08
#define OHCI_STS 0x0c
#define OHCI_INTR 0x10
#define OHCI_INTR_DISABLE 0x14
#define OHCI_HCCA 0x18
#define OHCI_PERIOD_CURRENT 0x1c
#define OHCI_CONTROL_HEAD 0x20
#define OHCI_CONTROL_CURRENT 0x24
#define OHCI_BULK_HEAD 0x28
#define OHCI_BULK_CURRENT 0x2c
#define OHCI_DONE_HEAD 0x30
#define OHCI_FRAME_INTERVAL 0x34
#define OHCI_FRAME_NUMBER 0x3c
#define OHCI_PERIOD_START 0x40
#define OHCI_LS_THRESHOLD 0x44
#define OHCI_RH_A 0x48
#define OHCI_RH_B 0x4c
#define OHCI_RH_STATUS 0x50
#define OHCI_PORT(p) (0x50U+4U*(p))
#define OHCI_CTL_PLE 0x04U
#define OHCI_CTL_CLE 0x10U
#define OHCI_CTL_OPERATIONAL 0x80U
#define OHCI_CTL_IR 0x100U
#define OHCI_CMD_RESET 1U
#define OHCI_CMD_CLF 2U
#define OHCI_CMD_OCR 8U
#define OHCI_INT_WDH 2U
#define OHCI_INT_UE 0x10U
#define OHCI_INT_RHSC 0x40U
#define OHCI_INT_MIE 0x80000000U
#define OHCI_STS_W1C 0x4000007fU
/* Root ports are scanned by management; avoid a sticky RHSC interrupt storm. */
#define OHCI_INTR_DEFAULT (OHCI_INT_WDH|OHCI_INT_UE)
#define OHCI_PORT_CONNECT 1U
#define OHCI_PORT_ENABLE 2U
#define OHCI_PORT_SUSPEND 4U
#define OHCI_PORT_OC 8U
#define OHCI_PORT_RESET 0x10U
#define OHCI_PORT_POWER 0x100U
#define OHCI_PORT_LOW 0x200U
#define OHCI_PORT_CSC 0x10000U
#define OHCI_PORT_CHANGES 0x1f0000U
#define OHCI_ED_SKIP 0x4000U
#define OHCI_ED_LOW 0x2000U
#define OHCI_ED_HALTED 1U
#define OHCI_ED_CARRY 2U
#define OHCI_TD_ROUND 0x40000U
#define OHCI_TD_IN 0x100000U
#define OHCI_TD_OUT 0x80000U
#define OHCI_TD_DATA0 0x2000000U
#define OHCI_TD_DATA1 0x3000000U
#define OHCI_TD_NOT_ACCESSED 0xf0000000U
#define OHCI_PCI_MASTER 4U
#define OHCI_PCI_MEMORY 2U
#define OHCI_PCI_INT_DISABLE 0x400U
#define OHCI_SPEED_FULL 0U
#define OHCI_SPEED_LOW 1U
#define OHCI_SPEED_HIGH 2U
typedef struct OHCIED { volatile ohci_u32 flags, tail, head, next; } OHCIED;
typedef struct OHCITD { volatile ohci_u32 flags, cbp, next, end; } OHCITD;
typedef struct OHCIHCCA {
    volatile ohci_u32 interrupt[32];
    volatile ohci_u16 frame, pad;
    volatile ohci_u32 done;
    ohci_u8 reserved[120];
} OHCIHCCA;
typedef char ohci_ed_size[sizeof(OHCIED)==16 ? 1 : -1];
typedef char ohci_td_size[sizeof(OHCITD)==16 ? 1 : -1];
typedef char ohci_hcca_size[sizeof(OHCIHCCA)==256 ? 1 : -1];
#endif
