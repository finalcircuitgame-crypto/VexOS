#include "usb/xhci.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include <stddef.h>

/**
 * Tiny64 xHCI Driver (bring-up + command ring + event ring polling)
 * Spec refs:
 * - xHCI 1.1: 4.2 (init), 4.8/4.9 (rings), 6.4 (TRBs), 7 (commands/events)
 */

#define XHCI_MMIO_MAP_SIZE 0x100000
#define XHCI_CMD_RING_TRBS 256
#define XHCI_EVT_RING_TRBS 256

// ERST entry is 16 bytes (xHCI 6.5)
typedef struct {
  uint64_t segment_base;
  uint32_t segment_size;
  uint32_t rsvd;
} __attribute__((packed)) xhci_erst_entry_t;

static xhci_cap_regs_t *cap_regs;
static xhci_op_regs_t *op_regs;
static xhci_runtime_regs_t *runtime_regs;
static uint32_t *doorbell_regs;

static xhci_trb_t *command_ring;
static uint32_t command_ring_index = 0;
static uint8_t command_ring_cycle = 1;

static xhci_trb_t *event_ring;
static uint32_t event_ring_index = 0;
static uint8_t event_ring_cycle = 1;
static xhci_erst_entry_t *erst;

// DCBAA - Device Context Base Address Array
static uint64_t *dcbaa;

// Forward declarations for printing
void PrintString(const char *str, uint32_t color);
void serial_print(const char *str);

static void print_hex64(uint64_t val) {
  for (int i = 15; i >= 0; i--) {
    char c = (val >> (i * 4)) & 0xF;
    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    char s[2] = {c, 0};
    serial_print(s);
  }
}

static inline uint32_t trb_type(uint32_t control) {
  return (control >> 10) & 0x3F;
}

static inline uint32_t trb_cc(uint32_t status) { return (status >> 24) & 0xFF; }

static inline uint32_t trb_slot_id(uint32_t control) {
  return (control >> 24) & 0xFF;
}

static inline uint32_t make_trb_control(uint32_t type, uint8_t cycle) {
  return (uint32_t)(cycle & 1u) | (type << 10);
}

static void xhci_ring_doorbell_cmd(void) {
  // Doorbell 0, target=0 (Command Ring)
  doorbell_regs[0] = 0;
}

static void xhci_event_ring_update_erdp(void) {
  // ERDP points to the next TRB to be dequeued.
  // Set EHB (bit 3) to clear Event Handler Busy.
  uint64_t next = (uint64_t)(uintptr_t)&event_ring[event_ring_index];
  runtime_regs->interrupters[0].erdp = next | (1ULL << 3);
}

static int xhci_poll_event(xhci_trb_t *out) {
  xhci_trb_t trb = event_ring[event_ring_index];

  // Producer sets cycle bit; consumer tracks expected cycle.
  if ((trb.control & 1u) != (uint32_t)event_ring_cycle) {
    return 0;
  }

  *out = trb;

  // Advance consumer
  event_ring_index++;
  if (event_ring_index >= XHCI_EVT_RING_TRBS) {
    event_ring_index = 0;
    event_ring_cycle ^= 1;
  }

  xhci_event_ring_update_erdp();
  return 1;
}

struct xhci_device_state {
  uint32_t slot_id;
  uint32_t ep0_mps;
  xhci_trb_t *ep0_ring;
  uint32_t ep0_index;
  uint8_t ep0_cycle;

  uint8_t *dev_ctx;

  uint8_t speed_code;

  uint8_t hid_ifnum;
  uint8_t hid_proto;
  uint8_t intr_epaddr;
  uint16_t intr_mps;
  uint8_t intr_interval;

  xhci_trb_t *intr_ring;
  uint32_t intr_index;
  uint8_t intr_cycle;

  uint8_t *intr_buf;
};
typedef struct xhci_device_state xhci_device_state_t;

static void xhci_ring_doorbell_ep0(uint32_t slot_id);
static void xhci_ep0_ring_push(xhci_device_state_t *dev, const xhci_trb_t *trb);
static int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc);
static void xhci_print_u32_dec(uint32_t v);
static void xhci_print_hex32(uint32_t v);

static xhci_trb_t *xhci_alloc_tr_ring(void);

static int xhci_wait_for_command_completion(uint32_t *out_slot_id,
                                            uint32_t *out_cc);
static void xhci_cmd_ring_push(const xhci_trb_t *trb);
static void xhci_ring_doorbell_cmd(void);

static int xhci_ep0_control_in(xhci_device_state_t *dev, uint64_t setup,
                               void *buf, uint32_t len) {
  xhci_trb_t setup_trb;
  setup_trb.data = setup;
  setup_trb.status = 8;
  setup_trb.control = make_trb_control(TRB_TYPE_SETUP_STAGE, dev->ep0_cycle) |
                      (2u << 16) | (1u << 6) | (1u << 5);

  xhci_trb_t data_trb;
  data_trb.data = (uint64_t)(uintptr_t)buf;
  data_trb.status = len;
  data_trb.control = make_trb_control(TRB_TYPE_DATA_STAGE, dev->ep0_cycle) |
                     (1u << 16) | (1u << 5);

  xhci_trb_t status_trb;
  status_trb.data = 0;
  status_trb.status = 0;
  status_trb.control =
      make_trb_control(TRB_TYPE_STATUS_STAGE, dev->ep0_cycle) | (1u << 5);

  xhci_ep0_ring_push(dev, &setup_trb);
  xhci_ep0_ring_push(dev, &data_trb);
  xhci_ep0_ring_push(dev, &status_trb);
  xhci_ring_doorbell_ep0(dev->slot_id);

  uint32_t cc = 0;
  if (!xhci_wait_for_transfer_event(dev->slot_id, &cc)) {
    serial_print("[xHCI] EP0 control IN: timeout\n");
    return 0;
  }
  if (cc != 1) {
    serial_print("[xHCI] EP0 control IN: completion_code=");
    xhci_print_u32_dec(cc);
    serial_print("\n");
    return 0;
  }
  return 1;
}

static int xhci_ep0_control_no_data_out(xhci_device_state_t *dev,
                                        uint64_t setup) {
  xhci_trb_t setup_trb;
  setup_trb.data = setup;
  setup_trb.status = 8;
  setup_trb.control = make_trb_control(TRB_TYPE_SETUP_STAGE, dev->ep0_cycle) |
                      (0u << 16) | (1u << 6) | (1u << 5);

  xhci_trb_t status_trb;
  status_trb.data = 0;
  status_trb.status = 0;
  status_trb.control = make_trb_control(TRB_TYPE_STATUS_STAGE, dev->ep0_cycle) |
                       (1u << 16) | (1u << 5);

  xhci_ep0_ring_push(dev, &setup_trb);
  xhci_ep0_ring_push(dev, &status_trb);
  xhci_ring_doorbell_ep0(dev->slot_id);

  uint32_t cc = 0;
  if (!xhci_wait_for_transfer_event(dev->slot_id, &cc)) {
    serial_print("[xHCI] EP0 control OUT: timeout\n");
    return 0;
  }
  if (cc != 1) {
    serial_print("[xHCI] EP0 control OUT: completion_code=");
    xhci_print_u32_dec(cc);
    serial_print("\n");
    return 0;
  }
  return 1;
}

static int xhci_ep0_get_config_and_set_config(xhci_device_state_t *dev) {
  uint8_t *buf = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;

  uint64_t setup9 = 0;
  setup9 |= 0x80ULL;
  setup9 |= 0x06ULL << 8;
  setup9 |= 0x0200ULL << 16;
  setup9 |= 0x0000ULL << 32;
  setup9 |= 9ULL << 48;

  if (!xhci_ep0_control_in(dev, setup9, buf, 9)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(Configuration 9) failed\n");
    return 0;
  }

  uint16_t total_len = (uint16_t)(buf[2] | (buf[3] << 8));
  if (total_len == 0 || total_len > 4096)
    total_len = 4096;

  uint64_t setupFull = 0;
  setupFull |= 0x80ULL;
  setupFull |= 0x06ULL << 8;
  setupFull |= 0x0200ULL << 16;
  setupFull |= 0x0000ULL << 32;
  setupFull |= ((uint64_t)total_len) << 48;

  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;
  if (!xhci_ep0_control_in(dev, setupFull, buf, total_len)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(Configuration full) failed\n");
    return 0;
  }

  uint8_t cfg_value = buf[5];
  serial_print("[USB] Config total_len=");
  xhci_print_u32_dec(total_len);
  serial_print(" config_value=");
  xhci_print_u32_dec(cfg_value);
  serial_print("\n");

  dev->hid_ifnum = 0xFF;
  dev->hid_proto = 0;
  dev->intr_epaddr = 0;
  dev->intr_mps = 0;
  dev->intr_interval = 0;

  uint8_t cur_ifnum = 0xFF;
  uint8_t cur_ifclass = 0;
  uint8_t cur_ifsub = 0;
  uint8_t cur_ifproto = 0;

  uint32_t off = 0;
  while (off + 2 <= total_len) {
    uint8_t bLength = buf[off + 0];
    uint8_t bType = buf[off + 1];
    if (bLength < 2)
      break;
    if (off + bLength > total_len)
      break;

    if (bType == 4 && bLength >= 9) {
      uint8_t ifnum = buf[off + 2];
      uint8_t alt = buf[off + 3];
      uint8_t nendp = buf[off + 4];
      uint8_t cls = buf[off + 5];
      uint8_t sub = buf[off + 6];
      uint8_t proto = buf[off + 7];

      cur_ifnum = ifnum;
      cur_ifclass = cls;
      cur_ifsub = sub;
      cur_ifproto = proto;

      if (cls == 3 && (sub == 0 || sub == 1)) {
        dev->hid_ifnum = ifnum;
        dev->hid_proto = proto;
      }

      serial_print("[USB] Interface if=");
      xhci_print_u32_dec(ifnum);
      serial_print(" alt=");
      xhci_print_u32_dec(alt);
      serial_print(" ep=");
      xhci_print_u32_dec(nendp);
      serial_print(" class=");
      xhci_print_u32_dec(cls);
      serial_print(" sub=");
      xhci_print_u32_dec(sub);
      serial_print(" proto=");
      xhci_print_u32_dec(proto);
      serial_print("\n");
    } else if (bType == 5 && bLength >= 7) {
      uint8_t epaddr = buf[off + 2];
      uint8_t attr = buf[off + 3];
      uint16_t mps = (uint16_t)(buf[off + 4] | (buf[off + 5] << 8));
      uint8_t interval = buf[off + 6];

      uint8_t xfertype = attr & 0x3u;
      uint8_t dir_in = (epaddr & 0x80u) ? 1u : 0u;
      if (dev->intr_epaddr == 0 && cur_ifclass == 3 && cur_ifsub == 1 &&
          dir_in && xfertype == 3) {
        dev->intr_epaddr = epaddr;
        dev->intr_mps = mps;
        dev->intr_interval = interval;
      }
      serial_print("[USB] Endpoint addr=");
      xhci_print_hex32(epaddr);
      serial_print(" attr=");
      xhci_print_hex32(attr);
      serial_print(" mps=");
      xhci_print_u32_dec(mps);
      serial_print(" interval=");
      xhci_print_u32_dec(interval);
      serial_print("\n");
    }

    off += bLength;
  }

  if (cfg_value == 0)
    cfg_value = 1;
  uint64_t setcfg = 0;
  setcfg |= 0x00ULL;
  setcfg |= 0x09ULL << 8;
  setcfg |= ((uint64_t)cfg_value) << 16;
  setcfg |= 0x0000ULL << 32;
  setcfg |= 0ULL << 48;

  if (!xhci_ep0_control_no_data_out(dev, setcfg)) {
    serial_print("[xHCI] EP0 SET_CONFIGURATION failed\n");
    return 0;
  }

  serial_print("[USB] SET_CONFIGURATION done\n");
  return 1;
}

static int xhci_cmd_configure_intr_in_ep(xhci_device_state_t *dev);
static int xhci_hid_set_protocol_boot(xhci_device_state_t *dev);
static int xhci_hid_set_idle(xhci_device_state_t *dev);
static void xhci_hid_start_polling(xhci_device_state_t *dev);

static int xhci_wait_for_command_completion(uint32_t *out_slot_id,
                                            uint32_t *out_cc) {
  // Busy-wait poll. This is fine for early bring-up.
  for (uint32_t spins = 0; spins < 5000000; spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt))
      continue;

    if (trb_type(evt.control) == TRB_TYPE_COMMAND_COMPLETION) {
      if (out_slot_id)
        *out_slot_id = trb_slot_id(evt.control);
      if (out_cc)
        *out_cc = trb_cc(evt.status);
      return 1;
    }
  }
  return 0;
}

static void xhci_cmd_ring_push(const xhci_trb_t *trb) {
  // Place TRB at current index
  command_ring[command_ring_index] = *trb;

  // Force cycle bit
  command_ring[command_ring_index].control &= ~1u;
  command_ring[command_ring_index].control |=
      (uint32_t)(command_ring_cycle & 1u);

  command_ring_index++;

  // If we are about to step onto the Link TRB, jump back and toggle cycle.
  if (command_ring_index == (XHCI_CMD_RING_TRBS - 1)) {
    // Producer writes to link TRB (already initialized), then toggles cycle
    // state.
    command_ring_index = 0;
    command_ring_cycle ^= 1;
  }
}

// Context structures (xHCI 6.2). Layout here is for CSZ=0 (32-byte contexts).
// For QEMU's xHCI this is typically fine; we'll also compute context size from
// HCCPARAMS1.

typedef struct {
  uint32_t drop_flags;
  uint32_t add_flags;
  uint32_t rsvd[5];
  uint32_t config;
} __attribute__((packed)) xhci_input_control_ctx_t;

typedef struct {
  uint32_t dword0;
  uint32_t dword1;
  uint32_t dword2;
  uint32_t dword3;
  uint32_t rsvd[4];
} __attribute__((packed)) xhci_slot_ctx_32_t;

typedef struct {
  uint32_t dword0;
  uint32_t dword1;
  uint32_t tr_deq_lo;
  uint32_t tr_deq_hi;
  uint32_t dword4;
  uint32_t rsvd[3];
} __attribute__((packed)) xhci_ep_ctx_32_t;

static uint32_t g_ctx_size = 32;

static void xhci_intr_ring_push(xhci_device_state_t *dev,
                                const xhci_trb_t *trb) {
  dev->intr_ring[dev->intr_index] = *trb;
  dev->intr_ring[dev->intr_index].control &= ~1u;
  dev->intr_ring[dev->intr_index].control |= (uint32_t)(dev->intr_cycle & 1u);

  dev->intr_index++;
  if (dev->intr_index == 255) {
    dev->intr_index = 0;
    dev->intr_cycle ^= 1;
  }
}

static void xhci_ring_doorbell_ep(uint32_t slot_id, uint32_t dci) {
  doorbell_regs[slot_id] = dci;
}

static int xhci_cmd_configure_intr_in_ep(xhci_device_state_t *dev) {
  if (dev->intr_epaddr == 0 || dev->intr_mps == 0) {
    serial_print(
        "[xHCI] No interrupt IN endpoint found; skipping HID polling\n");
    return 0;
  }

  // For EP1 IN, DCI = 3 (xHCI spec). Input context index = DCI+1 = 4.
  const uint32_t dci = 3;
  const uint32_t ctx_index = dci + 1;

  dev->intr_ring = xhci_alloc_tr_ring();
  dev->intr_index = 0;
  dev->intr_cycle = 1;

  uint8_t *input_ctx = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    input_ctx[i] = 0;

  xhci_input_control_ctx_t *icc = (xhci_input_control_ctx_t *)input_ctx;
  // Must include Slot Context when increasing Context Entries (xHCI spec
  // requirement).
  icc->add_flags = (1u << 0) | (1u << 3);
  icc->drop_flags = 0;

  // Slot Context at index 1: bump Context Entries to include DCI=3 (EP1 IN)
  // Context Entries field is bits 31:27 of Dword0.
  xhci_slot_ctx_32_t *slot = (xhci_slot_ctx_32_t *)(input_ctx + g_ctx_size);
  if (dev->dev_ctx) {
    // Device Context layout: Slot Context is index 0.
    xhci_slot_ctx_32_t *cur =
        (xhci_slot_ctx_32_t *)(dev->dev_ctx + (0 * g_ctx_size));
    *slot = *cur;
  } else {
    slot->dword0 = 0;
    slot->dword1 = 0;
    slot->dword2 = 0;
    slot->dword3 = 0;
    for (int i = 0; i < 4; i++)
      slot->rsvd[i] = 0;
  }
  slot->dword0 &= ~(0x1Fu << 27);
  slot->dword0 |= (3u & 0x1Fu) << 27;

  xhci_ep_ctx_32_t *ep =
      (xhci_ep_ctx_32_t *)(input_ctx + (ctx_index * g_ctx_size));

  // EP Type Interrupt IN = 7, CErr should be 3 for interrupt endpoints.
  // dword1: [31:16]=Max Packet Size, [15:8]=Max Burst, [7:3]=EP Type,
  // [2:0]=CErr
  ep->dword1 = ((uint32_t)(dev->intr_mps & 0xFFFFu) << 16) | (0u << 8) |
               ((7u & 0x7u) << 3) | 3u;

  uint64_t trdp = (uint64_t)(uintptr_t)dev->intr_ring;
  trdp &= ~0xFULL;
  trdp |= 1u;
  ep->tr_deq_lo = (uint32_t)(trdp & 0xFFFFFFFFu);
  ep->tr_deq_hi = (uint32_t)((trdp >> 32) & 0xFFFFFFFFu);

  // xHCI Interval encoding depends on speed. For HS/SS interrupt endpoints,
  // bInterval is already an exponent (1-16). For FS/LS it is in frames.
  // For bring-up: force a frequent interval so we don't wait a long time.
  // (We'll make this spec-correct once we are seeing reports.)
  uint32_t interval = dev->intr_interval; // from endpoint descriptor
  ep->dword0 = (interval & 0xFFu) << 16;
  // dword4: [15:0]=Avg TRB Length, [31:16]=Max ESIT Payload.
  // Set Max ESIT Payload to MPS (single packet) for now.
  ep->dword4 = (uint32_t)8u | ((uint32_t)(dev->intr_mps & 0xFFFFu) << 16);

  xhci_trb_t cmd;
  cmd.data = (uint64_t)(uintptr_t)input_ctx;
  cmd.status = 0;
  cmd.control =
      make_trb_control(TRB_TYPE_CONFIGURE_EP_CMD, command_ring_cycle) |
      (dev->slot_id << 24);

  xhci_cmd_ring_push(&cmd);
  xhci_ring_doorbell_cmd();

  uint32_t evt_slot = 0;
  uint32_t cc = 0;
  if (!xhci_wait_for_command_completion(&evt_slot, &cc)) {
    serial_print("[xHCI] ConfigureEP: timeout\n");
    return 0;
  }

  serial_print("[xHCI] ConfigureEP: completion_code=");
  xhci_print_u32_dec(cc);
  serial_print(" slot_id=");
  xhci_print_u32_dec(evt_slot);
  serial_print("\n");

  if (cc == 1 && dev->dev_ctx) {
    // Device Context layout: endpoint context index equals DCI. EP1 IN =>
    // DCI=3.
    xhci_ep_ctx_32_t *cur_ep =
        (xhci_ep_ctx_32_t *)(dev->dev_ctx + (3 * g_ctx_size));
    serial_print("[xHCI] EP1IN ctx d0=");
    xhci_print_hex32(cur_ep->dword0);
    serial_print(" d1=");
    xhci_print_hex32(cur_ep->dword1);
    serial_print(" trdp_lo=");
    xhci_print_hex32(cur_ep->tr_deq_lo);
    serial_print("\n");
  }

  return (cc == 1);
}

static int xhci_hid_set_protocol_boot(xhci_device_state_t *dev) {
  if (dev->hid_ifnum == 0xFF)
    return 0;
  uint64_t setup = 0;
  setup |= 0x21ULL;
  setup |= 0x0BULL << 8;
  setup |= 0x0000ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= 0ULL << 48;
  return xhci_ep0_control_no_data_out(dev, setup);
}

static int xhci_hid_set_idle(xhci_device_state_t *dev) {
  if (dev->hid_ifnum == 0xFF)
    return 0;
  uint64_t setup = 0;
  setup |= 0x21ULL;
  setup |= 0x0AULL << 8;
  setup |= 0x0000ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= 0ULL << 48;
  return xhci_ep0_control_no_data_out(dev, setup);
}

static void xhci_hid_start_polling(xhci_device_state_t *dev) {
  if (!dev->intr_ring)
    return;

  if (!dev->intr_buf) {
    dev->intr_buf = (uint8_t *)PMM_AllocatePage();
    for (uint32_t i = 0; i < 4096; i++)
      dev->intr_buf[i] = 0;
  }

  const uint32_t dci = 3;
  xhci_trb_t trb;
  trb.data = (uint64_t)(uintptr_t)dev->intr_buf;
  trb.status = (uint32_t)(dev->intr_mps & 0xFFFFu);
  trb.control = make_trb_control(TRB_TYPE_NORMAL, dev->intr_cycle) | (1u << 5) |
                (1u << 2);
  xhci_intr_ring_push(dev, &trb);
  xhci_ring_doorbell_ep(dev->slot_id, dci);
}

static void xhci_print_u32_dec(uint32_t v) {
  char s[12];
  int n = 0;
  char buf[12];
  int i = 0;
  if (v == 0)
    buf[i++] = '0';
  while (v > 0) {
    buf[i++] = (v % 10) + '0';
    v /= 10;
  }
  while (i > 0)
    s[n++] = buf[--i];
  s[n] = 0;
  serial_print(s);
}

static void xhci_print_hex32(uint32_t v);

// PortSC bits
static const uint32_t PORTSC_CCS = 1u << 0; // Current Connect Status
static const uint32_t PORTSC_PED = 1u << 1; // Port Enabled/Disabled
static const uint32_t PORTSC_PR = 1u << 4;  // Port Reset
static const uint32_t PORTSC_PLS_MASK = 0xFu << 5;
static const uint32_t PORTSC_PP = 1u << 9; // Port Power
static const uint32_t PORTSC_SPEED_MASK = 0xFu << 10;
static const uint32_t PORTSC_LWS = 1u << 16; // Port Link State Write Strobe
// RW1C bits (write 1 to clear).
static const uint32_t PORTSC_W1C = (1u << 17) | (1u << 18) | (1u << 19) |
                                  (1u << 20) | (1u << 21) | (1u << 22);
static const uint32_t PORTSC_WPR = 1u << 31; // Warm Port Reset (USB3)

static uint32_t xhci_port_speed(uint32_t portsc) {
  return (portsc & PORTSC_SPEED_MASK) >> 10;
}

static uint32_t xhci_port_pls(uint32_t portsc) { return (portsc >> 5) & 0xFu; }

static void xhci_log_portsc(uint32_t port_id, const char *prefix,
                            uint32_t portsc) {
  serial_print(prefix);
  serial_print(" port=");
  xhci_print_u32_dec(port_id);
  serial_print(" PortSC=");
  xhci_print_hex32(portsc);
  serial_print(" PLS=");
  xhci_print_u32_dec(xhci_port_pls(portsc));
  serial_print(" speed=");
  xhci_print_u32_dec(xhci_port_speed(portsc));
  serial_print(" PED=");
  xhci_print_u32_dec((portsc >> 1) & 1u);
  serial_print("\n");
}

static int xhci_port_ready(uint32_t ps) {
  return (ps & PORTSC_CCS) && (ps & PORTSC_PED) &&
         (xhci_port_speed(ps) != 0) && (xhci_port_pls(ps) == 0);
}

static void xhci_port_power_on(volatile uint32_t *portsc, uint32_t *ps) {
  if (!(*ps & PORTSC_PP)) {
    uint32_t v = *ps;
    v |= PORTSC_PP;
    v &= ~PORTSC_W1C;
    *portsc = v;
    *ps = *portsc;
  }
}

static void xhci_port_clear_w1c(volatile uint32_t *portsc, uint32_t *ps) {
  if (*ps & PORTSC_W1C) {
    *portsc = *ps | PORTSC_W1C;
    *ps = *portsc;
  }
}

static int xhci_port_wait_ready(volatile uint32_t *portsc, uint32_t spins,
                                uint32_t *out_ps) {
  for (uint32_t i = 0; i < spins; i++) {
    uint32_t ps = *portsc;
    if (xhci_port_ready(ps)) {
      if (out_ps)
        *out_ps = ps;
      return 1;
    }
  }
  if (out_ps)
    *out_ps = *portsc;
  return 0;
}

static void xhci_port_force_u0(volatile uint32_t *portsc) {
  uint32_t ps = *portsc;
  if (!(ps & PORTSC_CCS))
    return;
  if (xhci_port_pls(ps) == 0)
    return;

  uint32_t v = ps;
  v &= ~PORTSC_PLS_MASK;
  v |= PORTSC_LWS;
  v &= ~PORTSC_W1C;
  *portsc = v;
}

static void xhci_port_warm_reset(volatile uint32_t *portsc) {
  uint32_t ps = *portsc;
  uint32_t v = ps;
  v &= ~PORTSC_W1C;
  v |= PORTSC_WPR;
  *portsc = v;

  for (uint32_t spins = 0; spins < 20000000; spins++) {
    ps = *portsc;
    if (!(ps & PORTSC_WPR))
      break;
  }
}

static void xhci_port_cold_reset(volatile uint32_t *portsc) {
  uint32_t ps = *portsc;
  uint32_t v = ps;
  v &= ~PORTSC_W1C;
  v |= PORTSC_PR;
  *portsc = v;

  for (uint32_t spins = 0; spins < 20000000; spins++) {
    ps = *portsc;
    if (!(ps & PORTSC_PR))
      break;
  }
}

static int xhci_force_port_ready(volatile uint32_t *portsc, uint32_t port_id,
                                 uint32_t *out_speed) {
  uint32_t ps = *portsc;
  xhci_port_power_on(portsc, &ps);
  xhci_port_clear_w1c(portsc, &ps);

  if (xhci_port_ready(ps)) {
    if (out_speed)
      *out_speed = xhci_port_speed(ps);
    return 1;
  }

  for (uint32_t attempt = 0; attempt < 4; attempt++) {
    if (!(ps & PORTSC_CCS))
      break;

    uint32_t speed_hint = xhci_port_speed(ps);
    int prefer_warm = (speed_hint >= 4);

    for (uint32_t pass = 0; pass < 2; pass++) {
      int do_warm = (pass == 0) ? prefer_warm : !prefer_warm;
      if (attempt & 1)
        do_warm = !do_warm;

      if (do_warm) {
        serial_print("[xHCI] Forcing warm reset\n");
        xhci_port_warm_reset(portsc);
        ps = *portsc;
        xhci_port_clear_w1c(portsc, &ps);
        xhci_log_portsc(port_id, "[xHCI] After warm reset", ps);
      } else {
        serial_print("[xHCI] Forcing cold reset\n");
        xhci_port_cold_reset(portsc);
        ps = *portsc;
        xhci_port_clear_w1c(portsc, &ps);
        xhci_log_portsc(port_id, "[xHCI] After cold reset", ps);
      }

      if (xhci_port_wait_ready(portsc, 20000000, &ps)) {
        if (out_speed)
          *out_speed = xhci_port_speed(ps);
        return 1;
      }

      xhci_port_force_u0(portsc);
      if (xhci_port_wait_ready(portsc, 20000000, &ps)) {
        if (out_speed)
          *out_speed = xhci_port_speed(ps);
        return 1;
      }

      ps = *portsc;
    }
  }

  if (out_speed)
    *out_speed = xhci_port_speed(ps);
  return 0;
}

static void xhci_bios_handoff(void) {
  // Walk xHCI extended capabilities to find USB Legacy Support (Cap ID = 1)
  // HCCPARAMS1 bits 31:16 give xECP (offset in 4-byte units).
  uint32_t hcc1 = cap_regs->hcc_params1;
  uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
  if (xecp == 0)
    return;

  uint32_t off = xecp * 4;
  while (off) {
    volatile uint32_t *ext = (volatile uint32_t *)((uint64_t)cap_regs + off);
    uint32_t cap = ext[0];
    uint8_t cap_id = (uint8_t)(cap & 0xFF);
    uint8_t next = (uint8_t)((cap >> 8) & 0xFF);

    if (cap_id == 1) {
      // USB Legacy Support
      // bit16: BIOS Owned Semaphore, bit24: OS Owned Semaphore
      serial_print("[xHCI] USBLEGSUP found; requesting OS ownership\n");
      uint32_t v = ext[0];
      v |= (1u << 24);
      ext[0] = v;

      // Wait for BIOS to release ownership
      for (uint32_t spins = 0; spins < 50000000; spins++) {
        uint32_t r = ext[0];
        if ((r & (1u << 16)) == 0) {
          serial_print("[xHCI] BIOS ownership released\n");
          return;
        }
      }
      serial_print("[xHCI] BIOS ownership still set (continuing anyway)\n");
      return;
    }

    if (next == 0)
      break;
    off += (uint32_t)next * 4;
  }
}

static void xhci_print_hex32(uint32_t v) {
  static const char *hex = "0123456789ABCDEF";
  char s[9];
  s[0] = hex[(v >> 28) & 0xF];
  s[1] = hex[(v >> 24) & 0xF];
  s[2] = hex[(v >> 20) & 0xF];
  s[3] = hex[(v >> 16) & 0xF];
  s[4] = hex[(v >> 12) & 0xF];
  s[5] = hex[(v >> 8) & 0xF];
  s[6] = hex[(v >> 4) & 0xF];
  s[7] = hex[(v >> 0) & 0xF];
  s[8] = 0;
  serial_print(s);
}

static int xhci_cmd_enable_slot(uint32_t *out_slot_id) {
  xhci_trb_t cmd;
  cmd.data = 0;
  cmd.status = 0;
  cmd.control = make_trb_control(TRB_TYPE_ENABLE_SLOT_CMD, command_ring_cycle);

  xhci_cmd_ring_push(&cmd);
  xhci_ring_doorbell_cmd();

  uint32_t slot = 0;
  uint32_t cc = 0;
  if (!xhci_wait_for_command_completion(&slot, &cc)) {
    serial_print("[xHCI] EnableSlot: timeout waiting for Command Completion\n");
    return 0;
  }

  serial_print("[xHCI] EnableSlot: completion_code=");
  xhci_print_u32_dec(cc);
  serial_print(" slot_id=");
  xhci_print_u32_dec(slot);
  serial_print("\n");

  if (out_slot_id)
    *out_slot_id = slot;
  return (cc == 1 && slot != 0);
}

static xhci_trb_t *xhci_alloc_tr_ring(void) {
  xhci_trb_t *ring = (xhci_trb_t *)PMM_AllocatePage();
  for (int i = 0; i < 256; i++) {
    ring[i].data = 0;
    ring[i].status = 0;
    ring[i].control = 0;
  }
  // Link TRB at end
  ring[255].data = (uint64_t)(uintptr_t)ring;
  ring[255].status = 0;
  ring[255].control = make_trb_control(TRB_TYPE_LINK, 1) | (1u << 1);
  return ring;
}

static xhci_device_state_t g_devs[256];

static void xhci_ring_doorbell_ep0(uint32_t slot_id) {
  // Doorbell target is DCI. For EP0, DCI=1.
  doorbell_regs[slot_id] = 1;
}

static void xhci_ep0_ring_push(xhci_device_state_t *dev,
                               const xhci_trb_t *trb) {
  dev->ep0_ring[dev->ep0_index] = *trb;
  dev->ep0_ring[dev->ep0_index].control &= ~1u;
  dev->ep0_ring[dev->ep0_index].control |= (uint32_t)(dev->ep0_cycle & 1u);

  dev->ep0_index++;
  if (dev->ep0_index == 255) {
    dev->ep0_index = 0;
    dev->ep0_cycle ^= 1;
  }
}

static int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc) {
  uint32_t seen_other = 0;
  for (uint32_t spins = 0; spins < 50000000; spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt))
      continue;

    if (trb_type(evt.control) == TRB_TYPE_TRANSFER_EVENT) {
      uint32_t cc = trb_cc(evt.status);
      uint32_t eslot = trb_slot_id(evt.control);
      if (eslot == slot_id) {
        if (out_cc)
          *out_cc = cc;
        return 1;
      }

      // Rate-limited debug: if we're getting transfer events but not for our
      // slot, print a few so we know the ring is alive.
      if (seen_other < 4) {
        serial_print("[xHCI] TransferEvent other slot=");
        xhci_print_u32_dec(eslot);
        serial_print(" cc=");
        xhci_print_u32_dec(cc);
        serial_print("\n");
        seen_other++;
      }
    }
  }
  return 0;
}

static void xhci_print_hex16(uint16_t v) {
  static const char *hex = "0123456789ABCDEF";
  char s[5];
  s[0] = hex[(v >> 12) & 0xF];
  s[1] = hex[(v >> 8) & 0xF];
  s[2] = hex[(v >> 4) & 0xF];
  s[3] = hex[(v >> 0) & 0xF];
  s[4] = 0;
  serial_print(s);
}

static int xhci_ep0_get_device_descriptor(xhci_device_state_t *dev) {
  // USB Device Descriptor is 18 bytes
  uint8_t *buf = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;

  // Setup packet (USB2.0 9.3): GET_DESCRIPTOR(Device)
  // bmRequestType=0x80 (Device-to-host, Standard, Device)
  // bRequest=6, wValue=(1<<8)|0, wIndex=0, wLength=18
  uint64_t setup = 0;
  setup |= 0x80ULL;         // bmRequestType
  setup |= 0x06ULL << 8;    // bRequest
  setup |= 0x0100ULL << 16; // wValue
  setup |= 0x0000ULL << 32; // wIndex
  setup |= 18ULL << 48;     // wLength

  // Setup Stage TRB
  xhci_trb_t setup_trb;
  setup_trb.data = setup;
  // Transfer length=8, interrupter target=0
  setup_trb.status = 8;
  // TRT=2 (IN data stage) in bits 17:16
  // IDT=1 (bit 6) because setup packet is in the TRB parameter field
  setup_trb.control = make_trb_control(TRB_TYPE_SETUP_STAGE, dev->ep0_cycle) |
                      (2u << 16) | (1u << 6) | (1u << 5);

  // Data Stage TRB (IN)
  xhci_trb_t data_trb;
  data_trb.data = (uint64_t)(uintptr_t)buf;
  data_trb.status = 18; // transfer length
  // DIR=1 in bit 16
  data_trb.control = make_trb_control(TRB_TYPE_DATA_STAGE, dev->ep0_cycle) |
                     (1u << 16) | (1u << 5);

  // Status Stage TRB (OUT for IN data stage)
  xhci_trb_t status_trb;
  status_trb.data = 0;
  status_trb.status = 0;
  // DIR=0
  status_trb.control =
      make_trb_control(TRB_TYPE_STATUS_STAGE, dev->ep0_cycle) | (1u << 5);

  xhci_ep0_ring_push(dev, &setup_trb);
  xhci_ep0_ring_push(dev, &data_trb);
  xhci_ep0_ring_push(dev, &status_trb);

  xhci_ring_doorbell_ep0(dev->slot_id);

  uint32_t cc = 0;
  if (!xhci_wait_for_transfer_event(dev->slot_id, &cc)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR: timeout\n");
    return 0;
  }

  serial_print("[xHCI] EP0 GET_DESCRIPTOR: completion_code=");
  xhci_print_u32_dec(cc);
  serial_print("\n");
  if (cc != 1)
    return 0;

  // Parse: idVendor at 8, idProduct at 10, bDeviceClass at 4
  uint16_t vid = (uint16_t)(buf[8] | (buf[9] << 8));
  uint16_t pid = (uint16_t)(buf[10] | (buf[11] << 8));
  uint8_t cls = buf[4];
  uint8_t sub = buf[5];
  uint8_t proto = buf[6];

  serial_print("[USB] Device Descriptor VID:PID=");
  xhci_print_hex16(vid);
  serial_print(":");
  xhci_print_hex16(pid);
  serial_print(" class=");
  xhci_print_u32_dec(cls);
  serial_print(" sub=");
  xhci_print_u32_dec(sub);
  serial_print(" proto=");
  xhci_print_u32_dec(proto);
  serial_print("\n");

  return 1;
}

static int xhci_cmd_address_device(uint32_t slot_id, uint32_t port_id,
                                   uint32_t speed_code) {
  // Allocate Device Context and EP0 transfer ring
  uint8_t *dev_ctx = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    dev_ctx[i] = 0;

  xhci_trb_t *ep0_ring = xhci_alloc_tr_ring();

  // Write DCBAA entry for this slot
  dcbaa[slot_id] = (uint64_t)(uintptr_t)dev_ctx;

  // Allocate Input Context (must hold Input Control + Slot + EP0)
  uint8_t *input_ctx = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    input_ctx[i] = 0;

  // Input Control Context at offset 0
  xhci_input_control_ctx_t *icc = (xhci_input_control_ctx_t *)input_ctx;
  // add slot ctx (bit0) + ep0 ctx (bit1)
  icc->add_flags = (1u << 0) | (1u << 1);
  icc->drop_flags = 0;

  // Slot Context at index 1
  xhci_slot_ctx_32_t *slot = (xhci_slot_ctx_32_t *)(input_ctx + g_ctx_size);
  slot->dword0 = ((speed_code & 0xFu) << 20) | ((1u & 0x1Fu) << 27);
  slot->dword1 = ((port_id & 0xFFu) << 16);
  slot->dword2 = 0;
  slot->dword3 = 0;

  // EP0 Context at index 2
  xhci_ep_ctx_32_t *ep0 = (xhci_ep_ctx_32_t *)(input_ctx + (2 * g_ctx_size));

  uint32_t mps = 64;
  if (speed_code == 1)
    mps = 8; // full-speed
  if (speed_code == 2)
    mps = 8; // low-speed
  if (speed_code == 4)
    mps = 512; // superspeed

  ep0->dword1 = ((mps & 0xFFFFu) << 16) | ((4u & 0x7u) << 3);

  uint64_t trdp = (uint64_t)(uintptr_t)ep0_ring;
  trdp &= ~0xFULL;
  trdp |= 1u; // DCS = 1
  ep0->tr_deq_lo = (uint32_t)(trdp & 0xFFFFFFFFu);
  ep0->tr_deq_hi = (uint32_t)((trdp >> 32) & 0xFFFFFFFFu);

  ep0->dword0 = (3u << 1);
  ep0->dword4 = 8u;

  // Address Device Command TRB
  xhci_trb_t cmd;
  cmd.data = (uint64_t)(uintptr_t)input_ctx;
  cmd.status = 0;
  cmd.control =
      make_trb_control(TRB_TYPE_ADDRESS_DEVICE_CMD, command_ring_cycle) |
      (slot_id << 24);

  xhci_cmd_ring_push(&cmd);
  xhci_ring_doorbell_cmd();

  uint32_t evt_slot = 0;
  uint32_t cc = 0;
  if (!xhci_wait_for_command_completion(&evt_slot, &cc)) {
    serial_print("[xHCI] AddressDevice: timeout\n");
    return 0;
  }

  serial_print("[xHCI] AddressDevice: completion_code=");
  xhci_print_u32_dec(cc);
  serial_print(" slot_id=");
  xhci_print_u32_dec(evt_slot);
  serial_print("\n");

  if (cc != 1)
    return 0;

  // Save device state for EP0 transfers (per-slot)
  xhci_device_state_t *dev = &g_devs[slot_id];
  dev->slot_id = slot_id;
  dev->ep0_mps = mps;
  dev->ep0_ring = ep0_ring;
  dev->ep0_index = 0;
  dev->ep0_cycle = 1;
  dev->dev_ctx = dev_ctx;
  dev->speed_code = (uint8_t)(speed_code & 0xFFu);
  dev->intr_ring = NULL;
  dev->intr_index = 0;
  dev->intr_cycle = 1;
  dev->hid_ifnum = 0xFF;
  dev->hid_proto = 0;
  dev->intr_epaddr = 0;
  dev->intr_mps = 0;
  dev->intr_interval = 0;

  // Next milestone: read the USB Device Descriptor via EP0 control transfer
  if (!xhci_ep0_get_device_descriptor(dev)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR failed\n");
  }

  if (!xhci_ep0_get_config_and_set_config(dev)) {
    serial_print("[xHCI] Config descriptor / set config failed\n");
  }

  // Basic HID bring-up: boot protocol + idle, then enable interrupt IN EP and
  // poll reports.
  if (dev->hid_ifnum != 0xFF) {
    serial_print("[HID] SET_PROTOCOL(boot)\n");
    (void)xhci_hid_set_protocol_boot(dev);
    serial_print("[HID] SET_IDLE\n");
    (void)xhci_hid_set_idle(dev);
    if (xhci_cmd_configure_intr_in_ep(dev)) {
      serial_print("[HID] Interrupt IN armed\n");
      xhci_hid_start_polling(dev);
    }
  }

  return 1;
}

/**
 * See xHCI Spec Section 4.2: Host Controller Initialization
 */
void xhci_init(uint64_t mmio_phys) {
  serial_print("[xHCI] Initializing Controller at ");
  print_hex64(mmio_phys);
  serial_print("\n");

  // 1) Map MMIO space
  serial_print("[xHCI] Mapping MMIO...\n");
  for (uint64_t i = 0; i < XHCI_MMIO_MAP_SIZE; i += 4096) {
    VMM_MapPage((void *)(mmio_phys + i), (void *)(mmio_phys + i),
                PAGE_WRITE | PAGE_PRESENT);
  }

  cap_regs = (xhci_cap_regs_t *)mmio_phys;
  op_regs = (xhci_op_regs_t *)(mmio_phys + cap_regs->cap_length);

  uint32_t dboff = cap_regs->dboff & ~0x3u;
  uint32_t rtsoff = cap_regs->rtsoff & ~0x1Fu;

  doorbell_regs = (uint32_t *)(mmio_phys + dboff);
  runtime_regs = (xhci_runtime_regs_t *)(mmio_phys + rtsoff);

  serial_print("[xHCI] cap_length: ");
  {
    char s[4];
    s[0] = (cap_regs->cap_length / 10) + '0';
    s[1] = (cap_regs->cap_length % 10) + '0';
    s[2] = 0;
    serial_print(s);
  }
  serial_print("\n");

  // Real hardware often requires BIOS->OS ownership handoff.
  xhci_bios_handoff();

  // Context size (HCCPARAMS1.CSZ): 0=32B contexts, 1=64B contexts
  g_ctx_size = (cap_regs->hcc_params1 & (1u << 2)) ? 64u : 32u;
  serial_print("[xHCI] Context size: ");
  xhci_print_u32_dec(g_ctx_size);
  serial_print("\n");

  // 2) Reset controller
  serial_print("[xHCI] Resetting Controller...\n");
  op_regs->usb_cmd &= ~USB_CMD_RS;
  serial_print("[xHCI] Waiting for halt...\n");
  while (!(op_regs->usb_sts & USB_STS_HCH))
    ;

  serial_print("[xHCI] Issuing Reset...\n");
  op_regs->usb_cmd |= USB_CMD_HCRST;
  while (op_regs->usb_cmd & USB_CMD_HCRST)
    ;
  serial_print("[xHCI] Waiting for CNR...\n");
  while (op_regs->usb_sts & (1 << 11))
    ;

  // 3) Configure MaxSlots
  uint32_t max_slots = cap_regs->hcs_params1 & 0xFF;
  op_regs->config = max_slots;
  serial_print("[xHCI] Configured Max Slots: ");
  {
    char s[4];
    s[0] = (max_slots / 10) + '0';
    s[1] = (max_slots % 10) + '0';
    s[2] = 0;
    serial_print(s);
  }
  serial_print("\n");

  // 4) DCBAA + Scratchpad Buffers (xHCI 4.2 + 6.1)
  dcbaa = (uint64_t *)PMM_AllocatePage();
  for (int i = 0; i < 512; i++)
    dcbaa[i] = 0;

  // Max Scratchpad Buffers = (MaxScratchpadBuffersHi << 5) |
  // MaxScratchpadBuffersLo
  uint32_t hcs2 = cap_regs->hcs_params2;
  uint32_t max_sp_lo = (hcs2 >> 27) & 0x1F;
  uint32_t max_sp_hi = (hcs2 >> 21) & 0x1F;
  uint32_t scratchpad_count = (max_sp_hi << 5) | max_sp_lo;

  if (scratchpad_count > 0) {
    serial_print("[xHCI] Scratchpads: ");
    {
      char s[12];
      int n = 0;
      uint32_t tmp = scratchpad_count;
      char buf[12];
      int i = 0;
      if (tmp == 0)
        buf[i++] = '0';
      while (tmp > 0) {
        buf[i++] = (tmp % 10) + '0';
        tmp /= 10;
      }
      while (i > 0)
        s[n++] = buf[--i];
      s[n] = 0;
      serial_print(s);
    }
    serial_print("\n");

    // Scratchpad Buffer Array is an array of 64-bit pointers, one per
    // scratchpad buffer. Typically fits in one page.
    uint64_t *sp_array = (uint64_t *)PMM_AllocatePage();
    for (uint32_t i = 0; i < 512; i++)
      sp_array[i] = 0;

    for (uint32_t i = 0; i < scratchpad_count; i++) {
      void *sp_buf = PMM_AllocatePage();
      sp_array[i] = (uint64_t)(uintptr_t)sp_buf;
    }

    dcbaa[0] = (uint64_t)(uintptr_t)sp_array;
  }

  op_regs->dcbaap = (uint64_t)(uintptr_t)dcbaa;

  // 5) Command Ring: 256 TRBs with last TRB as Link TRB
  command_ring = (xhci_trb_t *)PMM_AllocatePage();
  for (int i = 0; i < XHCI_CMD_RING_TRBS; i++) {
    command_ring[i].data = 0;
    command_ring[i].status = 0;
    command_ring[i].control = 0;
  }

  // Link TRB at end points back to ring start, toggles cycle
  command_ring[XHCI_CMD_RING_TRBS - 1].data = (uint64_t)(uintptr_t)command_ring;
  command_ring[XHCI_CMD_RING_TRBS - 1].status = 0;
  // cycle=1 (initial), type=LINK, TC=1 (bit1)
  command_ring[XHCI_CMD_RING_TRBS - 1].control =
      make_trb_control(TRB_TYPE_LINK, 1) | (1u << 1);

  command_ring_index = 0;
  command_ring_cycle = 1;
  // CRCR: ring base (aligned) + RCS
  op_regs->crcr = ((uint64_t)(uintptr_t)command_ring) | 1u;

  // 6) Event Ring + ERST (single segment)
  event_ring = (xhci_trb_t *)PMM_AllocatePage();
  for (int i = 0; i < XHCI_EVT_RING_TRBS; i++) {
    event_ring[i].data = 0;
    event_ring[i].status = 0;
    event_ring[i].control = 0;
  }

  erst = (xhci_erst_entry_t *)PMM_AllocatePage();
  erst[0].segment_base = (uint64_t)(uintptr_t)event_ring;
  erst[0].segment_size = XHCI_EVT_RING_TRBS;
  erst[0].rsvd = 0;

  event_ring_index = 0;
  event_ring_cycle = 1;

  runtime_regs->interrupters[0].erstsz = 1;
  runtime_regs->interrupters[0].erstba = (uint64_t)(uintptr_t)erst;
  runtime_regs->interrupters[0].erdp = (uint64_t)(uintptr_t)&event_ring[0];
  // Enable interrupter even though we poll; some implementations may not
  // generate events otherwise. IMAN: bit0=IP (RW1C), bit1=IE
  runtime_regs->interrupters[0].iman = (1u << 1) | (1u << 0);

  // 7) Run controller (do NOT enable INTE yet; polling only)
  op_regs->usb_cmd |= USB_CMD_RS | USB_CMD_INTE;
  serial_print("[xHCI] Controller Running\n");

  // Prime ERDP
  xhci_event_ring_update_erdp();

  // 8) Ports info
  uint32_t num_ports = (cap_regs->hcs_params1 >> 24) & 0xFF;
  serial_print("[xHCI] Number of ports: ");
  {
    char s[4];
    s[0] = (num_ports / 10) + '0';
    s[1] = (num_ports % 10) + '0';
    s[2] = 0;
    serial_print(s);
  }
  serial_print("\n");

  // PortSC bits
  const uint32_t PORTSC_CCS = 1u << 0; // Current Connect Status
  const uint32_t PORTSC_PED = 1u << 1; // Port Enabled/Disabled
  const uint32_t PORTSC_PR = 1u << 4;  // Port Reset
  const uint32_t PORTSC_PP = 1u << 9;  // Port Power
  const uint32_t PORTSC_SPEED_MASK = 0xFu << 10;
  const uint32_t PORTSC_WPR = 1u << 31; // Warm Port Reset (USB3)

  const uint32_t PORTSC_PLS_MASK = 0xFu << 5;

  // RW1C bits (write 1 to clear).
  const uint32_t PORTSC_W1C = (1u << 17) | (1u << 18) | (1u << 19) |
                              (1u << 20) | (1u << 21) | (1u << 22);

  serial_print("[xHCI] PORT SCAN BEGIN\n");

  // Power on ALL ports and wait for them to stabilize.
  // We do NOT break early, to ensure USB2 ports get initialized too.
  serial_print("[xHCI] Powering on all ports...\n");
  for (uint32_t i = 0; i < num_ports; i++) {
    volatile uint32_t *portsc =
        (uint32_t *)((uint64_t)op_regs + 0x400 + (i * 0x10));
    uint32_t ps = *portsc;

    if (!(ps & PORTSC_PP)) {
      *portsc = (ps | PORTSC_PP) & ~PORTSC_W1C;
    }
  }

  // Always wait a bit for lines to stabilize (especially USB 2.0)
  serial_print("[xHCI] Waiting for ports to stabilize...\n");
  for (uint32_t tries = 0; tries < 20; tries++) {
    for (volatile uint32_t d = 0; d < 2000000; d++) { }

    for (uint32_t i = 0; i < num_ports; i++) {
      volatile uint32_t *portsc =
          (uint32_t *)((uint64_t)op_regs + 0x400 + (i * 0x10));
      uint32_t ps = *portsc;

      if (!(ps & PORTSC_PP)) {
        *portsc = (ps | PORTSC_PP) & ~PORTSC_W1C;
      }
    }
  }

  for (uint32_t i = 0; i < num_ports; i++) {
    volatile uint32_t *portsc =
        (uint32_t *)((uint64_t)op_regs + 0x400 + (i * 0x10));
    uint32_t ps = *portsc;

    uint32_t port_id = i + 1;
    if (ps & PORTSC_CCS) {
      xhci_log_portsc(port_id, "[xHCI] Device detected", ps);
    } else {
      xhci_log_portsc(port_id, "[xHCI] Port", ps);
      continue;
    }

    // If the port is already usable, do NOT reset it. On some real controllers
    // a reset here can knock the link into Disabled/Recovery and it won't come
    // back.
    uint32_t initial_speed = xhci_port_speed(ps);
    uint32_t initial_pls = xhci_port_pls(ps);
    uint32_t initial_ped = (ps & PORTSC_PED) ? 1u : 0u;
    uint32_t ready = 0;
    if (initial_ped && initial_speed != 0 && initial_pls == 0) {
      serial_print("[xHCI] Port already ready; skipping reset\n");
      ready = 1;
    }

    if (!ready) {
      // Ensure port power is on (some controllers require PP before reset)
      if (!(ps & PORTSC_PP)) {
        *portsc = (ps | PORTSC_PP) & ~PORTSC_W1C;
        ps = *portsc;
      }

      // Clear any pending change bits before doing reset
      ps = *portsc;
      if (ps & PORTSC_W1C) {
        *portsc = ps | PORTSC_W1C;
      }

      ps = *portsc;
      uint32_t speed_hint = xhci_port_speed(ps);

      if (speed_hint == 4) {
        // SuperSpeed: prefer Warm Port Reset
        serial_print("[xHCI] SuperSpeed port; using Warm Reset\n");
        uint32_t w = ps;
        w &= ~PORTSC_W1C;
        w |= PORTSC_WPR;
        *portsc = w;

        for (uint32_t spins = 0; spins < 20000000; spins++) {
          ps = *portsc;
          if (!(ps & PORTSC_WPR))
            break;
        }

        ps = *portsc;
        if (ps & PORTSC_W1C) {
          *portsc = ps | PORTSC_W1C;
          ps = *portsc;
        }
        xhci_log_portsc(port_id, "[xHCI] After warm reset", ps);
      } else {
        // Non-SS: cold Port Reset
        uint32_t v = ps;
        v &= ~PORTSC_W1C;
        v |= PORTSC_PR;
        *portsc = v;

        // Wait for PR to clear
        uint32_t ok = 0;
        for (uint32_t spins = 0; spins < 20000000; spins++) {
          ps = *portsc;
          if (!(ps & PORTSC_PR)) {
            ok = 1;
            break;
          }
        }
        if (!ok) {
          serial_print("[xHCI] Port reset timeout PortSC=");
          xhci_print_hex32(*portsc);
          serial_print("\n");
        }

        // Clear RW1C bits that may be set after reset
        ps = *portsc;
        if (ps & PORTSC_W1C) {
          *portsc = ps | PORTSC_W1C;
        }

        ps = *portsc;
        xhci_log_portsc(port_id, "[xHCI] After cold reset", ps);
      }

      // Wait for the port to actually be usable (PED=1, speed!=0, and for USB3:
      // link in U0). On real hardware, speed may remain 0 until link training
      // completes.
      for (uint32_t spins = 0; spins < 20000000; spins++) {
        ps = *portsc;
        uint32_t speed = xhci_port_speed(ps);
        uint32_t pls = xhci_port_pls(ps);
        if ((ps & PORTSC_CCS) && (ps & PORTSC_PED) && (speed != 0) &&
            (pls == 0)) {
          ready = 1;
          break;
        }
      }
    }

    ps = *portsc;
    uint32_t speed = xhci_port_speed(ps);
    if (ready) {
      serial_print("[xHCI] Port ready\n");
    } else {
      serial_print("[xHCI] Port NOT ready after reset(s)\n");
      uint32_t pls = xhci_port_pls(ps);
      serial_print("[xHCI] Stuck state: PLS=");
      xhci_print_u32_dec(pls);
      serial_print(" speed=");
      xhci_print_u32_dec(speed);
      serial_print("\n");

      if (pls == 3) { // U3 (Suspend)
        serial_print("[xHCI] Port in U3; attempting Wakeup (PLS=15)...\n");
        uint32_t cmd = ps & ~PORTSC_W1C;
        cmd &= ~PORTSC_PLS_MASK;
        cmd |= (15u << 5); // Resume
        cmd |= (1u << 16); // LWS (Link Write Strobe) required to write PLS
        *portsc = cmd;

        // Wait for Resume to complete (transition to U0)
        for (uint32_t w = 0; w < 20000000; w++) {
          ps = *portsc;
          if (xhci_port_pls(ps) == 0) { // U0
            serial_print("[xHCI] Wakeup successful (U0)\n");
            ready = 1;
            break;
          }
        }
      }
    }
    xhci_log_portsc(port_id, "[xHCI] Final", ps);

    if (!ready) {
      // Don't attempt EnableSlot/AddressDevice with invalid speed/PED.
      continue;
    }

    // Enable Slot + Address Device (still no descriptor transfers yet)
    uint32_t slot_id = 0;
    if (!xhci_cmd_enable_slot(&slot_id)) {
      serial_print("[xHCI] EnableSlot failed\n");
      continue;
    }

    if (!xhci_cmd_address_device(slot_id, port_id, speed)) {
      serial_print("[xHCI] AddressDevice failed\n");
      continue;
    }

    serial_print("[xHCI] PORT SCAN END\n");
  }
}

void xhci_send_command(xhci_trb_t *trb) { (void)trb; }

void xhci_poll_events() {
  if (!event_ring)
    return;

  while (1) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt)) {
      break;
    }

    if (trb_type(evt.control) != TRB_TYPE_TRANSFER_EVENT) {
      continue;
    }

    uint32_t cc = trb_cc(evt.status);
    uint32_t slot_id = trb_slot_id(evt.control);
    uint32_t ep_id = (evt.control >> 16) & 0x1Fu;

    if (slot_id == 0 || slot_id >= 256) {
      continue;
    }

    xhci_device_state_t *dev = &g_devs[slot_id];
    if (dev->slot_id != slot_id) {
      continue;
    }

    // EP1 IN is DCI=3.
    if (ep_id != 3) {
      continue;
    }

    if (cc == 1 && dev->intr_buf) {
      uint32_t a =
          (uint32_t)(dev->intr_buf[0] | (dev->intr_buf[1] << 8) |
                     (dev->intr_buf[2] << 16) | (dev->intr_buf[3] << 24));
      uint32_t b =
          (uint32_t)(dev->intr_buf[4] | (dev->intr_buf[5] << 8) |
                     (dev->intr_buf[6] << 16) | (dev->intr_buf[7] << 24));
      if (1) {
        serial_print("[HID] slot=");
        xhci_print_u32_dec(slot_id);
        serial_print(" report=");
        xhci_print_hex32(a);
        serial_print(" ");
        xhci_print_hex32(b);
        serial_print("\n");
      }
    }

    // Re-arm interrupt IN for next report.
    if (dev->intr_ring && dev->intr_mps) {
      xhci_hid_start_polling(dev);
    }
  }
}
