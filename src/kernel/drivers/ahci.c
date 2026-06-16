#include "ahci.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"

// Forward declaration
void serial_print(const char *str);
void print_hex(uint64_t val);

// AHCI HBA Memory Registers
typedef struct {
  uint32_t cap;
  uint32_t ghc;
  uint32_t is;
  uint32_t pi;
  uint32_t vs;
  uint32_t ccc_ctl;
  uint32_t ccc_pts;
  uint32_t em_loc;
  uint32_t em_ctl;
  uint32_t cap2;
  uint32_t bohc;
  uint8_t res[0x74];
  uint8_t vendor[0x60];
  ahci_port_t ports[32];
} __attribute__((packed)) hba_mem_t;

static hba_mem_t *g_hba = NULL;
static ahci_port_t *g_sata_port = NULL;

void ahci_init(uint64_t abar) {
  serial_print("[AHCI] Initializing at ABAR=");
  print_hex(abar);
  serial_print("\n");

  // Map ABAR (1 page is usually enough for HBA regs)
  VMM_MapPage((void *)abar, (void *)abar, PAGE_WRITE);

  g_hba = (hba_mem_t *)abar;

  // Enable AHCI awareness
  g_hba->ghc |= (1 << 31); // AE (AHCI Enable)

  uint32_t pi = g_hba->pi;
  serial_print("[AHCI] Ports Implemented: ");
  print_hex(pi);
  serial_print("\n");

  for (int i = 0; i < 32; i++) {
    if (pi & (1 << i)) {
      uint32_t ssts =
          *((volatile uint32_t *)&g_hba->ports[i]
                .res1[0]); // SSTS is at ports[i].ssts, roughly offset 0x28.
      // Wait, we need the exact ahci_port_t struct layout.
      // For now, let's just log it.
      serial_print("[AHCI] Port ");
      char c = (char)('0' + i);
      char s[2] = {c, 0};
      serial_print(s);
      serial_print(" active\n");

      // Just grab the first port for simplicity in this minimal driver
      if (!g_sata_port) {
        g_sata_port = &g_hba->ports[i];
        serial_print("[AHCI] Selected as primary drive\n");
      }
    }
  }
}

int ahci_read_sectors(uint32_t lba, uint32_t count, void *buffer) {
  if (!g_sata_port)
    return -1;
  // TODO: implement full command list DMA setup
  return 0;
}

int ahci_write_sectors(uint32_t lba, uint32_t count, const void *buffer) {
  if (!g_sata_port)
    return -1;
  // TODO: implement full command list DMA setup
  return 0;
}
