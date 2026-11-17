#include "e1000.h"
#include "netdev.h"
#include "heap.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);
void print_hex(uint64_t value);

#define E1000_REG_CTRL       0x0000u
#define E1000_REG_STATUS     0x0008u
#define E1000_REG_IMC        0x00D8u
#define E1000_REG_RCTL       0x0100u
#define E1000_REG_TCTL       0x0400u
#define E1000_REG_TIPG       0x0410u
#define E1000_REG_RDBAL      0x2800u
#define E1000_REG_RDBAH      0x2804u
#define E1000_REG_RDLEN      0x2808u
#define E1000_REG_RDH        0x2810u
#define E1000_REG_RDT        0x2818u
#define E1000_REG_TDBAL      0x3800u
#define E1000_REG_TDBAH      0x3804u
#define E1000_REG_TDLEN      0x3808u
#define E1000_REG_TDH        0x3810u
#define E1000_REG_TDT        0x3818u
#define E1000_REG_RAL0       0x5400u
#define E1000_REG_RAH0       0x5404u
#define E1000_REG_MTA        0x5200u

#define E1000_MMIO_BYTES     0x6000u
#define E1000_PAGE_SIZE      4096u
#define E1000_RING_COUNT     16u
#define E1000_RX_BUFFER_SIZE 2048u
#define E1000_MTA_COUNT      128u

#define E1000_CTRL_RST       (1u << 26)
#define E1000_CTRL_SLU       (1u << 6)
#define E1000_STATUS_LU      (1u << 1)
#define E1000_RCTL_EN        (1u << 1)
#define E1000_RCTL_BAM       (1u << 15)
#define E1000_RCTL_SECRC     (1u << 26)
#define E1000_TCTL_EN        (1u << 1)
#define E1000_TCTL_PSP       (1u << 3)
#define E1000_TXD_CMD_EOP    (1u << 0)
#define E1000_TXD_CMD_IFCS   (1u << 1)
#define E1000_TXD_CMD_RS     (1u << 3)
#define E1000_TXD_STAT_DD    (1u << 0)
#define E1000_RXD_STAT_DD    (1u << 0)

#define E1000_TX_TIMEOUT_SPINS 1000000u
#define E1000_RESET_SPINS      1000000u

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    volatile uint8_t *registers;
    net_device_t netdev;
    uint16_t next_rx;
    uint16_t next_tx;
} e1000_state_t;

/*
 * The current VexOS kernel identity-maps its image and uses coherent x86 DMA.
 * Keep descriptor rings and packet storage static, naturally aligned, and in
 * the kernel image so their CPU addresses are also their physical DMA addresses.
 */
static e1000_state_t g_e1000;
static e1000_rx_desc_t g_rx_ring[E1000_RING_COUNT]
    __attribute__((aligned(16)));
static e1000_tx_desc_t g_tx_ring[E1000_RING_COUNT]
    __attribute__((aligned(16)));
static uint8_t g_rx_buffers[E1000_RING_COUNT][E1000_RX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static uint8_t g_tx_buffers[E1000_RING_COUNT][E1000_RX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static int g_initialized;

static uint32_t e1000_read_reg(uint32_t offset) {
    return *(volatile uint32_t *)(g_e1000.registers + offset);
}

static void e1000_write_reg(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_e1000.registers + offset) = value;
    /* Reading STATUS flushes posted PCI MMIO writes on this device. */
    (void)e1000_read_reg(E1000_REG_STATUS);
}

static void e1000_dma_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static void e1000_copy_bytes(void *destination, const void *source,
                             size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        out[i] = in[i];
}

static int e1000_map_registers(uint64_t base) {
    if (base == 0 || base > UINT64_MAX - E1000_MMIO_BYTES)
        return 0;

    uint64_t page_base = base & ~((uint64_t)E1000_PAGE_SIZE - 1u);
    uint64_t page_offset = base - page_base;
    uint64_t map_bytes = (page_offset + E1000_MMIO_BYTES +
                          E1000_PAGE_SIZE - 1u) &
                         ~((uint64_t)E1000_PAGE_SIZE - 1u);

    const uint64_t map_flags = PAGE_WRITE |
                               (1ull << 3) | /* PAGE_WRITE_THROUGH */
                               (1ull << 4);  /* PAGE_CACHE_DISABLE */

    for (uint64_t offset = 0; offset < map_bytes;
         offset += E1000_PAGE_SIZE) {
        void *address = (void *)(uintptr_t)(page_base + offset);
        if (!VMM_MapPage(address, address, map_flags))
            return 0;
    }

    g_e1000.registers = (volatile uint8_t *)(uintptr_t)base;
    return 1;
}

static int e1000_read_mac(uint8_t mac[6]) {
    uint32_t low = e1000_read_reg(E1000_REG_RAL0);
    uint32_t high = e1000_read_reg(E1000_REG_RAH0);
    if ((high & (1u << 31)) == 0)
        return 0;

    mac[0] = (uint8_t)low;
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);

    int all_zero = 1;
    int all_ones = 1;
    for (size_t i = 0; i < 6; ++i) {
        if (mac[i] != 0)
            all_zero = 0;
        if (mac[i] != 0xFFu)
            all_ones = 0;
    }
    return !all_zero && !all_ones;
}

static int e1000_init_rings(void) {
    uint64_t rx_address = (uint64_t)(uintptr_t)&g_rx_ring[0];
    uint64_t tx_address = (uint64_t)(uintptr_t)&g_tx_ring[0];

    if ((rx_address & 0x0Fu) || (tx_address & 0x0Fu) ||
        rx_address > UINT32_MAX || tx_address > UINT32_MAX ||
        rx_address + sizeof(g_rx_ring) - 1u > UINT32_MAX ||
        tx_address + sizeof(g_tx_ring) - 1u > UINT32_MAX)
        return 0;

    for (uint16_t i = 0; i < E1000_RING_COUNT; ++i) {
        uint64_t buffer = (uint64_t)(uintptr_t)&g_rx_buffers[i][0];
        uint64_t tx_buffer = (uint64_t)(uintptr_t)&g_tx_buffers[i][0];
        if (buffer > UINT32_MAX || tx_buffer > UINT32_MAX ||
            buffer + E1000_RX_BUFFER_SIZE - 1u > UINT32_MAX ||
            tx_buffer + E1000_RX_BUFFER_SIZE - 1u > UINT32_MAX)
            return 0;

        g_rx_ring[i].buffer_addr = buffer;
        g_rx_ring[i].length = 0;
        g_rx_ring[i].checksum = 0;
        g_rx_ring[i].status = 0;
        g_rx_ring[i].errors = 0;
        g_rx_ring[i].special = 0;

        g_tx_ring[i].buffer_addr = 0;
        g_tx_ring[i].length = 0;
        g_tx_ring[i].checksum_offset = 0;
        g_tx_ring[i].command = 0;
        g_tx_ring[i].status = E1000_TXD_STAT_DD;
        g_tx_ring[i].checksum_start = 0;
        g_tx_ring[i].special = 0;
    }

    e1000_dma_barrier();

    e1000_write_reg(E1000_REG_RDBAL, (uint32_t)rx_address);
    e1000_write_reg(E1000_REG_RDBAH, (uint32_t)(rx_address >> 32));
    e1000_write_reg(E1000_REG_RDLEN,
                    (uint32_t)(sizeof(g_rx_ring)));
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_RING_COUNT - 1u);

    e1000_write_reg(E1000_REG_TDBAL, (uint32_t)tx_address);
    e1000_write_reg(E1000_REG_TDBAH, (uint32_t)(tx_address >> 32));
    e1000_write_reg(E1000_REG_TDLEN,
                    (uint32_t)(sizeof(g_tx_ring)));
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);

    /* 2048-byte legacy RX buffers, broadcast acceptance, CRC stripped. */
    e1000_write_reg(E1000_REG_RCTL,
                    E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);

    /* Enable transmit, pad short Ethernet frames, and set collision defaults. */
    e1000_write_reg(E1000_REG_TCTL,
                    E1000_TCTL_EN | E1000_TCTL_PSP |
                    (0x10u << 4) | (0x40u << 12));
    e1000_write_reg(E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));

    return 1;
}

static int e1000_transmit(net_device_t *device, const void *frame,
                          size_t length) {
    (void)device;
    if (!frame || length < NETDEV_ETH_HEADER_SIZE ||
        length > E1000_RX_BUFFER_SIZE)
        return 0;

    uint16_t index = g_e1000.next_tx;
    volatile e1000_tx_desc_t *descriptor = &g_tx_ring[index];

    if ((descriptor->status & E1000_TXD_STAT_DD) == 0)
        return 0;

    e1000_copy_bytes(g_tx_buffers[index], frame, length);

    descriptor->buffer_addr =
        (uint64_t)(uintptr_t)&g_tx_buffers[index][0];
    descriptor->length = (uint16_t)length;
    descriptor->checksum_offset = 0;
    descriptor->command = E1000_TXD_CMD_EOP |
                          E1000_TXD_CMD_IFCS |
                          E1000_TXD_CMD_RS;
    descriptor->status = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;

    e1000_dma_barrier();
    uint16_t next = (uint16_t)((index + 1u) % E1000_RING_COUNT);
    g_e1000.next_tx = next;
    e1000_write_reg(E1000_REG_TDT, next);
    return 1;
}

static void e1000_poll(net_device_t *device, uint32_t budget) {
    if (!device || budget == 0 || !g_e1000.registers)
        return;

    device->link_up =
        (e1000_read_reg(E1000_REG_STATUS) & E1000_STATUS_LU) != 0;

    for (uint32_t work = 0; work < budget; ++work) {
        uint16_t index = g_e1000.next_rx;
        volatile e1000_rx_desc_t *descriptor = &g_rx_ring[index];

        if ((descriptor->status & E1000_RXD_STAT_DD) == 0)
            break;

        e1000_dma_barrier();
        uint16_t length = descriptor->length;
        uint8_t errors = descriptor->errors;

        if (errors == 0 && length >= NETDEV_ETH_HEADER_SIZE &&
            length <= E1000_RX_BUFFER_SIZE) {
            netdev_receive(device, &g_rx_buffers[index][0], length);
        } else {
            ++device->rx_dropped;
        }

        descriptor->length = 0;
        descriptor->checksum = 0;
        descriptor->errors = 0;
        descriptor->special = 0;
        descriptor->status = 0;
        e1000_dma_barrier();

        g_e1000.next_rx =
            (uint16_t)((index + 1u) % E1000_RING_COUNT);
        e1000_write_reg(E1000_REG_RDT, index);
    }
}

static const netdev_ops_t g_e1000_ops = {
    .transmit = e1000_transmit,
    .poll = e1000_poll
};

int e1000_init(uint64_t mmio_base) {
    if (g_initialized)
        return 1;

    serial_print("[E1000] Mapping controller registers at ");
    print_hex(mmio_base);
    serial_print("\n");

    if (!e1000_map_registers(mmio_base)) {
        serial_print("[E1000] MMIO mapping failed\n");
        g_e1000.registers = NULL;
        return 0;
    }

    /* Keep the first driver deliberately polling-only until IRQ routing exists. */
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read_reg(E1000_REG_STATUS);

    e1000_write_reg(E1000_REG_RCTL, 0);
    e1000_write_reg(E1000_REG_TCTL, 0);
    e1000_write_reg(E1000_REG_CTRL,
                    e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_RST);

    int reset_complete = 0;
    for (uint32_t spin = 0; spin < E1000_RESET_SPINS; ++spin) {
        if ((e1000_read_reg(E1000_REG_CTRL) & E1000_CTRL_RST) == 0) {
            reset_complete = 1;
            break;
        }
    }
    if (!reset_complete) {
        serial_print("[E1000] Controller reset timed out\n");
        return 0;
    }

    /* Reset returns interrupt masking and queue state to device defaults. */
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read_reg(E1000_REG_STATUS);

    uint8_t mac[6];
    if (!e1000_read_mac(mac)) {
        serial_print("[E1000] No valid receive MAC address in RAL/RAH\n");
        return 0;
    }

    for (uint32_t i = 0; i < E1000_MTA_COUNT; ++i)
        e1000_write_reg(E1000_REG_MTA + (i * 4u), 0);

    g_e1000.next_rx = 0;
    g_e1000.next_tx = 0;
    if (!e1000_init_rings()) {
        serial_print("[E1000] DMA ring setup failed (requires low identity-mapped RAM)\n");
        return 0;
    }

    /* Ask the PHY to bring up its link; poll STATUS for the actual result. */
    e1000_write_reg(E1000_REG_CTRL,
                    e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_SLU);

    g_e1000.netdev = (net_device_t){0};
    g_e1000.netdev.name[0] = 'e';
    g_e1000.netdev.name[1] = 't';
    g_e1000.netdev.name[2] = 'h';
    g_e1000.netdev.name[3] = '0';
    g_e1000.netdev.mtu = 1500u;
    g_e1000.netdev.flags = NETDEV_FLAG_ETHERNET |
                           NETDEV_FLAG_BROADCAST;
    g_e1000.netdev.ops = &g_e1000_ops;
    g_e1000.netdev.driver_data = &g_e1000;
    for (size_t i = 0; i < 6; ++i)
        g_e1000.netdev.mac[i] = mac[i];
    g_e1000.netdev.link_up =
        (e1000_read_reg(E1000_REG_STATUS) & E1000_STATUS_LU) != 0;

    if (!netdev_register(&g_e1000.netdev)) {
        serial_print("[E1000] Network-device registration failed\n");
        return 0;
    }

    serial_print("[E1000] Controller initialized; interface eth0 MAC=");
    for (size_t i = 0; i < 6; ++i) {
        static const char digits[] = "0123456789ABCDEF";
        char pair[3] = {
            digits[(mac[i] >> 4) & 0x0Fu],
            digits[mac[i] & 0x0Fu],
            0
        };
        serial_print(pair);
        if (i != 5)
            serial_print(":");
    }
    serial_print(g_e1000.netdev.link_up ? " link=up\n" : " link=down\n");
    serial_print("[E1000] RX/TX descriptor rings online (polling mode)\n");
    g_initialized = 1;
    return 1;
}
