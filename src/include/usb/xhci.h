#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/**
 * xHCI Specification References:
 * - Section 5: Register Interface
 * - Section 6: Data Structures
 */

// --- Capability Registers (Section 5.3) ---
typedef struct {
    uint8_t  cap_length;    // Capability Register Length
    uint8_t  reserved;
    uint16_t hci_version;   // Interface Version Number
    uint32_t hcs_params1;   // Structural Parameters 1
    uint32_t hcs_params2;   // Structural Parameters 2
    uint32_t hcs_params3;   // Structural Parameters 3
    uint32_t hcc_params1;   // Capability Parameters 1
    uint32_t dboff;         // Doorbell Offset
    uint32_t rtsoff;        // Runtime Register Space Offset
    uint32_t hcc_params2;   // Capability Parameters 2
} __attribute__((packed)) xhci_cap_regs_t;

// --- Operational Registers (Section 5.4) ---
typedef struct {
    uint32_t usb_cmd;       // USB Command
    uint32_t usb_sts;       // USB Status
    uint32_t page_size;     // Page Size
    uint8_t  reserved1[8];
    uint32_t dn_ctrl;       // Device Notification Control
    uint64_t crcr;          // Command Ring Control Register
    uint8_t  reserved2[16];
    uint64_t dcbaap;        // Device Context Base Address Array Pointer
    uint32_t config;        // Configure
} __attribute__((packed)) xhci_op_regs_t;

// --- Runtime Registers (Section 5.5) ---
typedef struct {
    uint32_t iman;          // Interrupter Management
    uint32_t imod;          // Interrupter Moderation
    uint32_t erstsz;        // Event Ring Segment Table Size
    uint32_t reserved;
    uint64_t erstba;        // Event Ring Segment Table Base Address
    uint64_t erdp;          // Event Ring Dequeue Pointer
} __attribute__((packed)) xhci_interrupter_regs_t;

typedef struct {
    uint32_t mfindex;       // Microframe Index
    uint8_t  reserved[28];
    xhci_interrupter_regs_t interrupters[1024];
} __attribute__((packed)) xhci_runtime_regs_t;

// --- TRB (Transfer Request Block) Structure (Section 6.4) ---
typedef struct {
    uint64_t data;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

// TRB Types
#define TRB_TYPE_NORMAL             1
#define TRB_TYPE_SETUP_STAGE        2
#define TRB_TYPE_DATA_STAGE         3
#define TRB_TYPE_STATUS_STAGE       4
#define TRB_TYPE_LINK               6
#define TRB_TYPE_ENABLE_SLOT_CMD    9
#define TRB_TYPE_ADDRESS_DEVICE_CMD 11
#define TRB_TYPE_CONFIGURE_EP_CMD   12

#define TRB_TYPE_PORT_STATUS_CHANGE 34
#define TRB_TYPE_COMMAND_COMPLETION 33
#define TRB_TYPE_TRANSFER_EVENT     32

// USB Command Register Bits
#define USB_CMD_RS    (1 << 0) // Run/Stop
#define USB_CMD_HCRST (1 << 1) // Host Controller Reset
#define USB_CMD_INTE  (1 << 2) // Interrupter Enable

// USB Status Register Bits
#define USB_STS_HCH   (1 << 0) // HCHalted

void xhci_init(uint64_t mmio_base);

#endif
