#include "ahci.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);
void print_hex(uint64_t val);

#define AHCI_HBA_BYTES       0x1100u
#define AHCI_PORTS_OFFSET    0x100u
#define AHCI_PORT_STRIDE     0x80u
#define AHCI_MAX_SECTORS     128u
#define AHCI_SECTOR_SIZE     512u
#define AHCI_DMA_BYTES       (AHCI_MAX_SECTORS * AHCI_SECTOR_SIZE)
#define AHCI_TIMEOUT_SPINS   20000000u

#define HBA_GHC_HR           (1u << 0)
#define HBA_GHC_IE           (1u << 1)
#define HBA_GHC_AE           (1u << 31)
#define HBA_CAP_S64A         (1u << 31)
#define HBA_CAP2_BOH         (1u << 0)

#define PORT_CMD_ST          (1u << 0)
#define PORT_CMD_FRE         (1u << 4)
#define PORT_CMD_FR          (1u << 14)
#define PORT_CMD_CR          (1u << 15)
#define PORT_TFD_ERR         (1u << 0)
#define PORT_TFD_DRQ         (1u << 3)
#define PORT_TFD_BSY         (1u << 7)
#define PORT_IS_TFES         (1u << 30)

#define SATA_SIG_ATA         0x00000101u
#define FIS_TYPE_REG_H2D     0x27u

#define ATA_CMD_IDENTIFY         0xECu
#define ATA_CMD_READ_DMA         0xC8u
#define ATA_CMD_WRITE_DMA        0xCAu
#define ATA_CMD_READ_DMA_EXT     0x25u
#define ATA_CMD_WRITE_DMA_EXT    0x35u
#define ATA_CMD_FLUSH_CACHE      0xE7u
#define ATA_CMD_FLUSH_CACHE_EXT  0xEAu

#define PAGE_WRITE_THROUGH   (1ull << 3)
#define PAGE_CACHE_DISABLE   (1ull << 4)

typedef struct __attribute__((packed)) {
  volatile uint32_t clb;
  volatile uint32_t clbu;
  volatile uint32_t fb;
  volatile uint32_t fbu;
  volatile uint32_t is;
  volatile uint32_t ie;
  volatile uint32_t cmd;
  volatile uint32_t reserved0;
  volatile uint32_t tfd;
  volatile uint32_t sig;
  volatile uint32_t ssts;
  volatile uint32_t sctl;
  volatile uint32_t serr;
  volatile uint32_t sact;
  volatile uint32_t ci;
  volatile uint32_t sntf;
  volatile uint32_t fbs;
  volatile uint32_t reserved1[11];
  volatile uint32_t vendor[4];
} ahci_port_t;

typedef struct __attribute__((packed)) {
  volatile uint32_t cap;
  volatile uint32_t ghc;
  volatile uint32_t is;
  volatile uint32_t pi;
  volatile uint32_t vs;
  volatile uint32_t ccc_ctl;
  volatile uint32_t ccc_pts;
  volatile uint32_t em_loc;
  volatile uint32_t em_ctl;
  volatile uint32_t cap2;
  volatile uint32_t bohc;
  uint8_t reserved[0x74];
  uint8_t vendor[0x60];
  ahci_port_t ports[32];
} ahci_hba_t;

typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint16_t prdtl;
  uint32_t prdbc;
  uint32_t ctba;
  uint32_t ctbau;
  uint32_t reserved[4];
} ahci_command_header_t;

typedef struct __attribute__((packed)) {
  uint32_t dba;
  uint32_t dbau;
  uint32_t reserved;
  uint32_t dbc;
} ahci_prdt_entry_t;

typedef struct __attribute__((packed, aligned(128))) {
  uint8_t cfis[64];
  uint8_t acmd[16];
  uint8_t reserved[48];
  ahci_prdt_entry_t prdt[1];
} ahci_command_table_t;

/*
 * VexOS currently identity-maps its kernel image. These statically allocated,
 * aligned work areas therefore have the same CPU and DMA physical addresses.
 * Keep the transfer buffer bounded: each command transfers at most 64 KiB.
 */
static ahci_command_header_t g_command_list[32]
    __attribute__((aligned(1024)));
static uint8_t g_received_fis[256] __attribute__((aligned(256)));
static ahci_command_table_t g_command_table;
static uint8_t g_dma_buffer[AHCI_DMA_BYTES] __attribute__((aligned(4096)));

static volatile ahci_hba_t *g_hba;
static volatile ahci_port_t *g_port;
static uint64_t g_sector_count;
static int g_ready;
static int g_supports_64bit_dma;
static int g_supports_lba48;
static int g_supports_flush;
static int g_supports_flush_ext;

static void ahci_zero(void *destination, size_t bytes) {
  uint8_t *out = (uint8_t *)destination;
  for (size_t i = 0; i < bytes; ++i)
    out[i] = 0;
}

static void ahci_copy(void *destination, const void *source, size_t bytes) {
  uint8_t *out = (uint8_t *)destination;
  const uint8_t *in = (const uint8_t *)source;
  for (size_t i = 0; i < bytes; ++i)
    out[i] = in[i];
}

static int ahci_wait_mask(volatile uint32_t *reg, uint32_t mask,
                          int want_set) {
  for (uint32_t i = 0; i < AHCI_TIMEOUT_SPINS; ++i) {
    uint32_t value = *reg;
    if (want_set ? ((value & mask) == mask) : ((value & mask) == 0))
      return 1;
  }
  return 0;
}

static int ahci_wait_taskfile_ready(volatile ahci_port_t *port) {
  for (uint32_t i = 0; i < AHCI_TIMEOUT_SPINS; ++i) {
    uint32_t status = port->tfd;
    if (status & PORT_TFD_ERR)
      return 0;
    if ((status & (PORT_TFD_BSY | PORT_TFD_DRQ)) == 0)
      return 1;
  }
  return 0;
}

static int ahci_dma_range_supported(const void *address, uint32_t bytes) {
  uint64_t start = (uint64_t)(uintptr_t)address;
  uint64_t end = start + (uint64_t)bytes - 1u;
  if (end < start)
    return 0;
  if (g_supports_64bit_dma)
    return 1;
  return end <= 0xFFFFFFFFull;
}

static int ahci_stop_engine(volatile ahci_port_t *port) {
  port->cmd &= ~PORT_CMD_ST;
  if (!ahci_wait_mask(&port->cmd, PORT_CMD_CR, 0))
    return 0;

  port->cmd &= ~PORT_CMD_FRE;
  if (!ahci_wait_mask(&port->cmd, PORT_CMD_FR, 0))
    return 0;
  return 1;
}

static int ahci_start_engine(volatile ahci_port_t *port) {
  uint64_t command_list = (uint64_t)(uintptr_t)g_command_list;
  uint64_t received_fis = (uint64_t)(uintptr_t)g_received_fis;

  if ((command_list & 1023u) || (received_fis & 255u))
    return 0;
  if (!ahci_dma_range_supported(g_command_list, sizeof(g_command_list)) ||
      !ahci_dma_range_supported(g_received_fis, sizeof(g_received_fis)) ||
      !ahci_dma_range_supported(&g_command_table, sizeof(g_command_table)) ||
      !ahci_dma_range_supported(g_dma_buffer, sizeof(g_dma_buffer))) {
    serial_print("[AHCI] DMA memory is above the controller address limit\n");
    return 0;
  }

  if (!ahci_stop_engine(port))
    return 0;

  ahci_zero(g_command_list, sizeof(g_command_list));
  ahci_zero(g_received_fis, sizeof(g_received_fis));
  ahci_zero(&g_command_table, sizeof(g_command_table));

  port->clb = (uint32_t)command_list;
  port->clbu = (uint32_t)(command_list >> 32);
  port->fb = (uint32_t)received_fis;
  port->fbu = (uint32_t)(received_fis >> 32);
  port->ie = 0;
  port->is = 0xFFFFFFFFu;
  port->serr = 0xFFFFFFFFu;

  port->cmd |= PORT_CMD_FRE;
  if (!ahci_wait_mask(&port->cmd, PORT_CMD_FR, 1))
    return 0;

  port->cmd |= PORT_CMD_ST;
  if (!ahci_wait_mask(&port->cmd, PORT_CMD_CR, 1))
    return 0;

  return 1;
}

static int ahci_issue_command(uint8_t command, uint64_t lba,
                              uint16_t sector_count, uint32_t byte_count,
                              int write, int use_lba48) {
  if (!g_port || byte_count > AHCI_DMA_BYTES ||
      (byte_count != 0 && !ahci_dma_range_supported(g_dma_buffer, byte_count)))
    return 0;

  if (!ahci_wait_taskfile_ready(g_port))
    return 0;
  if (!ahci_wait_mask(&g_port->ci, 1u, 0))
    return 0;

  ahci_zero(&g_command_list[0], sizeof(g_command_list[0]));
  ahci_zero(&g_command_table, sizeof(g_command_table));

  uint64_t table_address = (uint64_t)(uintptr_t)&g_command_table;
  ahci_command_header_t *header = &g_command_list[0];
  header->flags = (uint16_t)(5u | (write ? (1u << 6) : 0u));
  header->prdtl = byte_count ? 1u : 0u;
  header->ctba = (uint32_t)table_address;
  header->ctbau = (uint32_t)(table_address >> 32);

  if (byte_count) {
    uint64_t data_address = (uint64_t)(uintptr_t)g_dma_buffer;
    g_command_table.prdt[0].dba = (uint32_t)data_address;
    g_command_table.prdt[0].dbau = (uint32_t)(data_address >> 32);
    g_command_table.prdt[0].reserved = 0;
    g_command_table.prdt[0].dbc = (byte_count - 1u) | (1u << 31);
  }

  uint8_t *fis = g_command_table.cfis;
  fis[0] = FIS_TYPE_REG_H2D;
  fis[1] = 1u << 7; /* command FIS */
  fis[2] = command;

  if (command == ATA_CMD_IDENTIFY) {
    fis[7] = 0;
  } else {
    if (use_lba48) {
      fis[7] = 0x40u;
      fis[4] = (uint8_t)(lba);
      fis[5] = (uint8_t)(lba >> 8);
      fis[6] = (uint8_t)(lba >> 16);
      fis[8] = (uint8_t)(lba >> 24);
      fis[9] = (uint8_t)(lba >> 32);
      fis[10] = (uint8_t)(lba >> 40);
      fis[12] = (uint8_t)(sector_count);
      fis[13] = (uint8_t)(sector_count >> 8);
    } else {
      fis[7] = (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu));
      fis[4] = (uint8_t)(lba);
      fis[5] = (uint8_t)(lba >> 8);
      fis[6] = (uint8_t)(lba >> 16);
      fis[12] = (uint8_t)(sector_count);
    }
  }

  g_port->is = 0xFFFFFFFFu;
  g_port->serr = 0xFFFFFFFFu;
  __asm__ volatile("mfence" ::: "memory");

  g_port->ci |= 1u;

  for (uint32_t i = 0; i < AHCI_TIMEOUT_SPINS; ++i) {
    uint32_t interrupt_status = g_port->is;
    uint32_t taskfile_status = g_port->tfd;

    if ((interrupt_status & PORT_IS_TFES) || (taskfile_status & PORT_TFD_ERR)) {
      g_port->is = 0xFFFFFFFFu;
      g_port->serr = 0xFFFFFFFFu;
      return 0;
    }

    if ((g_port->ci & 1u) == 0) {
      __asm__ volatile("mfence" ::: "memory");
      g_port->is = 0xFFFFFFFFu;
      g_port->serr = 0xFFFFFFFFu;
      return 1;
    }
  }

  serial_print("[AHCI] Command timed out\n");
  return 0;
}

static int ahci_identify_device(void) {
  ahci_zero(g_dma_buffer, sizeof(g_dma_buffer));
  if (!ahci_issue_command(ATA_CMD_IDENTIFY, 0, 0, AHCI_SECTOR_SIZE, 0, 0))
    return 0;

  const uint16_t *identify = (const uint16_t *)g_dma_buffer;
  uint16_t command_set_82 = identify[82];
  uint16_t command_set_83 = identify[83];

  g_supports_lba48 = (command_set_83 & (1u << 10)) != 0;
  g_supports_flush = (command_set_82 & (1u << 12)) != 0;
  g_supports_flush_ext = (command_set_83 & (1u << 12)) != 0;

  if (g_supports_lba48) {
    g_sector_count = (uint64_t)identify[100] |
                     ((uint64_t)identify[101] << 16) |
                     ((uint64_t)identify[102] << 32) |
                     ((uint64_t)identify[103] << 48);
  } else {
    g_sector_count = (uint64_t)identify[60] |
                     ((uint64_t)identify[61] << 16);
  }

  if (g_sector_count == 0) {
    serial_print("[AHCI] IDENTIFY returned zero capacity\n");
    return 0;
  }

  return 1;
}

int ahci_init(uint64_t abar) {
  if (g_ready)
    return 1;
  if (abar == 0 || (abar & 0xFFFu) != 0) {
    serial_print("[AHCI] Invalid ABAR address\n");
    return 0;
  }

  serial_print("[AHCI] Initializing HBA at ");
  print_hex(abar);
  serial_print("\n");

  const uint64_t map_flags = PAGE_WRITE | PAGE_WRITE_THROUGH |
                             PAGE_CACHE_DISABLE;
  const uint64_t map_bytes = (AHCI_HBA_BYTES + 4095u) & ~4095ull;
  for (uint64_t offset = 0; offset < map_bytes; offset += 4096u) {
    void *address = (void *)(uintptr_t)(abar + offset);
    if (!VMM_MapPage(address, address, map_flags)) {
      serial_print("[AHCI] Failed to map HBA registers\n");
      return 0;
    }
  }

  g_hba = (volatile ahci_hba_t *)(uintptr_t)abar;
  g_supports_64bit_dma = (g_hba->cap & HBA_CAP_S64A) != 0;

  /* BIOS/OS handoff is optional and only exists when CAP2.BOH is set. */
  if (g_hba->cap2 & HBA_CAP2_BOH) {
    g_hba->bohc |= (1u << 1); /* OOS: OS owns the HBA */
    for (uint32_t i = 0; i < AHCI_TIMEOUT_SPINS; ++i) {
      uint32_t bohc = g_hba->bohc;
      if ((bohc & 1u) == 0 && (bohc & (1u << 4)) == 0)
        break;
      if (i + 1u == AHCI_TIMEOUT_SPINS) {
        serial_print("[AHCI] BIOS handoff timed out\n");
        return 0;
      }
    }
  }

  /* Disable HBA interrupts; this driver polls command completion. */
  g_hba->ghc = (g_hba->ghc & ~HBA_GHC_IE) | HBA_GHC_AE;

  uint32_t implemented_ports = g_hba->pi;
  serial_print("[AHCI] Ports implemented: ");
  print_hex(implemented_ports);
  serial_print("\n");

  for (uint32_t index = 0; index < 32; ++index) {
    if ((implemented_ports & (1u << index)) == 0)
      continue;

    volatile ahci_port_t *port = &g_hba->ports[index];
    uint32_t ssts = port->ssts;
    uint32_t det = ssts & 0x0Fu;
    uint32_t ipm = (ssts >> 8) & 0x0Fu;
    uint32_t signature = port->sig;

    if (det != 3u || ipm != 1u || signature != SATA_SIG_ATA)
      continue;

    serial_print("[AHCI] SATA link found on port ");
    char digit[2] = {(char)('0' + (index % 10u)), 0};
    serial_print(digit);
    serial_print("\n");

    g_port = port;
    if (!ahci_start_engine(port)) {
      serial_print("[AHCI] Could not start port command engine\n");
      g_port = NULL;
      continue;
    }

    if (!ahci_identify_device()) {
      serial_print("[AHCI] IDENTIFY failed on this port\n");
      ahci_stop_engine(port);
      g_port = NULL;
      continue;
    }

    g_ready = 1;
    serial_print("[AHCI] SATA disk ready; sectors=");
    print_hex(g_sector_count);
    serial_print(g_supports_lba48 ? " LBA48\n" : " LBA28\n");
    return 1;
  }

  g_port = NULL;
  serial_print("[AHCI] No usable SATA disk found\n");
  return 0;
}

int ahci_is_ready(void) {
  return g_ready;
}

uint64_t ahci_get_sector_count(void) {
  return g_ready ? g_sector_count : 0;
}

int ahci_read_sectors(uint64_t lba, uint32_t count, void *buffer) {
  if (!g_ready || !buffer || count == 0 ||
      lba >= g_sector_count || (uint64_t)count > g_sector_count - lba)
    return 0;

  uint8_t *destination = (uint8_t *)buffer;
  while (count) {
    uint32_t chunk = count;
    if (chunk > AHCI_MAX_SECTORS)
      chunk = AHCI_MAX_SECTORS;

    uint32_t bytes = chunk * AHCI_SECTOR_SIZE;
    uint8_t command = g_supports_lba48 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;
    if (!g_supports_lba48 && (lba > 0x0FFFFFFFull ||
        (uint64_t)chunk > 0x10000000ull - lba))
      return 0;

    if (!ahci_issue_command(command, lba, (uint16_t)chunk, bytes, 0,
                            g_supports_lba48))
      return 0;

    ahci_copy(destination, g_dma_buffer, bytes);
    destination += bytes;
    lba += chunk;
    count -= chunk;
  }

  return 1;
}

int ahci_write_sectors(uint64_t lba, uint32_t count, const void *buffer) {
  if (!g_ready || !buffer || count == 0 ||
      lba >= g_sector_count || (uint64_t)count > g_sector_count - lba)
    return 0;

  const uint8_t *source = (const uint8_t *)buffer;
  while (count) {
    uint32_t chunk = count;
    if (chunk > AHCI_MAX_SECTORS)
      chunk = AHCI_MAX_SECTORS;

    uint32_t bytes = chunk * AHCI_SECTOR_SIZE;
    uint8_t command = g_supports_lba48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA;
    if (!g_supports_lba48 && (lba > 0x0FFFFFFFull ||
        (uint64_t)chunk > 0x10000000ull - lba))
      return 0;

    ahci_copy(g_dma_buffer, source, bytes);
    if (!ahci_issue_command(command, lba, (uint16_t)chunk, bytes, 1,
                            g_supports_lba48))
      return 0;

    source += bytes;
    lba += chunk;
    count -= chunk;
  }

  /* Make completed writes durable when the device advertises a flush command. */
  if (g_supports_flush_ext) {
    if (!ahci_issue_command(ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0, 0, 1))
      return 0;
  } else if (g_supports_flush) {
    if (!ahci_issue_command(ATA_CMD_FLUSH_CACHE, 0, 0, 0, 0, 0))
      return 0;
  }

  return 1;
}
