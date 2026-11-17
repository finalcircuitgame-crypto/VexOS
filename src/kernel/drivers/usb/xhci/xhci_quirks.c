#include "xhci_quirks.h"

/*
 * Intel xHCI controllers expose the USB2PR/USB3PR routing registers in PCI
 * extended configuration space on supported generations. This preserves
 * the existing Intel-only routing workaround in pci.c behind an explicit
 * quirk flag. Do not add speculative reset/timing workarounds here.
 */
static const xhci_quirk_entry_t g_xhci_quirk_table[] = {
    { 0x8086u, 0u, 0x00u, 0xFFu,
      XHCI_QUIRK_INTEL_EXTENDED_ROUTING, "Intel xHCI port routing" },
};

static xhci_quirks_t g_detected_quirks;

const xhci_quirks_t *xhci_quirks_detect(uint16_t vendor_id,
                                        uint16_t device_id,
                                        uint8_t revision_id) {
    g_detected_quirks.vendor_id = vendor_id;
    g_detected_quirks.device_id = device_id;
    g_detected_quirks.revision_id = revision_id;
    g_detected_quirks.flags = XHCI_QUIRK_NONE;
    g_detected_quirks.name = "generic";

    for (unsigned int i = 0;
         i < sizeof(g_xhci_quirk_table) / sizeof(g_xhci_quirk_table[0]);
         ++i) {
        const xhci_quirk_entry_t *entry = &g_xhci_quirk_table[i];
        if (entry->vendor_id != vendor_id)
            continue;
        if (entry->device_id != 0u && entry->device_id != device_id)
            continue;
        if (revision_id < entry->revision_min ||
            revision_id > entry->revision_max)
            continue;

        g_detected_quirks.flags |= entry->flags;
        g_detected_quirks.name = entry->name;
    }

    return &g_detected_quirks;
}
