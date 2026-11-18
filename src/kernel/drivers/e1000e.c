#include "e1000e.h"
#include "pci.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);
void print_hex(uint64_t value);

#define E1000E_VENDOR_ID       0x8086u
#define E1000E_DEVICE_ID       0x10D3u
#define E1000E_MMIO_BYTES      0x6000u
#define E1000E_RING_COUNT      16u
#define E1000E_BUFFER_BYTES    2048u
#define E1000E_FRAME_MAX       1518u
#define E1000E_TIMEOUT         20000000u

/* 82574L register offsets. */
#define REG_CTRL       0x0000u
#define REG_STATUS     0x0008u
#define REG_EECD       0x0010u
#define REG_EERD       0x0014u
#define REG_CTRL_EXT   0x0018u
#define REG_ICR        0x00C0u
#define REG_ITR        0x00C4u
#define REG_ICS        0x00C8u
#define REG_IMS        0x00D0u
#define REG_IMC        0x00D8u
#define REG_RCTL       0x0100u
#define REG_TCTL       0x0400u
#define REG_TIPG       0x0410u
#define REG_RDBAL      0x2800u
#define REG_RDBAH      0x2804u
#define REG_RDLEN      0x2808u
#define REG_RDH        0x2810u
#define REG_RDT        0x2818u
#define REG_RDTR       0x2820u
#define REG_RXDCTL     0x2828u
#define REG_TDBAL      0x3800u
#define REG_TDBAH      0x3804u
#define REG_TDLEN      0x3808u
#define REG_TDH        0x3810u
#define REG_TDT        0x3818u
#define REG_TXDCTL     0x3828u
#define REG_RAL0       0x5400u
#define REG_RAH0       0x5404u

#define CTRL_RST       (1u << 26)
#define CTRL_SLU       (1u << 6)
#define STATUS_LU      (1u << 1)
#define RCTL_EN        (1u << 1)
#define RCTL_BAM       (1u << 15)
#define RCTL_SECRC     (1u << 26)
#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)
#define TXD_STAT_DD    (1u << 0)
#define RXD_STAT_DD    (1u << 0)
#define RXD_STAT_EOP   (1u << 1)
#define TXD_CMD_EOP    (1u << 0)
#define TXD_CMD_IFCS   (1u << 1)
#define TXD_CMD_RS     (1u << 3)

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint16_t checksum_offset;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000e_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000e_tx_desc_t;

/*
 * The kernel image is loaded at fixed low physical addresses and identity
 * mapped. These static DMA objects therefore have identical CPU/physical
 * addresses. Keep descriptors naturally aligned as required by the device.
 */
static e1000e_rx_desc_t rx_ring[E1000E_RING_COUNT] __attribute__((aligned(128)));
static e1000e_tx_desc_t tx_ring[E1000E_RING_COUNT] __attribute__((aligned(128)));
static uint8_t rx_buffers[E1000E_RING_COUNT][E1000E_BUFFER_BYTES]
    __attribute__((aligned(16)));
static uint8_t tx_buffers[E1000E_RING_COUNT][E1000E_BUFFER_BYTES]
    __attribute__((aligned(16)));

static volatile uint32_t *mmio;
static uint8_t mac[6];
static uint32_t rx_next;
static uint32_t tx_next;
static int ready;

static void zero_bytes(void *ptr, size_t bytes) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}
static void copy_bytes(void *dst, const void *src, size_t bytes) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < bytes; ++i) d[i] = s[i];
}
static uint32_t reg_read(uint32_t offset) {
    return mmio[offset / 4u];
}
static void reg_write(uint32_t offset, uint32_t value) {
    mmio[offset / 4u] = value;
}
static int wait_reg_clear(uint32_t offset, uint32_t mask) {
    for (uint32_t i = 0; i < E1000E_TIMEOUT; ++i)
        if ((reg_read(offset) & mask) == 0) return 1;
    return 0;
}
static int dma_address_ok(const void *ptr, size_t bytes) {
    uint64_t address = (uint64_t)(uintptr_t)ptr;
    return bytes && address <= 0xFFFFFFFFFFFFFFFFull - bytes &&
           address < (1ull << 52);
}
static void log_mac(void) {
    serial_print("[E1000E] MAC=");
    for (uint32_t i = 0; i < 6; ++i) {
        const char hex[] = "0123456789ABCDEF";
        char out[2] = {hex[mac[i] >> 4], 0};
        serial_print(out);
        out[0] = hex[mac[i] & 15u];
        serial_print(out);
        serial_print(i == 5 ? "\n" : ":");
    }
}

int e1000e_init(uint8_t bus, uint8_t slot, uint8_t function) {
    if (ready) return 1;

    uint32_t id = pci_config_read_dword(bus, slot, function, 0x00u);
    if ((uint16_t)id != E1000E_VENDOR_ID ||
        (uint16_t)(id >> 16) != E1000E_DEVICE_ID) {
        serial_print("[E1000E] Unsupported PCI identity\n");
        return 0;
    }

    uint32_t bar0 = pci_config_read_dword(bus, slot, function, 0x10u);
    uint32_t bar1 = pci_config_read_dword(bus, slot, function, 0x14u);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu || (bar0 & 1u)) {
        serial_print("[E1000E] BAR0 is not a usable MMIO BAR\n");
        return 0;
    }

    uint64_t mmio_base = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if ((bar0 & 0x6u) == 0x4u)
        mmio_base |= ((uint64_t)bar1 << 32);
    else if ((bar0 & 0x6u) != 0) {
        serial_print("[E1000E] Unsupported BAR0 memory type\n");
        return 0;
    }
    if (!mmio_base || (mmio_base & 0xFFFu)) {
        serial_print("[E1000E] Invalid MMIO base\n");
        return 0;
    }

    /* Enable memory decoding and bus mastering; preserve command bits only. */
    uint32_t command_status = pci_config_read_dword(bus, slot, function, 0x04u);
    uint16_t command = (uint16_t)command_status;
    command |= 0x0006u;
    command &= (uint16_t)~0x0400u; /* allow legacy INTx, though polling is used */
    pci_config_write_dword(bus, slot, function, 0x04u, command);

    const uint64_t map_flags = PAGE_PRESENT | PAGE_WRITE |
                               (1ull << 3) | (1ull << 4);
    for (uint64_t off = 0; off < E1000E_MMIO_BYTES; off += 4096u) {
        void *address = (void *)(uintptr_t)(mmio_base + off);
        if (!VMM_MapPage(address, address, map_flags)) {
            serial_print("[E1000E] Failed to map MMIO registers\n");
            return 0;
        }
    }
    mmio = (volatile uint32_t *)(uintptr_t)mmio_base;

    /* Polling mode: mask every device interrupt and clear pending causes. */
    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    if (!wait_reg_clear(REG_CTRL, CTRL_RST)) {
        serial_print("[E1000E] Controller reset timed out\n");
        mmio = NULL;
        return 0;
    }
    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);

    /* Request link-up and initialize descriptor storage. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);
    zero_bytes(rx_ring, sizeof(rx_ring));
    zero_bytes(tx_ring, sizeof(tx_ring));
    zero_bytes(rx_buffers, sizeof(rx_buffers));
    zero_bytes(tx_buffers, sizeof(tx_buffers));
    rx_next = 0;
    tx_next = 0;

    for (uint32_t i = 0; i < E1000E_RING_COUNT; ++i) {
        if (!dma_address_ok(rx_buffers[i], sizeof(rx_buffers[i])) ||
            !dma_address_ok(tx_buffers[i], sizeof(tx_buffers[i]))) {
            serial_print("[E1000E] DMA buffer address is unsupported\n");
            mmio = NULL;
            return 0;
        }
        rx_ring[i].address = (uint64_t)(uintptr_t)rx_buffers[i];
        tx_ring[i].address = (uint64_t)(uintptr_t)tx_buffers[i];
        tx_ring[i].status = TXD_STAT_DD;
    }

    uint64_t rx_desc_addr = (uint64_t)(uintptr_t)rx_ring;
    uint64_t tx_desc_addr = (uint64_t)(uintptr_t)tx_ring;
    if ((rx_desc_addr & 127u) || (tx_desc_addr & 127u) ||
        rx_desc_addr >= (1ull << 52) || tx_desc_addr >= (1ull << 52)) {
        serial_print("[E1000E] Descriptor ring alignment/address invalid\n");
        mmio = NULL;
        return 0;
    }

    /* RX descriptor ring, 16-byte descriptors. */
    reg_write(REG_RDBAL, (uint32_t)rx_desc_addr);
    reg_write(REG_RDBAH, (uint32_t)(rx_desc_addr >> 32));
    reg_write(REG_RDLEN, sizeof(rx_ring));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, E1000E_RING_COUNT - 1u);
    reg_write(REG_RDTR, 0);
    reg_write(REG_RXDCTL, 0);

    /* TX descriptor ring. */
    reg_write(REG_TDBAL, (uint32_t)tx_desc_addr);
    reg_write(REG_TDBAH, (uint32_t)(tx_desc_addr >> 32));
    reg_write(REG_TDLEN, sizeof(tx_ring));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TXDCTL, 0);
    reg_write(REG_TIPG, 10u | (8u << 10) | (6u << 20));
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));

    /* Read the factory-programmed receive address (QEMU supplies a valid MAC). */
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    mac[0] = (uint8_t)ral;
    mac[1] = (uint8_t)(ral >> 8);
    mac[2] = (uint8_t)(ral >> 16);
    mac[3] = (uint8_t)(ral >> 24);
    mac[4] = (uint8_t)rah;
    mac[5] = (uint8_t)(rah >> 8);
    if (!(rah & (1u << 31)) ||
        !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
        serial_print("[E1000E] No valid factory MAC address\n");
        mmio = NULL;
        return 0;
    }

    /* Enable broadcast receive and strip the Ethernet CRC from RX frames. */
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    (void)reg_read(REG_STATUS);
    ready = 1;
    serial_print("[E1000E] Intel 82574L initialized; RX/TX DMA rings ready\n");
    log_mac();
    serial_print(e1000e_link_up() ? "[E1000E] Link is up\n" :
                                    "[E1000E] Link is down (device ready)\n");
    return 1;
}

int e1000e_is_ready(void) {
    return ready;
}

int e1000e_link_up(void) {
    return ready && ((reg_read(REG_STATUS) & STATUS_LU) != 0);
}

const uint8_t *e1000e_mac_address(void) {
    return ready ? mac : NULL;
}

int e1000e_send(const void *frame, uint16_t length) {
    if (!ready || !frame || length < 14u || length > E1000E_FRAME_MAX)
        return 0;

    uint32_t index = tx_next;
    e1000e_tx_desc_t *desc = &tx_ring[index];
    for (uint32_t spin = 0; spin < E1000E_TIMEOUT; ++spin) {
        if (desc->status & TXD_STAT_DD) break;
        if (spin + 1u == E1000E_TIMEOUT) return 0;
    }

    copy_bytes(tx_buffers[index], frame, length);
    desc->length = length;
    desc->checksum_offset = 0;
    desc->checksum_start = 0;
    desc->special = 0;
    desc->status = 0;
    __asm__ volatile("mfence" ::: "memory");
    desc->command = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    __asm__ volatile("mfence" ::: "memory");
    tx_next = (index + 1u) % E1000E_RING_COUNT;
    reg_write(REG_TDT, tx_next);

    for (uint32_t spin = 0; spin < E1000E_TIMEOUT; ++spin) {
        if (desc->status & TXD_STAT_DD) {
            __asm__ volatile("mfence" ::: "memory");
            return 1;
        }
    }
    serial_print("[E1000E] TX completion timed out\n");
    return 0;
}

int e1000e_receive(void *frame, uint16_t capacity, uint16_t *length) {
    if (!ready || !frame || !length || capacity == 0) return 0;

    e1000e_rx_desc_t *desc = &rx_ring[rx_next];
    __asm__ volatile("mfence" ::: "memory");
    if (!(desc->status & RXD_STAT_DD)) return 0;

    int valid = (desc->status & RXD_STAT_EOP) &&
                desc->errors == 0 && desc->length >= 14u &&
                desc->length <= E1000E_BUFFER_BYTES &&
                desc->length <= capacity;
    if (valid) {
        copy_bytes(frame, rx_buffers[rx_next], desc->length);
        *length = desc->length;
    }

    desc->status = 0;
    desc->errors = 0;
    desc->length = 0;
    __asm__ volatile("mfence" ::: "memory");
    reg_write(REG_RDT, rx_next);
    rx_next = (rx_next + 1u) % E1000E_RING_COUNT;
    return valid ? 1 : -1;
}

/* Send a harmless ARP who-has for the QEMU user-network gateway (10.0.2.2). */
int e1000e_ci_transmit_test(void) {
    if (!ready) return 0;
    uint8_t frame[60];
    zero_bytes(frame, sizeof(frame));
    for (uint32_t i = 0; i < 6; ++i) frame[i] = 0xFFu;
    copy_bytes(frame + 6, mac, 6);
    frame[12] = 0x08u; frame[13] = 0x06u; /* EtherType ARP */
    frame[14] = 0x00u; frame[15] = 0x01u; /* Ethernet */
    frame[16] = 0x08u; frame[17] = 0x00u; /* IPv4 */
    frame[18] = 6u; frame[19] = 4u;
    frame[20] = 0x00u; frame[21] = 0x01u; /* request */
    copy_bytes(frame + 22, mac, 6);
    frame[28] = 10u; frame[29] = 0u; frame[30] = 2u; frame[31] = 15u;
    frame[38] = 10u; frame[39] = 0u; frame[40] = 2u; frame[41] = 2u;
    if (!e1000e_send(frame, sizeof(frame))) return 0;
    serial_print("[E1000E] CI ARP broadcast frame transmitted (TX DD)\n");
    return 1;
}
