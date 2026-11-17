#ifndef XHCI_QUIRKS_H
#define XHCI_QUIRKS_H

#include <stdint.h>

/*
 * Controller quirks are matched by PCI identity. A device_id of 0 matches
 * every device from the specified vendor; revision bounds are inclusive.
 * Keep the table conservative: only add workarounds backed by hardware tests.
 */
#define XHCI_QUIRK_NONE                 0u
#define XHCI_QUIRK_INTEL_EXTENDED_ROUTING (1u << 0)

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id; /* 0 = any device from vendor */
    uint8_t revision_min;
    uint8_t revision_max;
    uint32_t flags;
    const char *name;
} xhci_quirk_entry_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision_id;
    uint32_t flags;
    const char *name;
} xhci_quirks_t;

const xhci_quirks_t *xhci_quirks_detect(uint16_t vendor_id,
                                        uint16_t device_id,
                                        uint8_t revision_id);

#endif
