#include "pci.h"
#include "bootlog.h"
#include "ahci.h"
#include "usb/xhci.h"

// I/O ports for PCI
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// Forward declaration
void serial_print(const char *str);

void print_hex(uint64_t val) {
    for (int i = 7; i >= 0; i--) {
        char c = (val >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        serial_print(s);
    }
}

static void print_dec_u32(uint32_t v) {
    char s[12];
    int n = 0;
    char buf[12];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = (v % 10) + '0'; v /= 10; }
    while (i > 0) s[n++] = buf[--i];
    s[n] = 0;
    serial_print(s);
}

static void pci_init_ahci(uint8_t bus, uint8_t slot, uint8_t func) {
    if (ahci_is_ready())
        return;

    uint32_t bar5 = pci_config_read_dword(bus, slot, func, 0x24);
    if (bar5 == 0 || bar5 == 0xFFFFFFFFu || (bar5 & 1u)) {
        serial_print("[PCI] AHCI BAR5 is not a usable MMIO BAR\n");
        return;
    }

    // ABAR is BAR5. A 64-bit memory BAR cannot legally start in the final
    // BAR slot, so this driver accepts the standard 32-bit AHCI ABAR only.
    if ((bar5 & 0x6u) != 0) {
        serial_print("[PCI] Unsupported AHCI BAR5 memory type\n");
        return;
    }

    uint64_t abar = (uint64_t)(bar5 & 0xFFFFFFF0u);
    if (abar == 0) {
        serial_print("[PCI] AHCI controller has no assigned ABAR\n");
        return;
    }

    // Enable PCI memory decoding and bus mastering for the HBA DMA engine.
    // Write zero to the status half: PCI status bits are write-one-to-clear.
    uint32_t command_status = pci_config_read_dword(bus, slot, func, 0x04);
    uint16_t command = (uint16_t)command_status;
    command |= (uint16_t)(0x0002u | 0x0004u | 0x0400u); // MEM, MASTER, INTx disable
    pci_config_write_dword(bus, slot, func, 0x04, (uint32_t)command);

    serial_print("[PCI] Found AHCI controller; ABAR=");
    print_hex(abar);
    serial_print("\n");

    if (ahci_init(abar)) {
        serial_print("[PCI] AHCI storage initialized\n");
    } else {
        serial_print("[PCI] AHCI controller found, but no usable SATA disk initialized\n");
    }
}

/*
 * Probe storage controllers in a first pass. xHCI initialization can stall
 * while recovering a port, so mount writable storage and prepare boot logging
 * before touching any USB host controller.
 */
static void pci_probe_ahci_before_usb(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device =
                    pci_config_read_dword((uint8_t)bus, slot, func, 0);
                if ((uint16_t)vendor_device == 0xFFFFu)
                    continue;

                uint32_t class_info =
                    pci_config_read_dword((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (uint8_t)(class_info >> 24);
                uint8_t subclass = (uint8_t)(class_info >> 16);
                uint8_t prog_if = (uint8_t)(class_info >> 8);

                if (class_code == 0x01 && subclass == 0x06 &&
                    prog_if == 0x01 && !ahci_is_ready()) {
                    pci_init_ahci((uint8_t)bus, slot, func);
                }

                if (func == 0) {
                    uint32_t header_type =
                        pci_config_read_dword((uint8_t)bus, slot, func, 0x0C);
                    if (!((header_type >> 16) & 0x80u))
                        break;
                }
            }
        }
    }
}

static void xhci_force_usb2_routing(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint16_t vendor_id) {
    uint32_t usb2pr = pci_config_read_dword(bus, slot, func, 0xD0);
    uint32_t usb3pr = pci_config_read_dword(bus, slot, func, 0xD4);

    serial_print("[PCI] xHCI USB2PR=");
    print_hex(usb2pr);
    serial_print(" USB3PR=");
    print_hex(usb3pr);
    serial_print("\n");

    if (vendor_id != 0x8086) {
        return;
    }

    if (usb2pr != 0xFFFFFFFFu) {
        pci_config_write_dword(bus, slot, func, 0xD0, 0xFFFFFFFFu);
    }
    if (usb3pr != 0xFFFFFFFFu) {
        pci_config_write_dword(bus, slot, func, 0xD4, 0xFFFFFFFFu);
    }

    usb2pr = pci_config_read_dword(bus, slot, func, 0xD0);
    usb3pr = pci_config_read_dword(bus, slot, func, 0xD4);
    serial_print("[PCI] xHCI USB2PR(after)=");
    print_hex(usb2pr);
    serial_print(" USB3PR(after)=");
    print_hex(usb3pr);
    serial_print("\n");
}

void pci_enumerate() {
    serial_print("[PCI] Probing AHCI storage before USB initialization...\n");
    pci_probe_ahci_before_usb();
    (void)bootlog_prepare();

    serial_print("[PCI] Enumerating all buses...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read_dword((uint8_t)bus, slot, func, 0);
                if ((uint16_t)vendor_device == 0xFFFF) continue;

                uint16_t vendor_id = (uint16_t)(vendor_device & 0xFFFFu);
                uint16_t device_id = (uint16_t)((vendor_device >> 16) & 0xFFFFu);

                serial_print("[PCI] Device Found: ");
                print_hex(vendor_device);
                serial_print(" b=");
                print_dec_u32(bus);
                serial_print(" s=");
                print_dec_u32(slot);
                serial_print(" f=");
                print_dec_u32(func);

                uint32_t class_info = pci_config_read_dword((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_info >> 24) & 0xFF;
                uint8_t subclass   = (class_info >> 16) & 0xFF;
                uint8_t prog_if    = (class_info >> 8) & 0xFF;

                serial_print(" class=");
                print_hex(class_code);
                serial_print(" sub=");
                print_hex(subclass);
                serial_print(" if=");
                print_hex(prog_if);
                serial_print("\n");

                // SATA controller in AHCI programming-interface mode:
                // Base class 01h, subclass 06h, programming interface 01h.
                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    serial_print("[PCI] Found AHCI SATA controller!\n");
                    pci_init_ahci((uint8_t)bus, slot, func);
                }

                if (class_code == 0x0C && subclass == 0x03) {
                    if (prog_if == 0x30) serial_print("[PCI] USB Controller: xHCI\n");
                    else if (prog_if == 0x20) serial_print("[PCI] USB Controller: EHCI\n");
                    else if (prog_if == 0x10) serial_print("[PCI] USB Controller: OHCI\n");
                    else if (prog_if == 0x00) serial_print("[PCI] USB Controller: UHCI\n");
                    else serial_print("[PCI] USB Controller: Unknown\n");
                }

                // xHCI is Class 0x0C (Serial Bus), Subclass 0x03, Prog IF 0x30.
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x30) {
                    serial_print("[PCI] Found xHCI Controller!\n");
                    uint32_t bar0 = pci_config_read_dword((uint8_t)bus, slot, func, 0x10);
                    uint32_t bar1 = pci_config_read_dword((uint8_t)bus, slot, func, 0x14);
                    uint64_t mmio_base = bar0 & 0xFFFFFFF0;
                    if ((bar0 & 0x6) == 0x4) { // 64-bit BAR
                        mmio_base |= ((uint64_t)bar1 << 32);
                    }

                    // Enable bus mastering and MMIO for xHCI.
                    uint32_t command_status = pci_config_read_dword((uint8_t)bus, slot, func, 0x04);
                    uint16_t command = (uint16_t)command_status;
                    command |= (uint16_t)0x0006u;
                    pci_config_write_dword((uint8_t)bus, slot, func, 0x04, (uint32_t)command);

                    xhci_force_usb2_routing((uint8_t)bus, slot, func, vendor_id);
                    xhci_init(mmio_base);
                }

                // If not multi-function, don't check other functions.
                if (func == 0) {
                    uint32_t header_type = pci_config_read_dword((uint8_t)bus, slot, func, 0x0C);
                    if (!((header_type >> 16) & 0x80)) break;
                }
            }
        }
    }
    serial_print("[PCI] Enumeration Complete\n");
    (void)bootlog_flush();
}
