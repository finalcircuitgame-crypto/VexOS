#include "alx.h"

#include "pci.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);
extern void desktop_debug_set_network_status(const char *status);

#define ALX_VENDOR_ID                 0x1969u
#define ALX_DEVICE_ID_AR8171          0x10A1u
#define ALX_MMIO_BYTES                0x2000u
#define ALX_IDLE_DECISN_TIMER          0x1474u
#define ALX_IDLE_DECISN_TIMER_DEF      0x0400u
#define ALX_RING_COUNT                128u
#define ALX_RX_BUFFER_BYTES           1536u
#define ALX_DMA_BUFFER_STRIDE         2048u
#define ALX_FRAME_MAX                 1518u
#define ALX_WAIT_LIMIT                1000000u

#define ALX_PCI_STATUS_CAP_LIST        0x0010u
#define ALX_PCI_CAP_ID_EXP             0x10u
#define ALX_PCIE_DEVCTL_MRRS_SHIFT     12u
#define ALX_PCIE_DEVCTL_MRRS_MASK      (7u << ALX_PCIE_DEVCTL_MRRS_SHIFT)
#define ALX_PCIE_DEVCTL_MRRS_MIN       2u

/* ALX register offsets. */
#define ALX_MSIX_MASK                 0x0090u
#define ALX_ISR_DIS                   (1u << 31)
#define ALX_INT_RETRIG                0x1608u
#define ALX_MASTER                    0x1400u
#define ALX_IRQ_MODU_TIMER            0x1408u
#define ALX_PHY_CTRL                  0x140Cu
#define ALX_MAC_STS                   0x1410u
#define ALX_MDIO                      0x1414u
#define ALX_SERDES                    0x1424u
#define ALX_LPI_CTRL                  0x1440u
#define ALX_MDIO_EXTN                 0x1448u
#define ALX_DRV                       0x1804u
#define ALX_MAC_CTRL                  0x1480u
#define ALX_STAD0                     0x1488u
#define ALX_STAD1                     0x148Cu
#define ALX_MTU                       0x149Cu
#define ALX_SRAM5                     0x1524u
#define ALX_SRAM9                     0x1534u
#define ALX_RX_BASE_ADDR_HI           0x1540u
#define ALX_TX_BASE_ADDR_HI           0x1544u
#define ALX_RFD_ADDR_LO               0x1550u
#define ALX_RFD_RING_SZ               0x1560u
#define ALX_RFD_BUF_SZ                0x1564u
#define ALX_RRD_ADDR_LO               0x1568u
#define ALX_RRD_RING_SZ               0x1578u
#define ALX_TPD_PRI0_ADDR_LO          0x1580u
#define ALX_TPD_RING_SZ               0x1584u
#define ALX_TXQ0                      0x1590u
#define ALX_TXQ1                      0x1594u
#define ALX_RXQ0                      0x15A0u
#define ALX_RXQ2                      0x15A8u
#define ALX_DMA                       0x15C0u
#define ALX_SMB_TIMER                 0x15C4u
#define ALX_TINT_TPD_THRSHLD          0x15C8u
#define ALX_TINT_TIMER                0x15CCu
#define ALX_RFD_PIDX                  0x15E0u
#define ALX_TPD_PRI0_PIDX             0x15F2u
#define ALX_TPD_PRI0_CIDX             0x15F6u
#define ALX_ISR                       0x1600u
#define ALX_IMR                       0x1604u
#define ALX_CLK_GATE                  0x1814u
#define ALX_HQTPD                     0x193Cu
#define ALX_MISC                      0x19C0u
#define ALX_MSIC2                     0x19C8u
#define ALX_MISC3                     0x19CCu

/* Master / reset control. */
#define ALX_MASTER_DMA_MAC_RST        (1u << 0)
#define ALX_MASTER_OOB_DIS            (1u << 6)
#define ALX_MASTER_SYSALVTIMER_EN     (1u << 7)
#define ALX_MASTER_IRQMOD1_EN         (1u << 10)
#define ALX_MASTER_IRQMOD2_EN         (1u << 11)
#define ALX_MASTER_PCLKSEL_SRDS       (1u << 12)
#define ALX_MASTER_WAKEN_25M          (1u << 5)
#define ALX_MAC_STS_IDLE              0x0Fu

/* ALX PCIe/power-management registers and fields (Linux alx parity). */
#define ALX_UE_SVRT                   0x010Cu
#define ALX_PDLL_TRNS1                0x1104u
#define ALX_PMCTRL                    0x12F8u
#define ALX_WOL0                      0x14A0u

#define ALX_UE_SVRT_DLPROTERR         (1u << 4)
#define ALX_UE_SVRT_FCPROTERR         (1u << 13)
#define ALX_PDLL_TRNS1_D3PLLOFF_EN    (1u << 11)

#define ALX_PMCTRL_HOTRST_WTEN        (1u << 31)
#define ALX_PMCTRL_ASPM_FCEN          (1u << 30)
#define ALX_PMCTRL_SADLY_EN           (1u << 29)
#define ALX_PMCTRL_TXL1_AFTER_L0S     (1u << 19)
#define ALX_PMCTRL_RCVR_WT_1US        (1u << 15)
#define ALX_PMCTRL_L1_CLKSW_EN        (1u << 13)
#define ALX_PMCTRL_L0S_EN             (1u << 12)
#define ALX_PMCTRL_RXL1_AFTER_L0S     (1u << 11)
#define ALX_PMCTRL_L1_BUFSRX_EN       (1u << 7)
#define ALX_PMCTRL_L1_SRDSRX_PWD      (1u << 6)
#define ALX_PMCTRL_L1_SRDSPLL_EN      (1u << 5)
#define ALX_PMCTRL_L1_SRDS_EN         (1u << 4)
#define ALX_PMCTRL_L1_EN              (1u << 3)
#define ALX_PMCTRL_LCKDET_TIMER_SHIFT 24u
#define ALX_PMCTRL_L1REQ_TO_SHIFT     20u
#define ALX_PMCTRL_L1_TIMER_SHIFT     16u
#define ALX_PMCTRL_LCKDET_TIMER_MASK  0x0Fu
#define ALX_PMCTRL_L1REQ_TO_MASK      0x0Fu
#define ALX_PMCTRL_L1_TIMER_MASK      0x07u

#define ALX_MISC_INTNLOSC_OPEN        (1u << 3)
#define ALX_MISC_ISO_EN               (1u << 12)
#define ALX_MISC_PSW_OCP_MASK         (7u << 21)
#define ALX_MISC_PSW_OCP_DEF          (7u << 21)
#define ALX_MISC3_25M_BY_SW           (1u << 1)
#define ALX_MISC3_25M_NOTO_INTNL      (1u << 0)
#define ALX_MSIC2_CALB_START          (1u << 0)

#define ALX_PHY_CTRL_DSPRST_OUT       (1u << 0)
#define ALX_PHY_CTRL_LED_MODE         (1u << 2)
#define ALX_PHY_CTRL_GATE_25M         (1u << 5)
#define ALX_PHY_CTRL_IDDQ             (1u << 7)
#define ALX_PHY_CTRL_HIB_EN           (1u << 10)
#define ALX_PHY_CTRL_HIB_PULSE        (1u << 11)
#define ALX_PHY_CTRL_RST_ANALOG       (1u << 12)
#define ALX_PHY_CTRL_PLL_ON           (1u << 13)
#define ALX_PHY_CTRL_POWER_DOWN       (1u << 14)
#define ALX_PHY_CTRL_100AB_EN         (1u << 17)
#define ALX_PHY_CTRL_CLS              (ALX_PHY_CTRL_LED_MODE | ALX_PHY_CTRL_PLL_ON | ALX_PHY_CTRL_100AB_EN)

#define ALX_MDIO_MODE_EXT             (1u << 30)
#define ALX_MDIO_BUSY                 (1u << 27)
#define ALX_MDIO_CLK_SEL_25MD128      (7u << 24)
#define ALX_MDIO_START                (1u << 23)
#define ALX_MDIO_SPRES_PRMBL          (1u << 22)
#define ALX_MDIO_OP_READ              (1u << 21)
#define ALX_MDIO_REG_SHIFT            16u
#define ALX_MDIO_EXTN_DEVAD_SHIFT     16u
#define ALX_MDIO_DATA_MASK            0xFFFFu
#define ALX_MDIO_TIMEOUT              1000000u

#define ALX_MAC_CTRL_WOLSPED_SWEN     (1u << 30)
#define ALX_MAC_CTRL_MHASH_ALG_HI5B   (1u << 29)
#define ALX_MAC_CTRL_BRD_EN           (1u << 26)
#define ALX_MAC_CTRL_SPEED_SHIFT      20u
#define ALX_MAC_CTRL_SPEED_MASK       (3u << ALX_MAC_CTRL_SPEED_SHIFT)
#define ALX_MAC_CTRL_SPEED_10_100     1u
#define ALX_MAC_CTRL_SPEED_1000       2u
#define ALX_MAC_CTRL_PRMBLEN_SHIFT    10u
#define ALX_MAC_CTRL_PCRCE            (1u << 7)
#define ALX_MAC_CTRL_CRCE             (1u << 6)
#define ALX_MAC_CTRL_FULLD            (1u << 5)
#define ALX_MAC_CTRL_RXFC_EN          (1u << 3)
#define ALX_MAC_CTRL_TXFC_EN          (1u << 2)
#define ALX_DRV_PHY_AUTO               (1u << 28)
#define ALX_DRV_PHY_100                (1u << 26)
#define ALX_DRV_PHY_10                 (1u << 25)
#define ALX_DRV_PHY_DUPLEX             (1u << 24)
#define ALX_DRV_PHY_PAUSE              (1u << 23)
#define ALX_DRV_PHY_FIELD_MASK         (0xFFu << 21)
#define ALX_DRV_PHY_CONFIG             (ALX_DRV_PHY_AUTO | ALX_DRV_PHY_100 | \
                                        ALX_DRV_PHY_10 | ALX_DRV_PHY_DUPLEX)
#define ALX_PHY_INITED                 0x003Fu
#define ALX_MII_ADVERTISE_PAUSE        0x0400u
#define ALX_MAC_CTRL_RX_EN            (1u << 1)
#define ALX_MAC_CTRL_TX_EN            (1u << 0)

#define ALX_TXQ0_MODE_ENHANCE         (1u << 6)
#define ALX_TXQ0_EN                   (1u << 5)
#define ALX_TXQ0_SUPT_IPOPT           (1u << 4)
#define ALX_RXQ0_EN                   (1u << 31)
#define ALX_RXQ0_RSS_HASH_EN          (1u << 29)
#define ALX_RXQ0_RSS_MODE_DIS         (0u << 26)
#define ALX_TXQ0_TPD_BURSTPREF        5u
#define ALX_RXQ0_NUM_RFD_PREF         (8u << 20)
#define ALX_RXQ0_IDT_TBL_SIZE         (0x100u << 8)
#define ALX_RXQ0_RSS_HSTYP_ALL        0x3Cu
#define ALX_RXQ0_IPV6_PARSE_EN        (1u << 7)
#define ALX_RXQ0_ASPM_THRESH_100M      3u
#define ALX_TXQ1_ERRLGPKT_DROP_EN     (1u << 11)
#define ALX_HQTPD_BURST_EN            (1u << 31)
#define ALX_CLK_GATE_ALL              0x3Fu

#define ALX_RFD_UPDATED_SHIFT         31u
#define ALX_RRD_NOR_SHIFT             16u
#define ALX_RRD_NOR_MASK              0x0Fu
#define ALX_RRD_SI_SHIFT              20u
#define ALX_RRD_SI_MASK               0x0FFFu
#define ALX_RRD_PKTLEN_MASK           0x3FFFu
#define ALX_RRD_ERR_RES               (1u << 20)
#define ALX_RRD_ERR_FCS               (1u << 21)
#define ALX_RRD_ERR_FAE               (1u << 22)
#define ALX_RRD_ERR_TRUNC             (1u << 23)
#define ALX_RRD_ERR_LEN               (1u << 30)
#define ALX_RRD_ERROR_MASK            (ALX_RRD_ERR_RES | ALX_RRD_ERR_FCS | ALX_RRD_ERR_FAE | ALX_RRD_ERR_TRUNC | ALX_RRD_ERR_LEN)
#define ALX_TPD_EOP                   (1u << 31)

#define ALX_MII_BMCR                  0u
#define ALX_MII_BMSR                  1u
#define ALX_MII_ADVERTISE             4u
#define ALX_MII_CTRL1000              9u
#define ALX_MII_GIGA_PSSR             0x11u
#define ALX_MII_DBG_ADDR              0x1Du
#define ALX_MII_DBG_DATA              0x1Eu
#define ALX_MII_IER                   0x12u
#define ALX_BMCR_RESET                0x8000u
#define ALX_BMCR_ANENABLE             0x1000u
#define ALX_BMCR_ANRESTART            0x0200u
#define ALX_BMSR_LSTATUS              0x0004u
#define ALX_GIGA_PSSR_RESOLVED        0x0800u
#define ALX_GIGA_PSSR_DUPLEX          0x2000u
#define ALX_GIGA_PSSR_SPEED_MASK      0xC000u
#define ALX_GIGA_PSSR_100             0x4000u
#define ALX_GIGA_PSSR_1000            0x8000u

typedef struct __attribute__((packed)) {
    uint16_t length;
    uint16_t vlan_tag;
    uint32_t word1;
    uint64_t address;
} alx_tpd_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
} alx_rfd_t;

typedef struct __attribute__((packed)) {
    uint32_t word0;
    uint32_t rss_hash;
    uint32_t word2;
    volatile uint32_t word3;
} alx_rrd_t;

/*
 * The kernel image is identity mapped and loaded below the device DMA address
 * limit. Keep each DMA object page-aligned; the ALX RX descriptor format also
 * requires four-byte-aligned packet buffers.
 */
static alx_tpd_t g_tpd[ALX_RING_COUNT] __attribute__((aligned(4096)));
static alx_rfd_t g_rfd[ALX_RING_COUNT] __attribute__((aligned(4096)));
static alx_rrd_t g_rrd[ALX_RING_COUNT] __attribute__((aligned(4096)));
static uint8_t g_rx_buffers[ALX_RING_COUNT][ALX_DMA_BUFFER_STRIDE]
    __attribute__((aligned(4096)));
static uint8_t g_tx_buffers[ALX_RING_COUNT][ALX_DMA_BUFFER_STRIDE]
    __attribute__((aligned(4096)));

static volatile uint8_t *g_mmio;
static uint8_t g_mac[6];
static uint8_t g_ready;
static uint8_t g_datapath_ready;
static uint8_t g_link_up;
static uint8_t g_mac_started;
static uint8_t g_link_speed_code;
static uint8_t g_link_full_duplex;
static uint8_t g_revision;
static uint8_t g_revision_cr;
static uint16_t g_tx_next;
static uint16_t g_tx_inflight;
static uint16_t g_rx_read;
static uint16_t g_rx_write;
static uint16_t g_rrd_read;
static uint16_t g_rx_fault_index;
static uint8_t g_rx_fault_count;
static uint8_t g_rx_ring_fault;
static uint32_t g_link_poll_count;
static uint32_t g_mac_ctrl_base;

/* TX completion is tracked for every submitted descriptor, not just a probe. */
static uint16_t g_tx_last_consumer;
static uint32_t g_tx_stall_polls;
static uint32_t g_tx_recovery_count;
static uint8_t g_tx_completion_reported;
static uint8_t g_tx_reject_reported;
static uint8_t g_mdio_status_failures;
static char g_tx_timeout_diagnostic[768];
static uint8_t g_tx_timeout_diagnostic_pending;

static uint32_t alx_read32(uint32_t offset) {
    return *(volatile uint32_t *)(void *)(g_mmio + offset);
}

static uint16_t alx_read16(uint32_t offset) {
    return *(volatile uint16_t *)(void *)(g_mmio + offset);
}

static void alx_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(void *)(g_mmio + offset) = value;
}

static void alx_write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(void *)(g_mmio + offset) = value;
}

static void alx_delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; ++i)
        __asm__ volatile("pause");
}

static void alx_zero(void *destination, size_t length) {
    uint8_t *p = (uint8_t *)destination;
    for (size_t i = 0; i < length; ++i)
        p[i] = 0;
}

static void alx_copy(void *destination, const void *source, size_t length) {
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        d[i] = s[i];
}

static void alx_append_hex32(char *buffer, size_t capacity, size_t *used,
                            const char *label, uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    while (*label && *used + 1u < capacity)
        buffer[(*used)++] = *label++;
    if (*used + 11u >= capacity)
        return;
    buffer[(*used)++] = '0';
    buffer[(*used)++] = 'x';
    for (uint32_t i = 0; i < 8u; ++i)
        buffer[(*used)++] = hex[(value >> ((7u - i) * 4u)) & 0x0Fu];
    buffer[(*used)++] = '\n';
    buffer[*used] = '\0';
}

static void alx_log_tx_timeout(void) {
    char diagnostic[768] = "[ALX] TX timeout snapshot\n";
    size_t used = sizeof("[ALX] TX timeout snapshot\n") - 1u;
    uint16_t index = (uint16_t)(
        (g_tx_next + ALX_RING_COUNT - 1u) % ALX_RING_COUNT);
    const alx_tpd_t *descriptor = &g_tpd[index];

    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TXQ0=", alx_read32(ALX_TXQ0));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  MAC_CTRL=", alx_read32(ALX_MAC_CTRL));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  DMA=", alx_read32(ALX_DMA));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  MAC_STS=", alx_read32(ALX_MAC_STS));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_BASE_LO=", alx_read32(ALX_TPD_PRI0_ADDR_LO));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_BASE_HI=", alx_read32(ALX_TX_BASE_ADDR_HI));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_RING_SZ=", alx_read32(ALX_TPD_RING_SZ));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  SRAM9=", alx_read32(ALX_SRAM9));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_PIDX=", alx_read16(ALX_TPD_PRI0_PIDX));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_CIDX=", alx_read16(ALX_TPD_PRI0_CIDX));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_SW_NEXT=", g_tx_next);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_LAST_CIDX=", g_tx_last_consumer);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_INFLIGHT=", g_tx_inflight);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_STALL_POLLS=", g_tx_stall_polls);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_INDEX=", index);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_LEN=", descriptor->length);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_WORD1=", descriptor->word1);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_ADDR_LO=", (uint32_t)descriptor->address);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TPD_ADDR_HI=", (uint32_t)(descriptor->address >> 32));
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TXBUF_LO=", (uint32_t)(uintptr_t)g_tx_buffers[index]);
    alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  TXBUF_HI=", (uint32_t)((uint64_t)(uintptr_t)g_tx_buffers[index] >> 32));

    /* Keep a durable copy; the desktop frame would overwrite an immediate draw. */
    serial_print(diagnostic);
    desktop_debug_set_network_status(diagnostic);
    size_t copy_length = used;
    if (copy_length >= sizeof(g_tx_timeout_diagnostic))
        copy_length = sizeof(g_tx_timeout_diagnostic) - 1u;
    for (size_t i = 0; i < copy_length; ++i)
        g_tx_timeout_diagnostic[i] = diagnostic[i];
    g_tx_timeout_diagnostic[copy_length] = '\0';
    g_tx_timeout_diagnostic_pending = 1u;
}

/* Called after desktop_tick() so the next full desktop paint cannot erase it. */
void alx_render_pending_diagnostics(void) {
    if (!g_tx_timeout_diagnostic_pending)
        return;
    desktop_debug_set_network_status(g_tx_timeout_diagnostic);
    g_tx_timeout_diagnostic_pending = 0u;
}

static int alx_dma_address_ok(const void *pointer, size_t length) {
    uint64_t address = (uint64_t)(uintptr_t)pointer;
    return length != 0u &&
           address <= 0xFFFFFFFFFFFFFFFFull - (uint64_t)length &&
           address < (1ull << 52) &&
           address + (uint64_t)length <= (1ull << 52);
}

/*
 * Linux's alx driver guarantees a PCIe Max Read Request Size of at least
 * 512 bytes before programming the ALX DMA read-request length. Firmware can
 * leave this field below the required minimum, so mirror that safeguard.
 */
static int alx_configure_pcie_read_request(uint8_t bus, uint8_t slot,
                                           uint8_t function) {
    uint32_t command_status =
        pci_config_read_dword(bus, slot, function, 0x04u);
    uint16_t status = (uint16_t)(command_status >> 16);
    if ((status & ALX_PCI_STATUS_CAP_LIST) == 0u) {
        serial_print("[ALX] PCI capability list is unavailable\n");
        return 0;
    }

    uint8_t capability =
        (uint8_t)pci_config_read_dword(bus, slot, function, 0x34u);
    for (uint32_t i = 0; i < 48u; ++i) {
        capability &= 0xFCu;
        if (capability < 0x40u || capability > 0xF4u)
            break;

        uint32_t header =
            pci_config_read_dword(bus, slot, function, capability);
        uint8_t capability_id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);

        if (capability_id == ALX_PCI_CAP_ID_EXP) {
            /* Device Control is the low 16 bits at PCIe cap + 0x08. */
            uint32_t control_status = pci_config_read_dword(
                bus, slot, function, (uint8_t)(capability + 0x08u));
            uint16_t control = (uint16_t)control_status;
            uint16_t current_mrrs = (uint16_t)(
                (control & ALX_PCIE_DEVCTL_MRRS_MASK) >>
                ALX_PCIE_DEVCTL_MRRS_SHIFT);

            if (current_mrrs < ALX_PCIE_DEVCTL_MRRS_MIN) {
                control &= (uint16_t)~ALX_PCIE_DEVCTL_MRRS_MASK;
                control |= (uint16_t)(ALX_PCIE_DEVCTL_MRRS_MIN <<
                                      ALX_PCIE_DEVCTL_MRRS_SHIFT);

                /*
                 * Write zero to Device Status (the upper half) because its
                 * status bits are write-one-to-clear.
                 */
                pci_config_write_dword(
                    bus, slot, function, (uint8_t)(capability + 0x08u),
                    (uint32_t)control);

                control_status = pci_config_read_dword(
                    bus, slot, function, (uint8_t)(capability + 0x08u));
                uint16_t verified_mrrs = (uint16_t)(
                    ((uint16_t)control_status & ALX_PCIE_DEVCTL_MRRS_MASK) >>
                    ALX_PCIE_DEVCTL_MRRS_SHIFT);
                if (verified_mrrs < ALX_PCIE_DEVCTL_MRRS_MIN) {
                    serial_print("[ALX] Failed to raise PCIe MRRS to 512 bytes\n");
                    return 0;
                }

                serial_print("[ALX] PCIe MRRS raised to 512 bytes\n");
            }
            return 1;
        }

        if (next == 0u || next == capability)
            break;
        capability = next;
    }

    serial_print("[ALX] PCIe capability was not found\n");
    return 0;
}

static int alx_wait_mdio(void) {
    for (uint32_t i = 0; i < ALX_MDIO_TIMEOUT; ++i) {
        if ((alx_read32(ALX_MDIO) & ALX_MDIO_BUSY) == 0u)
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

static int alx_mdio_read(uint16_t reg, uint16_t *value) {
    if (!g_mmio || !value || reg > 31u)
        return 0;

    uint32_t command = ALX_MDIO_SPRES_PRMBL | ALX_MDIO_CLK_SEL_25MD128 |
                      ALX_MDIO_START | ALX_MDIO_OP_READ |
                      ((uint32_t)reg << ALX_MDIO_REG_SHIFT);
    alx_write32(ALX_MDIO, command);
    if (!alx_wait_mdio())
        return 0;
    *value = (uint16_t)(alx_read32(ALX_MDIO) & ALX_MDIO_DATA_MASK);
    return 1;
}

static int alx_mdio_write(uint16_t reg, uint16_t value) {
    if (!g_mmio || reg > 31u)
        return 0;

    uint32_t command = ALX_MDIO_SPRES_PRMBL | ALX_MDIO_CLK_SEL_25MD128 |
                      ALX_MDIO_START |
                      ((uint32_t)reg << ALX_MDIO_REG_SHIFT) |
                      (uint32_t)value;
    alx_write32(ALX_MDIO, command);
    return alx_wait_mdio();
}

static int alx_mdio_write_ext(uint8_t dev, uint16_t reg, uint16_t value) {
    if (!g_mmio || dev > 31u)
        return 0;

    alx_write32(ALX_MDIO_EXTN, ((uint32_t)dev << ALX_MDIO_EXTN_DEVAD_SHIFT) |
                               (uint32_t)reg);
    alx_write32(ALX_MDIO, ALX_MDIO_SPRES_PRMBL | ALX_MDIO_CLK_SEL_25MD128 |
                          ALX_MDIO_START | ALX_MDIO_MODE_EXT |
                          (uint32_t)value);
    return alx_wait_mdio();
}

static int alx_phy_debug_write(uint16_t reg, uint16_t value) {
    return alx_mdio_write(ALX_MII_DBG_ADDR, reg) &&
           alx_mdio_write(ALX_MII_DBG_DATA, value);
}

static int alx_phy_debug_read(uint16_t reg, uint16_t *value) {
    return value &&
           alx_mdio_write(ALX_MII_DBG_ADDR, reg) &&
           alx_mdio_read(ALX_MII_DBG_DATA, value);
}

/*
 * Keep the PCIe clock and power state in the same known-good configuration
 * used by the upstream ALX driver before touching the PHY. On some laptop
 * chipsets the PHY can answer MDIO reads while its 25 MHz/PCIe clock policy
 * is still wrong, leaving auto-negotiation enabled but never establishing
 * carrier.
 */
static void alx_enable_aspm(uint8_t l0s_enable, uint8_t l1_enable) {
    uint32_t value = alx_read32(ALX_PMCTRL);

    value &= ~((uint32_t)ALX_PMCTRL_LCKDET_TIMER_MASK
               << ALX_PMCTRL_LCKDET_TIMER_SHIFT);
    value |= (0x0Cu << ALX_PMCTRL_LCKDET_TIMER_SHIFT);
    value |= ALX_PMCTRL_RCVR_WT_1US | ALX_PMCTRL_L1_CLKSW_EN |
             ALX_PMCTRL_L1_SRDSRX_PWD;

    value &= ~((uint32_t)ALX_PMCTRL_L1REQ_TO_MASK
               << ALX_PMCTRL_L1REQ_TO_SHIFT);
    value |= (0x0Fu << ALX_PMCTRL_L1REQ_TO_SHIFT);
    value &= ~((uint32_t)ALX_PMCTRL_L1_TIMER_MASK
               << ALX_PMCTRL_L1_TIMER_SHIFT);
    value |= (4u << ALX_PMCTRL_L1_TIMER_SHIFT);

    value &= ~(ALX_PMCTRL_L1_SRDS_EN | ALX_PMCTRL_L1_SRDSPLL_EN |
               ALX_PMCTRL_L1_BUFSRX_EN | ALX_PMCTRL_SADLY_EN |
               ALX_PMCTRL_HOTRST_WTEN | ALX_PMCTRL_L0S_EN |
               ALX_PMCTRL_L1_EN | ALX_PMCTRL_ASPM_FCEN |
               ALX_PMCTRL_TXL1_AFTER_L0S | ALX_PMCTRL_RXL1_AFTER_L0S);

    /* A0/A1 with CR silicon requires the SRDS L1 clock path. */
    if (g_revision <= 1u && g_revision_cr)
        value |= ALX_PMCTRL_L1_SRDS_EN | ALX_PMCTRL_L1_SRDSPLL_EN;

    if (l0s_enable)
        value |= ALX_PMCTRL_L0S_EN | ALX_PMCTRL_ASPM_FCEN;
    if (l1_enable)
        value |= ALX_PMCTRL_L1_EN | ALX_PMCTRL_ASPM_FCEN;

    alx_write32(ALX_PMCTRL, value);
}

static void alx_reset_pcie(void) {
    uint32_t value;

    /* Clear stale wake state and prevent the PLL from powering off in D3. */
    alx_write32(ALX_WOL0, 0u);
    value = alx_read32(ALX_PDLL_TRNS1);
    alx_write32(ALX_PDLL_TRNS1,
                value & ~ALX_PDLL_TRNS1_D3PLLOFF_EN);

    /* Mask the PCIe protocol errors covered by the upstream ALX workaround. */
    value = alx_read32(ALX_UE_SVRT);
    value &= ~(ALX_UE_SVRT_DLPROTERR | ALX_UE_SVRT_FCPROTERR);
    alx_write32(ALX_UE_SVRT, value);

    /*
     * Keep the 25 MHz wake clock enabled. A0/A1 CR parts select the SRDS
     * clock; later revisions use the non-SRDS PCLK source.
     */
    value = alx_read32(ALX_MASTER);
    if (g_revision <= 1u && g_revision_cr) {
        if ((value & ALX_MASTER_WAKEN_25M) == 0u ||
            (value & ALX_MASTER_PCLKSEL_SRDS) == 0u) {
            value |= ALX_MASTER_PCLKSEL_SRDS | ALX_MASTER_WAKEN_25M;
            alx_write32(ALX_MASTER, value);
        }
    } else {
        if ((value & ALX_MASTER_WAKEN_25M) == 0u ||
            (value & ALX_MASTER_PCLKSEL_SRDS) != 0u) {
            value &= ~ALX_MASTER_PCLKSEL_SRDS;
            value |= ALX_MASTER_WAKEN_25M;
            alx_write32(ALX_MASTER, value);
        }
    }

    alx_enable_aspm(1u, 1u);
    serial_print("[ALX] PCIe power and clock initialization complete\n");
}

static int alx_reset_phy(void) {
    uint32_t value = alx_read32(ALX_PHY_CTRL);
    value &= ~(ALX_PHY_CTRL_DSPRST_OUT | ALX_PHY_CTRL_IDDQ |
               ALX_PHY_CTRL_GATE_25M | ALX_PHY_CTRL_POWER_DOWN |
               ALX_PHY_CTRL_CLS);
    value |= ALX_PHY_CTRL_RST_ANALOG | ALX_PHY_CTRL_HIB_PULSE |
             ALX_PHY_CTRL_HIB_EN;

    alx_write32(ALX_PHY_CTRL, value);
    alx_delay(20000u);
    alx_write32(ALX_PHY_CTRL, value | ALX_PHY_CTRL_DSPRST_OUT);
    alx_delay(1000000u);

    /* Restore the PHY analog defaults used by the vendor ALX driver. */
    if (!alx_phy_debug_write(0x29u, 0x129Du) ||
        !alx_phy_debug_write(0x04u, 0xBB8Bu) ||
        !alx_phy_debug_write(0x12u, 0x4C04u) ||
        !alx_phy_debug_write(0x05u, 0x2C46u) ||
        !alx_phy_debug_write(0x36u, 0xE12Cu) ||
        !alx_phy_debug_write(0x00u, 0x02EFu))
        return 0;

    uint16_t green = 0;
    if (!alx_phy_debug_read(0x3Du, &green))
        return 0;
    if (!alx_phy_debug_write(0x3Du, (uint16_t)(green & ~0x0080u)))
        return 0;

    /* PHY analog and link-pulse settings used by the ALX hardware driver. */
    (void)alx_mdio_write_ext(3u, 0x8062u, 0x0003u);
    (void)alx_mdio_write_ext(7u, 0x003Cu, 0u);
    (void)alx_mdio_write_ext(7u, 0x8027u, 0x8A05u);
    (void)alx_mdio_write_ext(7u, 0x8023u, 0x0000u);
    alx_write32(ALX_LPI_CTRL, alx_read32(ALX_LPI_CTRL) & ~1u);
    return 1;
}

/*
 * Linux alx uses the PHY control field plus the MII debug-page marker to
 * determine whether firmware/another OS already configured this PHY for the
 * requested advertised modes. Preserve that valid state across warm boots.
 */
static int alx_phy_is_configured(void) {
    if ((alx_read32(ALX_PHY_CTRL) & ALX_PHY_CTRL_DSPRST_OUT) == 0u)
        return 0;

    uint32_t driver_config = alx_read32(ALX_DRV);
    uint32_t phy_config =
        (driver_config & ALX_DRV_PHY_FIELD_MASK) >> 21;
    uint32_t expected_config = ALX_DRV_PHY_CONFIG >> 21;
    if (phy_config == 0u || phy_config != expected_config)
        return 0;

    uint16_t debug_page = 0u;
    if (!alx_mdio_read(ALX_MII_DBG_ADDR, &debug_page))
        return 0;

    return debug_page == ALX_PHY_INITED;
}

static int alx_configure_phy(void) {
    const uint16_t advertise_10_100 =
        0x01E1u | ALX_MII_ADVERTISE_PAUSE;
    const uint16_t advertise_1000_full = 0x0200u;

    /*
     * Match Linux alx's setup_speed_duplex sequence: select the normal PHY
     * debug page, program 10/100 and 1000 advertisements, restart autoneg,
     * set the PHY initialized marker, and publish the matching ALX_DRV
     * capability field. In particular, do not advertise 1000BASE-T half
     * duplex: Linux's default capability map advertises 1000 full duplex.
     */
    uint32_t driver_config = alx_read32(ALX_DRV);
    driver_config &= ~ALX_DRV_PHY_FIELD_MASK;

    if (!alx_mdio_write(ALX_MII_DBG_ADDR, 0u) ||
        !alx_mdio_write(ALX_MII_ADVERTISE, advertise_10_100) ||
        !alx_mdio_write(ALX_MII_CTRL1000, advertise_1000_full) ||
        !alx_mdio_write(ALX_MII_BMCR,
                        ALX_BMCR_RESET | ALX_BMCR_ANENABLE |
                        ALX_BMCR_ANRESTART))
        return 0;

    /* Wait for the PHY's self-clearing reset bit before marking it ready. */
    uint16_t control = 0u;
    uint8_t reset_complete = 0u;
    for (uint32_t i = 0; i < 200000u; ++i) {
        if (!alx_mdio_read(ALX_MII_BMCR, &control))
            return 0;
        if ((control & ALX_BMCR_RESET) == 0u) {
            reset_complete = 1u;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!reset_complete) {
        serial_print("[ALX] PHY BMCR reset timed out\n");
        return 0;
    }

    if (!alx_mdio_write(ALX_MII_DBG_ADDR, ALX_PHY_INITED))
        return 0;

    driver_config |= ALX_DRV_PHY_CONFIG;
    alx_write32(ALX_DRV, driver_config);

    serial_print("[ALX] PHY auto-negotiation started (10/100/1000)\n");
    return 1;
}

static int alx_reset_mac(void) {
    uint32_t pmctrl_saved = 0u;
    uint8_t a_cr = (g_revision <= 1u && g_revision_cr) ? 1u : 0u;

    alx_write32(ALX_IMR, 0u);
    alx_write32(ALX_MSIX_MASK, 0xFFFFFFFFu);
    alx_write32(ALX_ISR, ALX_ISR_DIS);

    alx_write32(ALX_RXQ0, alx_read32(ALX_RXQ0) & ~ALX_RXQ0_EN);
    alx_write32(ALX_TXQ0, alx_read32(ALX_TXQ0) & ~ALX_TXQ0_EN);

    /*
     * Give both descriptor engines time to stop before disabling packet
     * traffic in MAC_CTRL. The ALX hardware sequence uses a 40 us settle
     * interval here; this early-boot driver uses a conservative pause loop
     * because PIT timekeeping is not initialized yet.
     */
    alx_delay(100000u);

    alx_write32(ALX_MAC_CTRL,
                alx_read32(ALX_MAC_CTRL) &
                ~(ALX_MAC_CTRL_RX_EN | ALX_MAC_CTRL_TX_EN));

    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((alx_read32(ALX_MAC_STS) & ALX_MAC_STS_IDLE) == 0u)
            break;
        if (i == 99999u) {
            serial_print("[ALX] MAC did not become idle before reset\n");
            return 0;
        }
        __asm__ volatile("pause");
    }

    /*
     * On A0/A1 CR silicon, temporarily leave ASPM while resetting the MAC.
     * This is the same guard used by the upstream ALX reset path.
     */
    if (a_cr) {
        pmctrl_saved = alx_read32(ALX_PMCTRL);
        if (pmctrl_saved & (ALX_PMCTRL_L1_EN | ALX_PMCTRL_L0S_EN)) {
            alx_write32(ALX_PMCTRL,
                        pmctrl_saved & ~(ALX_PMCTRL_L1_EN |
                                         ALX_PMCTRL_L0S_EN));
        }
    }

    alx_write32(ALX_RFD_PIDX, 1u);
    uint32_t master = alx_read32(ALX_MASTER);
    alx_write32(ALX_MASTER, master | ALX_MASTER_DMA_MAC_RST |
                                  ALX_MASTER_OOB_DIS);

    /* Linux waits 10 us before checking the controller's reset handshake. */
    alx_delay(10000u);

    uint32_t reset_poll = 0u;
    for (; reset_poll < ALX_WAIT_LIMIT; ++reset_poll) {
        if (alx_read16(ALX_RFD_PIDX) == 0u)
            break;
        __asm__ volatile("pause");
    }
    if (reset_poll == ALX_WAIT_LIMIT) {
        serial_print("[ALX] MAC/DMA reset did not clear RFD producer index\n");
        return 0;
    }

    for (; reset_poll < ALX_WAIT_LIMIT; ++reset_poll) {
        master = alx_read32(ALX_MASTER);
        if ((master & ALX_MASTER_DMA_MAC_RST) == 0u)
            break;
        __asm__ volatile("pause");
    }
    if (reset_poll == ALX_WAIT_LIMIT) {
        serial_print("[ALX] MAC/DMA reset bit did not self-clear\n");
        return 0;
    }

    alx_delay(10000u);

    if (a_cr) {
        master = alx_read32(ALX_MASTER);
        alx_write32(ALX_MASTER, master | ALX_MASTER_PCLKSEL_SRDS);
        if (pmctrl_saved & (ALX_PMCTRL_L1_EN | ALX_PMCTRL_L0S_EN))
            alx_write32(ALX_PMCTRL, pmctrl_saved);
    }

    /*
     * Match Linux alx_reset_osc(): select the internal 25 MHz source before
     * toggling/calibrating the oscillator. Doing this only after calibration
     * leaves the calibration dependent on the inherited clock source.
     */
    uint32_t misc3 = alx_read32(ALX_MISC3);
    alx_write32(ALX_MISC3,
                (misc3 & ~ALX_MISC3_25M_BY_SW) | ALX_MISC3_25M_NOTO_INTNL);

    /* Recalibrate the internal oscillator after the MAC reset. */
    uint32_t misc = alx_read32(ALX_MISC);
    if (g_revision >= 2u) {
        /* B0+: restore OCP default, toggle the internal oscillator, calibrate. */
        misc &= ~ALX_MISC_INTNLOSC_OPEN;
        misc = (misc & ~ALX_MISC_PSW_OCP_MASK) | ALX_MISC_PSW_OCP_DEF;
        alx_write32(ALX_MISC, misc);
        alx_write32(ALX_MISC, misc | ALX_MISC_INTNLOSC_OPEN);

        uint32_t misc2 = alx_read32(ALX_MSIC2) & ~ALX_MSIC2_CALB_START;
        alx_write32(ALX_MSIC2, misc2);
        alx_write32(ALX_MSIC2, misc2 | ALX_MSIC2_CALB_START);
    } else {
        /* A0/A1: pulse the internal oscillator open bit, then close it. */
        misc &= ~ALX_MISC_INTNLOSC_OPEN;
        if (g_revision == 0u)
            misc &= ~ALX_MISC_ISO_EN;
        alx_write32(ALX_MISC, misc | ALX_MISC_INTNLOSC_OPEN);
        alx_write32(ALX_MISC, misc);
    }

    /*
     * Common finalization from the hardware driver: select the internal
     * 25 MHz source and ensure the oscillator-open bit is cleared. The
     * previous B0+ path left this bit set after calibration.
     */
    misc3 = alx_read32(ALX_MISC3);
    alx_write32(ALX_MISC3,
                (misc3 & ~ALX_MISC3_25M_BY_SW) | ALX_MISC3_25M_NOTO_INTNL);
    misc = alx_read32(ALX_MISC);
    misc &= ~ALX_MISC_INTNLOSC_OPEN;
    if (g_revision <= 1u)
        misc &= ~ALX_MISC_ISO_EN;
    alx_write32(ALX_MISC, misc);

    alx_delay(5000u);
    return 1;
}

static int alx_valid_mac(void) {
    uint8_t any = 0;
    for (uint32_t i = 0; i < 6u; ++i)
        any |= g_mac[i];
    return any != 0u && (g_mac[0] & 1u) == 0u &&
           !(g_mac[0] == 0xFFu && g_mac[1] == 0xFFu &&
             g_mac[2] == 0xFFu && g_mac[3] == 0xFFu &&
             g_mac[4] == 0xFFu && g_mac[5] == 0xFFu);
}

static int alx_read_mac_address(void) {
    uint32_t low = alx_read32(ALX_STAD0);
    uint32_t high = alx_read32(ALX_STAD1);

    g_mac[0] = (uint8_t)(high >> 8);
    g_mac[1] = (uint8_t)high;
    g_mac[2] = (uint8_t)(low >> 24);
    g_mac[3] = (uint8_t)(low >> 16);
    g_mac[4] = (uint8_t)(low >> 8);
    g_mac[5] = (uint8_t)low;
    return alx_valid_mac();
}

static void alx_log_mac(void) {
    static const char hex[] = "0123456789ABCDEF";
    serial_print("[ALX] MAC=");
    for (uint32_t i = 0; i < 6u; ++i) {
        char out[2] = {hex[g_mac[i] >> 4], 0};
        serial_print(out);
        out[0] = hex[g_mac[i] & 0x0Fu];
        serial_print(out);
        serial_print(i == 5u ? "\n" : ":");
    }
}

static int alx_setup_rings(void) {
    uint64_t rfd_address = (uint64_t)(uintptr_t)g_rfd;
    uint64_t rrd_address = (uint64_t)(uintptr_t)g_rrd;
    uint64_t tpd_address = (uint64_t)(uintptr_t)g_tpd;
    uint32_t rx_high = (uint32_t)(rrd_address >> 32);

    if (sizeof(alx_tpd_t) != 16u || sizeof(alx_rfd_t) != 8u ||
        sizeof(alx_rrd_t) != 16u ||
        (rfd_address >> 32) != rx_high ||
        (rfd_address & 0xFFFu) != 0u ||
        (rrd_address & 0xFFFu) != 0u ||
        (tpd_address & 0xFFFu) != 0u ||
        !alx_dma_address_ok(g_rfd, sizeof(g_rfd)) ||
        !alx_dma_address_ok(g_rrd, sizeof(g_rrd)) ||
        !alx_dma_address_ok(g_tpd, sizeof(g_tpd)) ||
        !alx_dma_address_ok(g_rx_buffers, sizeof(g_rx_buffers)) ||
        !alx_dma_address_ok(g_tx_buffers, sizeof(g_tx_buffers))) {
        serial_print("[ALX] DMA ring alignment/address validation failed\n");
        return 0;
    }

    alx_zero(g_tpd, sizeof(g_tpd));
    alx_zero(g_rfd, sizeof(g_rfd));
    alx_zero(g_rrd, sizeof(g_rrd));
    alx_zero(g_rx_buffers, sizeof(g_rx_buffers));
    alx_zero(g_tx_buffers, sizeof(g_tx_buffers));

    for (uint32_t i = 0; i < ALX_RING_COUNT; ++i) {
        uint64_t rx_address = (uint64_t)(uintptr_t)g_rx_buffers[i];
        uint64_t tx_address = (uint64_t)(uintptr_t)g_tx_buffers[i];
        if (!alx_dma_address_ok(g_rx_buffers[i], ALX_RX_BUFFER_BYTES) ||
            !alx_dma_address_ok(g_tx_buffers[i], ALX_FRAME_MAX) ||
            (rx_address & 3u) != 0u || (tx_address & 3u) != 0u) {
            serial_print("[ALX] DMA packet buffer validation failed\n");
            return 0;
        }
        g_rfd[i].address = rx_address;
    }

    g_rx_read = 0u;
    g_rx_write = ALX_RING_COUNT - 1u;
    g_rrd_read = 0u;
    g_rx_fault_index = 0u;
    g_rx_fault_count = 0u;
    g_rx_ring_fault = 0u;
    g_tx_inflight = 0u;
    g_tx_stall_polls = 0u;
    g_tx_last_consumer = 0u;
    g_tx_reject_reported = 0u;

    alx_write32(ALX_RX_BASE_ADDR_HI, rx_high);
    alx_write32(ALX_TX_BASE_ADDR_HI, (uint32_t)(tpd_address >> 32));
    alx_write32(ALX_RFD_ADDR_LO, (uint32_t)rfd_address);
    alx_write32(ALX_RRD_ADDR_LO, (uint32_t)rrd_address);
    alx_write32(ALX_TPD_PRI0_ADDR_LO, (uint32_t)tpd_address);

    alx_write32(ALX_RFD_RING_SZ, ALX_RING_COUNT);
    alx_write32(ALX_RRD_RING_SZ, ALX_RING_COUNT);
    alx_write32(ALX_RFD_BUF_SZ, ALX_RX_BUFFER_BYTES);
    alx_write32(ALX_TPD_RING_SZ, ALX_RING_COUNT);

    /* Publish the descriptor bases before asking the chip to load them. */
    __asm__ volatile("mfence" ::: "memory");
    alx_write32(ALX_SRAM9, 1u);

    /*
     * Match the upstream ALX ring setup: software producer/consumer indices
     * start at zero, descriptor addresses and ring sizes are published, then
     * SRAM_LOAD_PTR asks the controller to latch the new ring pointers.
     *
     * Do not write TPD_PIDX or require an immediate TPD_CIDX==0 readback here.
     * The pointer-load operation is a hardware command; the consumer register
     * is owned by the device and is not a reliable initialization handshake.
     * The first actual descriptor submission publishes producer index 1.
     */
    g_tx_next = 0u;
    g_tx_inflight = 0u;
    g_tx_last_consumer = 0u;
    g_tx_stall_polls = 0u;
    __asm__ volatile("mfence" ::: "memory");

    /* Keep one RFD slot unposted so producer and consumer never alias. */
    alx_write16(ALX_RFD_PIDX, (uint16_t)(ALX_RING_COUNT - 1u));
    serial_print("[ALX] TX ring bases loaded; software indices initialized at zero\n");
    return 1;
}

static void alx_configure_basic(void) {
    uint32_t value;

    g_mac_ctrl_base = ALX_MAC_CTRL_WOLSPED_SWEN |
                      ALX_MAC_CTRL_MHASH_ALG_HI5B |
                      ALX_MAC_CTRL_BRD_EN |
                      ALX_MAC_CTRL_PCRCE |
                      ALX_MAC_CTRL_CRCE |
                      ALX_MAC_CTRL_RXFC_EN |
                      ALX_MAC_CTRL_TXFC_EN |
                      (7u << ALX_MAC_CTRL_PRMBLEN_SHIFT);

    alx_write32(ALX_CLK_GATE, ALX_CLK_GATE_ALL);
    if (g_revision >= 2u)
        alx_write32(ALX_IDLE_DECISN_TIMER, ALX_IDLE_DECISN_TIMER_DEF);

    value = alx_read32(ALX_MASTER);
    value |= ALX_MASTER_IRQMOD2_EN | ALX_MASTER_IRQMOD1_EN |
             ALX_MASTER_SYSALVTIMER_EN;
    alx_write32(ALX_MASTER, value);
    alx_write32(ALX_IRQ_MODU_TIMER, 100u);
    alx_write32(ALX_INT_RETRIG, 20000u);
    alx_write32(ALX_SMB_TIMER, 400u * 500u);
    alx_write32(ALX_TINT_TPD_THRSHLD, ALX_RING_COUNT / 3u);
    alx_write32(ALX_TINT_TIMER, 200u);
    alx_write32(ALX_MTU, 1522u);
    alx_write32(ALX_TXQ1, ((1522u + 7u) >> 3) |
                          ALX_TXQ1_ERRLGPKT_DROP_EN);

    value = ALX_TXQ0_TPD_BURSTPREF |
            ALX_TXQ0_MODE_ENHANCE | ALX_TXQ0_SUPT_IPOPT |
            (1u << 7) | (0x200u << 16);
    alx_write32(ALX_TXQ0, value);
    alx_write32(ALX_HQTPD, ALX_HQTPD_BURST_EN |
                           (5u << 8) | (5u << 4) | 5u);

    /*
     * Keep receive steering in the same single-queue mode as Linux alx.
     * This driver has one RFD/RRD queue and no RSS indirection table, so RSS
     * hash generation must remain disabled. Enabling RSS without configuring
     * the hardware table/queue mapping can send traffic away from queue zero.
     */
    value = ALX_RXQ0_NUM_RFD_PREF | ALX_RXQ0_RSS_MODE_DIS |
            ALX_RXQ0_IDT_TBL_SIZE | ALX_RXQ0_RSS_HSTYP_ALL |
            ALX_RXQ0_IPV6_PARSE_EN | ALX_RXQ0_ASPM_THRESH_100M;
    alx_write32(ALX_RXQ0, value);
    value = alx_read32(ALX_RXQ0) & ~ALX_RXQ0_RSS_HASH_EN;
    alx_write32(ALX_RXQ0, value);

    uint32_t sram_bytes =
        (alx_read32(ALX_SRAM5) & 0x0FFFu) << 3;
    if (sram_bytes >= ALX_RX_BUFFER_BYTES) {
        uint32_t xon = sram_bytes > 8192u ?
                       (sram_bytes - 3212u) >> 3 :
                       (sram_bytes - ALX_RX_BUFFER_BYTES) >> 3;
        alx_write32(ALX_RXQ2, ((ALX_RX_BUFFER_BYTES >> 3) << 16) |
                              (xon & 0xFFFFu));
    }

    /* Match Linux's minimum 512-byte PCIe read request; four channels on B0+. */
    uint32_t channels = g_revision >= 2u ? 4u : 2u;
    value = (4u << 0) | (1u << 10) | (2u << 4) |
            (15u << 11) | (4u << 16) | ((channels - 1u) << 26);
    alx_write32(ALX_DMA, value);
    alx_write32(ALX_MAC_CTRL, g_mac_ctrl_base);
    alx_write32(ALX_IMR, 0u);
    alx_write32(ALX_MSIX_MASK, 0xFFFFFFFFu);
}

static void alx_stop_mac(void) {
    if (g_mac_started && g_link_up) {
        serial_print("[ALX] Physical Ethernet link lost; stopping MAC\n");
        desktop_debug_set_network_status("[ALX] Ethernet link lost; TX is paused until link negotiation succeeds");
    }
    alx_write32(ALX_RXQ0, alx_read32(ALX_RXQ0) & ~ALX_RXQ0_EN);
    alx_write32(ALX_TXQ0, alx_read32(ALX_TXQ0) & ~ALX_TXQ0_EN);
    alx_write32(ALX_MAC_CTRL,
                alx_read32(ALX_MAC_CTRL) &
                ~(ALX_MAC_CTRL_RX_EN | ALX_MAC_CTRL_TX_EN));
    g_mac_started = 0u;
    g_link_up = 0u;
}

static void alx_start_mac(uint8_t speed_code, uint8_t full_duplex) {
    uint32_t control = g_mac_ctrl_base & ~ALX_MAC_CTRL_SPEED_MASK;
    control |= ((uint32_t)speed_code << ALX_MAC_CTRL_SPEED_SHIFT);
    if (full_duplex)
        control |= ALX_MAC_CTRL_FULLD;
    control |= ALX_MAC_CTRL_RX_EN | ALX_MAC_CTRL_TX_EN;

    /*
     * Match the upstream Linux ALX start sequence: enable the descriptor
     * queues before enabling packet traffic in the MAC. This avoids exposing
     * the MAC to TX/RX traffic before the queue engines are ready.
     */
    alx_write32(ALX_RXQ0, alx_read32(ALX_RXQ0) | ALX_RXQ0_EN);
    alx_write32(ALX_TXQ0, alx_read32(ALX_TXQ0) | ALX_TXQ0_EN);
    alx_write32(ALX_MAC_CTRL, control);
    g_link_speed_code = speed_code;
    g_link_full_duplex = full_duplex;
    g_mac_started = 1u;
    g_link_up = 1u;

    serial_print("[ALX] Ethernet link established at ");
    serial_print(speed_code == 2u ? "1000 Mbps" : "100 Mbps");
    serial_print(full_duplex ? " full duplex\n" : " half duplex\n");
    desktop_debug_set_network_status(
        speed_code == 2u
            ? "[ALX] Ethernet link established at 1000 Mbps"
            : "[ALX] Ethernet link established at 100 Mbps");
}

static void alx_log_phy_status(uint16_t bmsr, uint16_t bmcr,
                               uint16_t status) {
    static uint16_t last_bmsr = 0xFFFFu;
    static uint16_t last_bmcr = 0xFFFFu;
    static uint16_t last_status = 0xFFFFu;
    static const char hex[] = "0123456789ABCDEF";

    if (bmsr == last_bmsr && bmcr == last_bmcr &&
        status == last_status)
        return;

    last_bmsr = bmsr;
    last_bmcr = bmcr;
    last_status = status;

    char value[7] = {'0', 'x', '0', '0', '0', '0', '\0'};
    serial_print("[ALX] PHY BMSR=");
    for (uint32_t i = 0; i < 4u; ++i)
        value[2u + i] = hex[(bmsr >> ((3u - i) * 4u)) & 0x0Fu];
    serial_print(value);

    serial_print(" BMCR=");
    for (uint32_t i = 0; i < 4u; ++i)
        value[2u + i] = hex[(bmcr >> ((3u - i) * 4u)) & 0x0Fu];
    serial_print(value);

    serial_print(" PSSR=");
    for (uint32_t i = 0; i < 4u; ++i)
        value[2u + i] = hex[(status >> ((3u - i) * 4u)) & 0x0Fu];
    serial_print(value);
    serial_print("\n");
}

static int alx_recover_datapath(void);

static void alx_update_link(void) {
    uint16_t bmsr = 0u;
    uint16_t bmcr = 0u;
    uint16_t status = 0u;

    if (!g_datapath_ready)
        return;

    /* BMSR link state is latched low; read it twice. */
    if (!alx_mdio_read(ALX_MII_BMSR, &bmsr) ||
        !alx_mdio_read(ALX_MII_BMSR, &bmsr) ||
        !alx_mdio_read(ALX_MII_BMCR, &bmcr) ||
        !alx_mdio_read(ALX_MII_GIGA_PSSR, &status)) {
        if (g_mdio_status_failures < 3u)
            g_mdio_status_failures++;
        if (g_mdio_status_failures == 1u)
            serial_print("[ALX] PHY status read failed; retaining current MAC state while retrying\n");
        if (g_mdio_status_failures >= 3u) {
            serial_print("[ALX] PHY status reads failed repeatedly; stopping MAC for safety\n");
            alx_stop_mac();
        }
        return;
    }

    g_mdio_status_failures = 0u;
    alx_log_phy_status(bmsr, bmcr, status);

    if ((bmsr & ALX_BMSR_LSTATUS) == 0u) {
        if (g_mac_started) {
            alx_stop_mac();
            /* Linux resets the MAC and reinitializes both DMA rings on link loss. */
            (void)alx_recover_datapath();
        } else {
            g_link_up = 0u;
        }
        return;
    }

    if ((status & ALX_GIGA_PSSR_RESOLVED) == 0u) {
        if (g_mac_started) {
            alx_stop_mac();
            (void)alx_recover_datapath();
        } else {
            g_link_up = 0u;
        }
        return;
    }

    uint8_t speed_code;
    switch (status & ALX_GIGA_PSSR_SPEED_MASK) {
    case ALX_GIGA_PSSR_1000:
        speed_code = 2u;
        break;
    case ALX_GIGA_PSSR_100:
    case 0u: /* 10 and 100 Mbps share the MAC's 10/100 speed setting. */
        speed_code = 1u;
        break;
    default:
        if (g_mac_started) {
            alx_stop_mac();
            (void)alx_recover_datapath();
        } else {
            g_link_up = 0u;
        }
        return;
    }

    uint8_t full_duplex =
        (status & ALX_GIGA_PSSR_DUPLEX) ? 1u : 0u;
    if (!g_mac_started || !g_link_up ||
        speed_code != g_link_speed_code ||
        full_duplex != g_link_full_duplex)
        alx_start_mac(speed_code, full_duplex);
    else
        g_link_up = 1u;
}

static int alx_recover_datapath(void) {
    if (!g_ready || !g_mmio)
        return 0;

    /*
     * Follow Linux alx's reset/re-init path: stop traffic, reset the MAC/DMA
     * engine, reload ring pointers, then restore queue/MAC configuration.
     */
    g_datapath_ready = 0u;
    g_mac_started = 0u;
    g_link_up = 0u;
    ++g_tx_recovery_count;
    serial_print("[ALX] Recovering Ethernet datapath: resetting MAC and DMA rings\n");
    desktop_debug_set_network_status("[ALX] Recovering TX: resetting MAC and DMA rings");

    alx_stop_mac();
    if (!alx_reset_mac())
        goto failed;
    if (!alx_setup_rings())
        goto failed;

    alx_configure_basic();
    g_tx_inflight = 0u;
    g_tx_last_consumer = 0u;
    g_tx_stall_polls = 0u;
    g_mdio_status_failures = 0u;
    g_datapath_ready = 1u;

    /* Re-check the PHY on the next poll; do not start the MAC blindly. */
    g_link_poll_count = 63u;
    serial_print("[ALX] MAC reset and DMA ring reinitialization completed\n");
    desktop_debug_set_network_status("[ALX] TX recovery complete; checking PHY link");
    return 1;

failed:
    g_datapath_ready = 0u;
    g_mac_started = 0u;
    g_link_up = 0u;
    serial_print("[ALX] ERROR: Ethernet datapath recovery failed; TX disabled\n");
    desktop_debug_set_network_status("[ALX] ERROR: Ethernet recovery failed; reboot required");
    return 0;
}

int alx_init(uint8_t bus, uint8_t slot, uint8_t function) {
    if (g_ready)
        return 1;

    uint32_t identity = pci_config_read_dword(bus, slot, function, 0x00u);
    if ((uint16_t)identity != ALX_VENDOR_ID ||
        (uint16_t)(identity >> 16) != ALX_DEVICE_ID_AR8171) {
        serial_print("[ALX] Unsupported PCI identity\n");
        return 0;
    }

    uint32_t bar0 = pci_config_read_dword(bus, slot, function, 0x10u);
    uint32_t bar1 = pci_config_read_dword(bus, slot, function, 0x14u);
    if (bar0 == 0u || bar0 == 0xFFFFFFFFu || (bar0 & 1u)) {
        serial_print("[ALX] BAR0 is not a usable MMIO BAR\n");
        return 0;
    }

    uint64_t mmio_base = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if ((bar0 & 0x6u) == 0x4u)
        mmio_base |= (uint64_t)bar1 << 32;
    else if ((bar0 & 0x6u) != 0u) {
        serial_print("[ALX] Unsupported BAR0 memory type\n");
        return 0;
    }
    if (mmio_base == 0u || (mmio_base & 0xFFFu) != 0u ||
        mmio_base >= (1ull << 52) ||
        mmio_base + ALX_MMIO_BYTES > (1ull << 52)) {
        serial_print("[ALX] Invalid MMIO base\n");
        return 0;
    }

    uint32_t command_status =
        pci_config_read_dword(bus, slot, function, 0x04u);
    uint16_t command = (uint16_t)command_status;
    command |= 0x0007u;       /* PCI I/O, memory decoding + bus mastering. */
    command &= (uint16_t)~0x0400u; /* Ensure INTx is not disabled. */
    pci_config_write_dword(bus, slot, function, 0x04u, command);
    if (!alx_configure_pcie_read_request(bus, slot, function))
        return 0;

    const uint64_t map_flags = PAGE_PRESENT | PAGE_WRITE |
                               (1ull << 3) | (1ull << 4);
    for (uint64_t offset = 0; offset < ALX_MMIO_BYTES; offset += 4096u) {
        void *address = (void *)(uintptr_t)(mmio_base + offset);
        if (!VMM_MapPage(address, address, map_flags)) {
            serial_print("[ALX] Failed to map MMIO registers\n");
            g_mmio = NULL;
            return 0;
        }
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)mmio_base;

    uint32_t class_revision =
        pci_config_read_dword(bus, slot, function, 0x08u);
    uint8_t revision_id = (uint8_t)class_revision;
    g_revision = revision_id >> 3;
    g_revision_cr = revision_id & 1u;
    if (g_revision > 3u) {
        serial_print("[ALX] Unsupported ALX silicon revision\n");
        g_mmio = NULL;
        return 0;
    }

    serial_print("[ALX] Qualcomm Atheros AR8171 detected (1969:10A1) revision=");
    serial_print(g_revision >= 2u ? "B0+" : "A0/A1");
    serial_print(g_revision_cr ? " CR\n" : "\n");
    alx_write32(ALX_IMR, 0u);
    alx_write32(ALX_MSIX_MASK, 0xFFFFFFFFu);

    /* This must precede PHY configuration, as in Linux alx. */
    alx_reset_pcie();
    alx_delay(10000u); /* Linux allows the PCIe clock/power policy to settle. */

    uint8_t phy_configured = alx_phy_is_configured() ? 1u : 0u;
    if (phy_configured) {
        serial_print("[ALX] PHY configuration already matches; preserving it\n");
    } else {
        serial_print("[ALX] PHY configuration missing or stale; resetting PHY\n");
        if (!alx_reset_phy()) {
            serial_print("[ALX] PHY reset/MDIO initialization failed\n");
            g_mmio = NULL;
            return 0;
        }
    }

    if (!alx_reset_mac()) {
        g_mmio = NULL;
        return 0;
    }
    if (!alx_read_mac_address()) {
        serial_print("[ALX] No valid factory MAC address in station registers\n");
        g_mmio = NULL;
        return 0;
    }

    alx_configure_basic();
    if (!alx_setup_rings()) {
        g_mmio = NULL;
        return 0;
    }
    if (!phy_configured && !alx_configure_phy()) {
        serial_print("[ALX] PHY auto-negotiation setup failed\n");
        g_mmio = NULL;
        return 0;
    }

    g_ready = 1u;
    g_datapath_ready = 1u;
    g_link_up = 0u;
    g_mac_started = 0u;
    g_link_poll_count = 511u;
    alx_update_link();

    serial_print("[ALX] AR8171 polling driver initialized; DMA rings configured\n");
    alx_log_mac();
    serial_print(g_link_up ? "[ALX] Link is up\n" :
                             "[ALX] Link is down (device ready)\n");
    desktop_debug_set_network_status(
        g_link_up ? "[ALX] Ethernet link is up; waiting for TX probe"
                  : "[ALX] Ethernet device ready, but link is down; TX cannot start");
    return 1;
}

int alx_is_ready(void) {
    return g_ready != 0u;
}

int alx_link_up(void) {
    return g_ready && g_datapath_ready && g_link_up;
}

const uint8_t *alx_mac_address(void) {
    return g_ready ? g_mac : NULL;
}

static int alx_tx_reap(uint16_t hardware_consumer) {
    if (hardware_consumer >= ALX_RING_COUNT)
        return 0;

    if (g_tx_inflight == 0u) {
        /* Establish a fresh completion baseline whenever the ring is empty. */
        g_tx_last_consumer = hardware_consumer;
        g_tx_stall_polls = 0u;
        return 1;
    }

    const uint16_t completed = (uint16_t)(
        (hardware_consumer + ALX_RING_COUNT - g_tx_last_consumer) %
        ALX_RING_COUNT);
    if (completed > g_tx_inflight)
        return 0;

    if (completed != 0u) {
        g_tx_inflight = (uint16_t)(g_tx_inflight - completed);
        g_tx_last_consumer = hardware_consumer;
        g_tx_stall_polls = 0u;
        if (!g_tx_completion_reported) {
            g_tx_completion_reported = 1u;
            serial_print("[ALX] TX completion observed: hardware consumed descriptors\n");
        }
    }
    return 1;
}

int alx_send(const void *frame, uint16_t length) {
    if (!g_ready || !g_datapath_ready || !g_link_up || !frame ||
        length < 14u || length > ALX_FRAME_MAX) {
        if (g_ready && !g_tx_reject_reported) {
            g_tx_reject_reported = 1u;
            serial_print("[ALX] TX rejected before descriptor submission (link/state/frame)\n");
            desktop_debug_set_network_status("[ALX] TX rejected: link is down, driver is not ready, or frame is invalid");
        }
        return 0;
    }

    uint16_t hardware_consumer = alx_read16(ALX_TPD_PRI0_CIDX);
    if (hardware_consumer >= ALX_RING_COUNT ||
        !alx_tx_reap(hardware_consumer)) {
        if (!g_tx_reject_reported) {
            g_tx_reject_reported = 1u;
            serial_print("[ALX] TX rejected: hardware consumer state is invalid or out of sync\n");
            desktop_debug_set_network_status("[ALX] TX halted: consumer index is invalid or out of sync");
        }
        return 0;
    }

    uint16_t next = (uint16_t)((g_tx_next + 1u) % ALX_RING_COUNT);
    if (next == hardware_consumer ||
        g_tx_inflight >= ALX_RING_COUNT - 1u) {
        if (!g_tx_reject_reported) {
            g_tx_reject_reported = 1u;
            serial_print("[ALX] TX ring full; hardware consumer has not freed a descriptor\n");
            desktop_debug_set_network_status("[ALX] TX ring full: hardware has not freed a descriptor");
        }
        return 0;
    }

    uint16_t index = g_tx_next;
    alx_copy(g_tx_buffers[index], frame, length);
    g_tpd[index].length = length;
    g_tpd[index].vlan_tag = 0u;
    g_tpd[index].word1 = ALX_TPD_EOP;
    g_tpd[index].address =
        (uint64_t)(uintptr_t)g_tx_buffers[index];

    __asm__ volatile("mfence" ::: "memory");
    g_tx_next = next;
    alx_write16(ALX_TPD_PRI0_PIDX, g_tx_next);
    /* Flush the posted MMIO write and catch a producer register that failed to stick. */
    if (alx_read16(ALX_TPD_PRI0_PIDX) != g_tx_next) {
        serial_print("[ALX] WARNING: TX producer-index readback differs after submission\n");
        desktop_debug_set_network_status("[ALX] TX warning: producer-index readback mismatch");
    }

    ++g_tx_inflight;
    g_tx_reject_reported = 0u;
    return 1;
}
int alx_receive(void *frame, uint16_t capacity, uint16_t *length) {
    if (!g_ready || !g_datapath_ready || !frame || !length || capacity == 0u)
        return 0;

    alx_rrd_t *completion = &g_rrd[g_rrd_read];
    __asm__ volatile("mfence" ::: "memory");
    uint32_t word3 = completion->word3;
    if ((word3 & (1u << ALX_RFD_UPDATED_SHIFT)) == 0u)
        return 0;

    uint32_t word0 = completion->word0;
    uint16_t descriptor_index =
        (uint16_t)((word0 >> ALX_RRD_SI_SHIFT) & ALX_RRD_SI_MASK);
    uint8_t descriptor_count =
        (uint8_t)((word0 >> ALX_RRD_NOR_SHIFT) & ALX_RRD_NOR_MASK);
    uint16_t packet_length = (uint16_t)(word3 & ALX_RRD_PKTLEN_MASK);

    /*
     * Linux treats an SI/NOR mismatch as ring corruption and schedules a
     * device reset. Do not advance either software ring index or recycle an
     * unrelated RFD: doing so would make every later completion misaligned.
     */
    if (descriptor_count != 1u ||
        descriptor_index >= ALX_RING_COUNT ||
        descriptor_index != g_rx_read) {
        g_rx_fault_index = descriptor_index;
        g_rx_fault_count = descriptor_count;
        g_rx_ring_fault = 1u;
        return -1;
    }

    int valid = packet_length >= 18u &&
                packet_length <= ALX_RX_BUFFER_BYTES &&
                (packet_length - 4u) <= capacity &&
                (word3 & ALX_RRD_ERROR_MASK) == 0u;

    if (valid) {
        *length = (uint16_t)(packet_length - 4u);
        alx_copy(frame, g_rx_buffers[descriptor_index], *length);
    }

    /* Consume this completion, including ordinary packet-level RX errors. */
    completion->word3 = 0u;
    __asm__ volatile("mfence" ::: "memory");

    /* Return the consumed RFD slot to hardware and advance the producer. */
    g_rfd[g_rx_write].address =
        (uint64_t)(uintptr_t)g_rx_buffers[g_rx_write];
    __asm__ volatile("mfence" ::: "memory");
    g_rx_write = (uint16_t)((g_rx_write + 1u) % ALX_RING_COUNT);
    alx_write16(ALX_RFD_PIDX, g_rx_write);

    g_rx_read = (uint16_t)((g_rx_read + 1u) % ALX_RING_COUNT);
    g_rrd_read = (uint16_t)((g_rrd_read + 1u) % ALX_RING_COUNT);
    return valid ? 1 : -1;
}

void alx_poll(void) {
    if (!g_ready || !g_datapath_ready)
        return;

    if (g_rx_ring_fault) {
        serial_print("[ALX] RX descriptor ring is out of sync; resetting MAC and DMA rings\n");
        char diagnostic[128] = "[ALX] RX ring fault\n";
        size_t used = sizeof("[ALX] RX ring fault\n") - 1u;
        alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  RRD_INDEX=", g_rrd_read);
        alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  RFD_EXPECTED=", g_rx_read);
        alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  RFD_REPORTED=", g_rx_fault_index);
        alx_append_hex32(diagnostic, sizeof(diagnostic), &used, "  RFD_COUNT=", g_rx_fault_count);
        desktop_debug_set_network_status(diagnostic);
        (void)alx_recover_datapath();
        return;
    }

    if (++g_link_poll_count >= 64u) {
        g_link_poll_count = 0u;
        alx_update_link();
        if (!g_datapath_ready)
            return;
    }

    const uint16_t consumer = alx_read16(ALX_TPD_PRI0_CIDX);
    if (consumer >= ALX_RING_COUNT || !alx_tx_reap(consumer)) {
        serial_print("[ALX] TX consumer index is invalid or no longer matches outstanding descriptors\n");
        alx_log_tx_timeout();
        (void)alx_recover_datapath();
        return;
    }

    if (g_tx_inflight == 0u) {
        g_tx_stall_polls = 0u;
        return;
    }

    if (g_tx_stall_polls < 512u)
        ++g_tx_stall_polls;

    if (g_tx_stall_polls >= 512u) {
        serial_print("[ALX] TX descriptors stalled for 512 polls; attempting Linux-style MAC/DMA recovery\n");
        alx_log_tx_timeout();
        (void)alx_recover_datapath();
    }
}
