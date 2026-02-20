#include "pci.h"
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

void pci_enumerate() {
    serial_print("[PCI] Enumerating all buses...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read_dword(bus, slot, func, 0);
                if ((uint16_t)vendor_device == 0xFFFF) continue;

                serial_print("[PCI] Device Found: ");
                print_hex(vendor_device);
                serial_print(" b=");
                print_dec_u32(bus);
                serial_print(" s=");
                print_dec_u32(slot);
                serial_print(" f=");
                print_dec_u32(func);

                uint32_t class_info = pci_config_read_dword(bus, slot, func, 0x08);
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

                if (class_code == 0x0C && subclass == 0x03) {
                    if (prog_if == 0x30) serial_print("[PCI] USB Controller: xHCI\n");
                    else if (prog_if == 0x20) serial_print("[PCI] USB Controller: EHCI\n");
                    else if (prog_if == 0x10) serial_print("[PCI] USB Controller: OHCI\n");
                    else if (prog_if == 0x00) serial_print("[PCI] USB Controller: UHCI\n");
                    else serial_print("[PCI] USB Controller: Unknown\n");
                }

                // xHCI is Class 0x0C (Serial Bus), Subclass 0x03 (USB), Prog IF 0x30 (USB 3.0 xHCI)
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x30) {
                    serial_print("[PCI] Found xHCI Controller!\n");
                    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
                    uint32_t bar1 = pci_config_read_dword(bus, slot, func, 0x14);
                    uint64_t mmio_base = bar0 & 0xFFFFFFF0;
                    if ((bar0 & 0x6) == 0x4) { // 64-bit BAR
                        mmio_base |= ((uint64_t)bar1 << 32);
                    }
                    
                    // Enable Bus Mastering and MMIO
                    uint32_t command = pci_config_read_dword(bus, slot, func, 0x04);
                    pci_config_write_dword(bus, slot, func, 0x04, command | 0x06);

                    xhci_init(mmio_base);
                }

                // If not multi-function, don't check other functions
                if (func == 0) {
                    uint32_t header_type = pci_config_read_dword(bus, slot, func, 0x0C);
                    if (!((header_type >> 16) & 0x80)) break;
                }
            }
        }
    }
    serial_print("[PCI] Enumeration Complete\n");
}