#include "usb/xhci.h"
#include "bootlog.h"
#include "xhci_internal.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include <stddef.h>

/**
 * VexOS xHCI Driver (bring-up + command ring + event ring polling)
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

xhci_cap_regs_t *cap_regs;
xhci_op_regs_t *op_regs;
xhci_runtime_regs_t *runtime_regs;
uint32_t *doorbell_regs;

xhci_trb_t *command_ring;
uint32_t command_ring_index = 0;
uint8_t command_ring_cycle = 1;

xhci_trb_t *event_ring;
uint32_t event_ring_index = 0;
uint8_t event_ring_cycle = 1;
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

uint32_t trb_type(uint32_t control) { return (control >> 10) & 0x3F; }

uint32_t trb_cc(uint32_t status) { return (status >> 24) & 0xFF; }

uint32_t trb_slot_id(uint32_t control) { return (control >> 24) & 0xFF; }

uint32_t make_trb_control(uint32_t type, uint8_t cycle) {
  return (uint32_t)(cycle & 1u) | (type << 10);
}

void xhci_ring_doorbell_cmd(void) {
  // Ensure the command TRB and any Link TRB update are visible to the xHC.
  xhci_dma_write_barrier();
  // Doorbell 0, target=0 (Command Ring)
  doorbell_regs[0] = 0;
}


static void xhci_ring_doorbell_ep0(uint32_t slot_id);
static void xhci_ep0_ring_push(xhci_device_state_t *dev, const xhci_trb_t *trb);
void xhci_print_u32_dec(uint32_t v);
void xhci_print_hex32(uint32_t v);

xhci_trb_t *xhci_alloc_tr_ring(void);

void xhci_cmd_ring_push(const xhci_trb_t *trb);

static void xhci_log_ep0_request(const char *direction,
                                const xhci_device_state_t *dev,
                                uint64_t setup, uint32_t transfer_len) {
  serial_print("[xHCI][EP0] ");
  serial_print(direction);
  serial_print(" slot=");
  xhci_print_u32_dec(dev ? dev->slot_id : 0);
  serial_print(" bmRequestType=");
  xhci_print_hex32((uint32_t)(setup & 0xFFu));
  serial_print(" request=");
  xhci_print_hex32((uint32_t)((setup >> 8) & 0xFFu));
  serial_print(" value=");
  xhci_print_hex32((uint32_t)((setup >> 16) & 0xFFFFu));
  serial_print(" index=");
  xhci_print_hex32((uint32_t)((setup >> 32) & 0xFFFFu));
  serial_print(" wLength=");
  xhci_print_u32_dec((uint32_t)((setup >> 48) & 0xFFFFu));
  serial_print(" transfer_len=");
  xhci_print_u32_dec(transfer_len);
  serial_print("\n");
}

int xhci_ep0_control_in(xhci_device_state_t *dev, uint64_t setup, void *buf,
                        uint32_t len) {
  xhci_log_ep0_request("IN", dev, setup, len);
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

int xhci_ep0_control_no_data_out(xhci_device_state_t *dev, uint64_t setup) {
  xhci_log_ep0_request("OUT", dev, setup, 0);
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
  dev->hid_subclass = 0;
  dev->hid_proto = 0;
  dev->hid_report_desc_len = 0;
  dev->hid_has_report_id = 0;
  dev->hid_mouse_report_id = 0;
  dev->hid_mouse_valid = 0;
  dev->hid_buttons_bitoff = 0;
  dev->hid_buttons_bits = 0;
  dev->hid_x_bitoff = 0;
  dev->hid_x_bits = 0;
  dev->hid_x_is_relative = 0;
  dev->hid_y_bitoff = 0;
  dev->hid_y_bits = 0;
  dev->hid_y_is_relative = 0;
  dev->hid_wheel_bitoff = 0;
  dev->hid_wheel_bits = 0;
  dev->hid_wheel_is_relative = 0;
  dev->hid_abs_have_last = 0;
  dev->hid_abs_last_x = 0;
  dev->hid_abs_last_y = 0;
  dev->intr_epaddr = 0;
  dev->intr_dci = 0;
  dev->intr_mps = 0;
  dev->intr_interval = 0;

  dev->kbd_prev_mod = 0;
  for (uint32_t i = 0; i < 6; i++) {
    dev->kbd_prev_keys[i] = 0;
  }

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
        dev->hid_subclass = sub;
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
    } else if (bType == 0x21 && bLength >= 9) {
      if (cur_ifclass == 3) {
        uint16_t rep_len =
            (uint16_t)(buf[off + 7] | (uint16_t)(buf[off + 8] << 8));
        dev->hid_report_desc_len = rep_len;
      }
    } else if (bType == 5 && bLength >= 7) {
      uint8_t epaddr = buf[off + 2];
      uint8_t attr = buf[off + 3];
      uint16_t mps = (uint16_t)(buf[off + 4] | (buf[off + 5] << 8));
      uint8_t interval = buf[off + 6];

      uint8_t xfertype = attr & 0x3u;
      uint8_t dir_in = (epaddr & 0x80u) ? 1u : 0u;
      if (dev->intr_epaddr == 0 && cur_ifclass == 3 &&
          (cur_ifsub == 0 || cur_ifsub == 1) && dir_in && xfertype == 3) {
        dev->intr_epaddr = epaddr;
        uint8_t epnum = (uint8_t)(epaddr & 0x0Fu);
        dev->intr_dci = (uint8_t)(2u * (uint32_t)epnum + 1u);
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

void xhci_cmd_ring_push(const xhci_trb_t *trb) {
  // Place TRB at current index
  command_ring[command_ring_index] = *trb;

  // Force cycle bit
  command_ring[command_ring_index].control &= ~1u;
  command_ring[command_ring_index].control |=
      (uint32_t)(command_ring_cycle & 1u);

  command_ring_index++;

  // Publish preceding entries before exposing the Link TRB to the controller.
  if (command_ring_index == (XHCI_CMD_RING_TRBS - 1)) {
    xhci_dma_write_barrier();
    command_ring[XHCI_CMD_RING_TRBS - 1].control ^= 1u;
    if (command_ring[XHCI_CMD_RING_TRBS - 1].control & (1u << 1))
      command_ring_cycle ^= 1u;
    command_ring_index = 0;
  }

  // The caller may ring the command doorbell immediately after this returns.
  xhci_dma_write_barrier();
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

uint32_t g_ctx_size = 32;

void xhci_ring_doorbell_ep(uint32_t slot_id, uint32_t dci) {
  // Publish queued endpoint TRBs before notifying the controller.
  xhci_dma_write_barrier();
  doorbell_regs[slot_id] = dci;
}
void xhci_print_u32_dec(uint32_t v) {
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

void xhci_print_i32_dec(int32_t v) {
  if (v < 0) {
    serial_print("-");
    // Avoid overflow for INT32_MIN by converting through int64.
    int64_t t = -(int64_t)v;
    xhci_print_u32_dec((uint32_t)t);
    return;
  }
  xhci_print_u32_dec((uint32_t)v);
}

void xhci_print_hex8(uint8_t v) {
  static const char *hex = "0123456789ABCDEF";
  char s[3];
  s[0] = hex[(v >> 4) & 0xFu];
  s[1] = hex[(v >> 0) & 0xFu];
  s[2] = 0;
  serial_print(s);
}

// PortSC bits
static const uint32_t PORTSC_CCS = 1u << 0; // Current Connect Status
static const uint32_t PORTSC_PED = 1u << 1; // Port Enabled/Disabled
static const uint32_t PORTSC_PR = 1u << 4;  // Port Reset
static const uint32_t PORTSC_PLS_MASK = 0xFu << 5;
static const uint32_t PORTSC_PP = 1u << 9; // Port Power
static const uint32_t PORTSC_SPEED_MASK = 0xFu << 10;
static const uint32_t PORTSC_LWS = 1u << 16; // Port Link State Write Strobe
static const uint32_t PORTSC_PIC_MASK = 0x3u << 14;
static const uint32_t PORTSC_WAKE_MASK = 0x7u << 25;
// RW1C bits (write 1 to clear).
static const uint32_t PORTSC_W1C = (1u << 17) | (1u << 18) | (1u << 19) |
                                  (1u << 20) | (1u << 21) | (1u << 22);
static const uint32_t PORTSC_WPR = 1u << 31; // Warm Port Reset (USB3)

static uint32_t xhci_port_speed(uint32_t portsc) {
  return (portsc & PORTSC_SPEED_MASK) >> 10;
}

static uint32_t xhci_port_pls(uint32_t portsc) { return (portsc >> 5) & 0xFu; }

// Only replay ordinary writable configuration fields when updating PORTSC.
// PED is RW1C (writing 1 disables the port), while PR/WPR are write-one-to-set
// commands and PLS is only changed when LWS is asserted. Never echo those bits
// from a status read back into the register.
static uint32_t xhci_portsc_rw_config(uint32_t ps) {
  return ps & (PORTSC_PP | PORTSC_PIC_MASK | PORTSC_WAKE_MASK);
}

static const char *xhci_speed_name(uint32_t speed) {
  switch (speed) {
  case 0:
    return "none";
  case 1:
    return "full"; // USB 1.1 full-speed
  case 2:
    return "low"; // USB 1.1 low-speed
  case 3:
    return "high"; // USB 2.0 high-speed
  case 4:
    return "superspeed"; // USB 3.x
  case 5:
    return "superspeed+"; // USB 3.1/3.2 SuperSpeedPlus (common on orange ports)
  default:
    return "unknown";
  }
}

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
  {
    uint32_t sp = xhci_port_speed(portsc);
    xhci_print_u32_dec(sp);
    serial_print("(");
    serial_print(xhci_speed_name(sp));
    serial_print(")");
  }
  serial_print(" PED=");
  xhci_print_u32_dec((portsc >> 1) & 1u);
  serial_print("\n");
}

static void xhci_log_controller_registers(const char *phase) {
  serial_print("[xHCI][REGS] ");
  serial_print(phase);
  serial_print(" HCIVERSION=");
  xhci_print_hex32((uint32_t)cap_regs->hci_version);
  serial_print(" CAPLENGTH=");
  xhci_print_u32_dec(cap_regs->cap_length);
  serial_print(" HCS1=");
  xhci_print_hex32(cap_regs->hcs_params1);
  serial_print(" HCS2=");
  xhci_print_hex32(cap_regs->hcs_params2);
  serial_print(" HCS3=");
  xhci_print_hex32(cap_regs->hcs_params3);
  serial_print(" HCC1=");
  xhci_print_hex32(cap_regs->hcc_params1);
  serial_print(" DBOFF=");
  xhci_print_hex32(cap_regs->dboff);
  serial_print(" RTSOFF=");
  xhci_print_hex32(cap_regs->rtsoff);
  serial_print("\n");

  serial_print("[xHCI][REGS] USBCMD=");
  xhci_print_hex32(op_regs->usb_cmd);
  serial_print(" USBSTS=");
  xhci_print_hex32(op_regs->usb_sts);
  serial_print(" PAGESIZE=");
  xhci_print_hex32(op_regs->page_size);
  serial_print(" CRCR=");
  print_hex64(op_regs->crcr);
  serial_print(" DCBAAP=");
  print_hex64(op_regs->dcbaap);
  serial_print(" CONFIG=");
  xhci_print_hex32(op_regs->config);
  serial_print("\n");

  if (runtime_regs) {
    serial_print("[xHCI][REGS] MFINDEX=");
    xhci_print_hex32(runtime_regs->mfindex);
    serial_print(" IMAN0=");
    xhci_print_hex32(runtime_regs->interrupters[0].iman);
    serial_print(" ERSTSZ0=");
    xhci_print_hex32(runtime_regs->interrupters[0].erstsz);
    serial_print(" ERSTBA0=");
    print_hex64(runtime_regs->interrupters[0].erstba);
    serial_print(" ERDP0=");
    print_hex64(runtime_regs->interrupters[0].erdp);
    serial_print("\n");
  }
}

static int xhci_port_ready(uint32_t ps) {
  return (ps & PORTSC_CCS) && (ps & PORTSC_PED) &&
         (xhci_port_speed(ps) != 0) && (xhci_port_pls(ps) == 0);
}

static void xhci_port_power_on(volatile uint32_t *portsc, uint32_t *ps) {
  if (!(*ps & PORTSC_PP)) {
    // Do not echo PED or reset-command bits from the read value.
    *portsc = xhci_portsc_rw_config(*ps) | PORTSC_PP;
    *ps = *portsc;
  }
}

static void xhci_port_clear_w1c(volatile uint32_t *portsc, uint32_t *ps) {
  uint32_t change_bits = *ps & PORTSC_W1C;
  if (change_bits) {
    // Clear only the observed RW1C change bits. In particular, do not write
    // back PED=1: that would disable a port immediately after a successful reset.
    *portsc = xhci_portsc_rw_config(*ps) | change_bits;
    *ps = *portsc;
  }
}

static int xhci_port_wait_ready(volatile uint32_t *portsc,
                                uint32_t port_id, const char *stage,
                                uint64_t timeout_us, uint32_t *out_ps) {
  uint64_t start = xhci_time_now_ticks();
  uint64_t deadline = xhci_time_deadline_us(timeout_us);
  while (!xhci_time_expired(deadline)) {
    uint32_t ps = *portsc;
    if (xhci_port_ready(ps)) {
      if (out_ps)
        *out_ps = ps;
      xhci_time_log_elapsed(stage, start, "ready");
      xhci_log_portsc(port_id, "[xHCI][PORT] Ready state", ps);
      return 1;
    }
    __asm__ volatile("pause");
  }

  uint32_t final_ps = *portsc;
  if (out_ps)
    *out_ps = final_ps;
  int ready = xhci_port_ready(final_ps);
  xhci_time_log_elapsed(stage, start, ready ? "ready-at-deadline" : "timeout");
  xhci_log_portsc(port_id, "[xHCI][PORT] Wait final state", final_ps);
  return ready;
}

static void xhci_port_force_u0(volatile uint32_t *portsc) {
  uint32_t ps = *portsc;
  if (!(ps & PORTSC_CCS))
    return;
  if (xhci_port_pls(ps) == 0)
    return;

  // Request U0 without echoing PED=1 (which would disable the port).
  uint32_t v = xhci_portsc_rw_config(ps) | PORTSC_LWS;
  *portsc = v;
}

static int xhci_port_warm_reset(volatile uint32_t *portsc,
                                 uint32_t port_id, uint64_t timeout_us) {
  uint64_t start = xhci_time_now_ticks();
  uint32_t ps = *portsc;
  *portsc = xhci_portsc_rw_config(ps) | PORTSC_WPR;

  uint64_t deadline = xhci_time_deadline_us(timeout_us);
  while (!xhci_time_expired(deadline)) {
    ps = *portsc;
    if (!(ps & PORTSC_WPR)) {
      xhci_time_log_elapsed("port_warm_reset", start, "complete");
      xhci_log_portsc(port_id, "[xHCI][PORT] After warm reset", ps);
      return 1;
    }
    __asm__ volatile("pause");
  }

  ps = *portsc;
  int complete = (ps & PORTSC_WPR) == 0;
  xhci_time_log_elapsed("port_warm_reset", start,
                        complete ? "complete-at-deadline" : "timeout");
  xhci_log_portsc(port_id, "[xHCI][PORT] Warm reset final", ps);
  return complete;
}

static int xhci_port_cold_reset(volatile uint32_t *portsc,
                                uint32_t port_id, uint64_t timeout_us) {
  uint64_t start = xhci_time_now_ticks();
  uint32_t ps = *portsc;
  *portsc = xhci_portsc_rw_config(ps) | PORTSC_PR;

  uint64_t deadline = xhci_time_deadline_us(timeout_us);
  while (!xhci_time_expired(deadline)) {
    ps = *portsc;
    if (!(ps & PORTSC_PR)) {
      xhci_time_log_elapsed("port_cold_reset", start, "complete");
      xhci_log_portsc(port_id, "[xHCI][PORT] After cold reset", ps);
      return 1;
    }
    __asm__ volatile("pause");
  }

  ps = *portsc;
  int complete = (ps & PORTSC_PR) == 0;
  xhci_time_log_elapsed("port_cold_reset", start,
                        complete ? "complete-at-deadline" : "timeout");
  xhci_log_portsc(port_id, "[xHCI][PORT] Cold reset final", ps);
  return complete;
}

static int xhci_force_port_ready(volatile uint32_t *portsc, uint32_t port_id,
                                 uint32_t *out_speed) {
  uint32_t ps = *portsc;
  xhci_port_power_on(portsc, &ps);
  xhci_port_clear_w1c(portsc, &ps);

  xhci_log_portsc(port_id, "[xHCI][PORT] Recovery start", ps);
  if (xhci_port_ready(ps)) {
    if (out_speed)
      *out_speed = xhci_port_speed(ps);
    return 1;
  }

  if (!(ps & PORTSC_CCS))
    return 0;

  uint32_t speed_hint = xhci_port_speed(ps);

  /*
   * Do not retry identical resets four times. A single bounded recovery
   * sequence is enough to distinguish a slow transition from a dead port.
   */
  if (speed_hint >= 4) {
    serial_print("[xHCI] Forcing warm reset (USB3)\n");
    (void)bootlog_checkpoint();
    if (xhci_port_warm_reset(portsc, port_id, 100000u)) {
      ps = *portsc;
      xhci_port_clear_w1c(portsc, &ps);
      (void)bootlog_checkpoint();

      if (xhci_port_wait_ready(portsc, port_id, "after_reset_ready", 300000u, &ps)) {
        if (out_speed)
          *out_speed = xhci_port_speed(ps);
        return 1;
      }
    } else {
      serial_print("[xHCI] Warm reset timed out\n");
    }

    xhci_port_force_u0(portsc);
    if (xhci_port_wait_ready(portsc, port_id, "after_u0_request", 150000u, &ps)) {
      if (out_speed)
        *out_speed = xhci_port_speed(ps);
      return 1;
    }
  }

  serial_print("[xHCI] Forcing cold reset\n");
  (void)bootlog_checkpoint();
  if (!xhci_port_cold_reset(portsc, port_id, 100000u)) {
    serial_print("[xHCI] Cold reset timed out\n");
    ps = *portsc;
    if (out_speed)
      *out_speed = xhci_port_speed(ps);
    return 0;
  }

  ps = *portsc;
  xhci_port_clear_w1c(portsc, &ps);
  xhci_log_portsc(port_id, "[xHCI] After cold reset", ps);
  (void)bootlog_checkpoint();

  if (xhci_port_wait_ready(portsc, port_id, "after_reset_ready", 300000u, &ps)) {
    if (out_speed)
      *out_speed = xhci_port_speed(ps);
    return 1;
  }

  if (out_speed)
    *out_speed = xhci_port_speed(ps);
  return 0;
}

static void xhci_bios_handoff(void) {
  /* HCCPARAMS1.xECP is an offset in dwords from the capability base. */
  uint32_t hcc1 = cap_regs->hcc_params1;
  uint32_t off = ((hcc1 >> 16) & 0xFFFFu) * 4u;
  if (!off)
    return;

  for (uint32_t visited = 0; visited < 256u; ++visited) {
    if (off > XHCI_MMIO_MAP_SIZE - sizeof(uint32_t)) {
      serial_print("[xHCI] Extended capability offset out of range; stopping scan\n");
      return;
    }

    volatile uint32_t *ext = (volatile uint32_t *)((uint64_t)cap_regs + off);
    uint32_t cap = ext[0];
    uint8_t cap_id = (uint8_t)(cap & 0xFFu);
    uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);

    if (cap_id == 1u) {
      serial_print("[xHCI] USBLEGSUP found; requesting OS ownership\n");
      ext[0] = cap | (1u << 24);

      uint64_t deadline = xhci_time_deadline_us(500000u);
      while (!xhci_time_expired(deadline)) {
        if ((ext[0] & (1u << 16)) == 0) {
          serial_print("[xHCI] BIOS ownership released\n");
          return;
        }
        __asm__ volatile("pause");
      }
      serial_print("[xHCI] BIOS ownership handoff timed out; continuing\n");
      return;
    }

    if (next == 0)
      return;
    off += (uint32_t)next * 4u;
  }

  serial_print("[xHCI] Extended capability chain limit reached\n");
}

void xhci_print_hex32(uint32_t v) {
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

xhci_trb_t *xhci_alloc_tr_ring(void) {
  xhci_trb_t *ring = (xhci_trb_t *)PMM_AllocatePage();
  for (int i = 0; i < 256; i++) {
    ring[i].data = 0;
    ring[i].status = 0;
    ring[i].control = 0;
  }
  // Link TRB at end
  ring[255].data = (uint64_t)(uintptr_t)ring;
  ring[255].status = 0;
  // C=0 keeps the Link TRB unavailable until the producer reaches it.
  ring[255].control = make_trb_control(TRB_TYPE_LINK, 0) | (1u << 1);
  return ring;
}

xhci_device_state_t g_devs[256];

static void xhci_ring_doorbell_ep0(uint32_t slot_id) {
  // Publish the complete control TD before ringing EP0.
  xhci_dma_write_barrier();
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
    // Publish preceding control TRBs before handing over the Link TRB.
    xhci_dma_write_barrier();
    dev->ep0_ring[255].control ^= 1u;
    if (dev->ep0_ring[255].control & (1u << 1))
      dev->ep0_cycle ^= 1u;
    dev->ep0_index = 0;
  }

  xhci_dma_write_barrier();
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

static int xhci_cmd_evaluate_ctx_update_ep0_mps(xhci_device_state_t *dev,
                                                uint32_t new_mps) {
  if (!dev || dev->slot_id == 0)
    return 0;

  uint8_t *input_ctx = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    input_ctx[i] = 0;

  xhci_input_control_ctx_t *icc = (xhci_input_control_ctx_t *)input_ctx;
  icc->add_flags = (1u << 1);
  icc->drop_flags = 0;

  xhci_ep_ctx_32_t *ep0 = (xhci_ep_ctx_32_t *)(input_ctx + (2 * g_ctx_size));
  ep0->dword1 = ((new_mps & 0xFFFFu) << 16) | ((4u & 0x7u) << 3);

  xhci_trb_t cmd;
  cmd.data = (uint64_t)(uintptr_t)input_ctx;
  cmd.status = 0;
  cmd.control = make_trb_control(TRB_TYPE_EVALUATE_CTX_CMD, command_ring_cycle) |
                (dev->slot_id << 24);

  xhci_cmd_ring_push(&cmd);
  xhci_ring_doorbell_cmd();

  uint32_t evt_slot = 0;
  uint32_t cc = 0;
  if (!xhci_wait_for_command_completion(&evt_slot, &cc)) {
    serial_print("[xHCI] EvaluateContext: timeout\n");
    return 0;
  }

  if (cc != 1 || evt_slot != dev->slot_id) {
    serial_print("[xHCI] EvaluateContext: completion_code=");
    xhci_print_u32_dec(cc);
    serial_print(" slot_id=");
    xhci_print_u32_dec(evt_slot);
    serial_print("\n");
    return 0;
  }

  dev->ep0_mps = new_mps;
  return 1;
}

static int xhci_ep0_get_device_descriptor(xhci_device_state_t *dev) {
  // USB Device Descriptor is 18 bytes
  uint8_t *buf = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;

  // Setup packet (USB2.0 9.3): GET_DESCRIPTOR(Device)
  // bmRequestType=0x80 (Device-to-host, Standard, Device)
  // bRequest=6, wValue=(1<<8)|0, wIndex=0, wLength=18
  uint64_t setup8 = 0;
  setup8 |= 0x80ULL;
  setup8 |= 0x06ULL << 8;
  setup8 |= 0x0100ULL << 16;
  setup8 |= 0x0000ULL << 32;
  setup8 |= 8ULL << 48;

  if (!xhci_ep0_control_in(dev, setup8, buf, 8)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(8) failed\n");
    return 0;
  }

  uint8_t mps0 = buf[7];
  if (dev->speed_code == 1) {
    if (mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64) {
      if (dev->ep0_mps != (uint32_t)mps0) {
        if (!xhci_cmd_evaluate_ctx_update_ep0_mps(dev, (uint32_t)mps0)) {
          serial_print("[xHCI] EP0 update MPS failed\n");
          return 0;
        }
      }
    }
  }

  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;

  uint64_t setup18 = 0;
  setup18 |= 0x80ULL;
  setup18 |= 0x06ULL << 8;
  setup18 |= 0x0100ULL << 16;
  setup18 |= 0x0000ULL << 32;
  setup18 |= 18ULL << 48;

  if (!xhci_ep0_control_in(dev, setup18, buf, 18)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(18) failed\n");
    return 0;
  }

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
  if (speed_code == 3)
    mps = 64; // high-speed
  if (speed_code >= 4)
    mps = 512; // superspeed / superspeed+

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
  dev->hid_subclass = 0;
  dev->hid_proto = 0;
  dev->hid_report_desc_len = 0;
  dev->hid_has_report_id = 0;
  dev->hid_mouse_report_id = 0;
  dev->hid_mouse_valid = 0;
  dev->hid_buttons_bitoff = 0;
  dev->hid_buttons_bits = 0;
  dev->hid_x_bitoff = 0;
  dev->hid_x_bits = 0;
  dev->hid_y_bitoff = 0;
  dev->hid_y_bits = 0;
  dev->hid_wheel_bitoff = 0;
  dev->hid_wheel_bits = 0;
  dev->intr_epaddr = 0;
  dev->intr_dci = 0;
  dev->intr_mps = 0;
  dev->intr_interval = 0;

  dev->kbd_prev_mod = 0;
  for (uint32_t i = 0; i < 6; i++) {
    dev->kbd_prev_keys[i] = 0;
  }

  // Next milestone: read the USB Device Descriptor via EP0 control transfer
  if (!xhci_ep0_get_device_descriptor(dev)) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR failed\n");
  }

  if (!xhci_ep0_get_config_and_set_config(dev)) {
    serial_print("[xHCI] Config descriptor / set config failed\n");
  }

  // Basic HID bring-up: try parsing report descriptor first, then choose protocol.
  if (dev->hid_ifnum != 0xFF) {
    xhci_hid_try_parse_mouse_report_desc(dev);

    if (dev->hid_subclass == 1) {
      if (dev->hid_proto == 1) {
        // Keyboard: we only support boot protocol for now.
        serial_print("[HID] SET_PROTOCOL(boot) for keyboard\n");
        (void)xhci_hid_set_protocol_boot(dev);
      } else if (dev->hid_proto == 2) {
        // Mouse: only use boot protocol if we couldn't parse the report descriptor.
        if (!dev->hid_mouse_valid) {
          serial_print("[HID] SET_PROTOCOL(boot) for mouse (fallback)\n");
          (void)xhci_hid_set_protocol_boot(dev);
        } else {
          serial_print("[HID] Using report protocol for mouse\n");
        }
      }
    }

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

  /* xHCI initializes before IRQ0 is enabled; prepare a standalone clock. */
  xhci_time_init();
  xhci_time_log_clock();
  uint64_t controller_init_start = xhci_time_now_ticks();
  xhci_log_controller_registers("initial");

  // Real hardware often requires BIOS->OS ownership handoff.
  {
    uint64_t handoff_start = xhci_time_now_ticks();
    xhci_bios_handoff();
    xhci_time_log_elapsed("bios_ownership_handoff", handoff_start, "finished");
    xhci_log_controller_registers("after-bios-handoff");
  }

  // Context size (HCCPARAMS1.CSZ): 0=32B contexts, 1=64B contexts
  g_ctx_size = (cap_regs->hcc_params1 & (1u << 2)) ? 64u : 32u;
  serial_print("[xHCI] Context size: ");
  xhci_print_u32_dec(g_ctx_size);
  serial_print("\n");

  // 2) Reset controller
  serial_print("[xHCI] Resetting Controller...\n");
  op_regs->usb_cmd &= ~USB_CMD_RS;
  serial_print("[xHCI] Waiting for halt (timeout 1000 ms)...\n");
  (void)bootlog_checkpoint();
  (void)bootlog_flush();
  {
    uint64_t wait_start = xhci_time_now_ticks();
    uint64_t deadline = xhci_time_deadline_us(1000000u);
    while (!(op_regs->usb_sts & USB_STS_HCH) &&
           !xhci_time_expired(deadline))
      __asm__ volatile("pause");
    if (!(op_regs->usb_sts & USB_STS_HCH)) {
      xhci_time_log_elapsed("controller_halt", wait_start, "timeout");
      serial_print("[xHCI] ERROR: controller did not halt; skipping this controller\n");
      xhci_log_controller_registers("halt-timeout");
      xhci_time_log_elapsed("controller_init", controller_init_start, "halt-timeout");
      (void)bootlog_checkpoint();
      return;
    }
    xhci_time_log_elapsed("controller_halt", wait_start, "halted");
    xhci_log_controller_registers("halted");
  }

  serial_print("[xHCI] Issuing Reset...\n");
  (void)bootlog_checkpoint();
  (void)bootlog_flush();
  op_regs->usb_cmd |= USB_CMD_HCRST;
  {
    uint64_t wait_start = xhci_time_now_ticks();
    uint64_t deadline = xhci_time_deadline_us(1000000u);
    while ((op_regs->usb_cmd & USB_CMD_HCRST) &&
           !xhci_time_expired(deadline))
      __asm__ volatile("pause");
    if (op_regs->usb_cmd & USB_CMD_HCRST) {
      xhci_time_log_elapsed("controller_reset", wait_start, "timeout");
      serial_print("[xHCI] ERROR: controller reset timed out; skipping this controller\n");
      xhci_log_controller_registers("reset-timeout");
      xhci_time_log_elapsed("controller_init", controller_init_start, "reset-timeout");
      (void)bootlog_checkpoint();
      return;
    }
    xhci_time_log_elapsed("controller_reset", wait_start, "complete");
    xhci_log_controller_registers("after-reset-bit-clear");
  }

  serial_print("[xHCI] Waiting for CNR (timeout 1000 ms)...\n");
  (void)bootlog_checkpoint();
  (void)bootlog_flush();
  {
    const uint32_t USB_STS_CNR = 1u << 11;
    uint64_t wait_start = xhci_time_now_ticks();
    uint64_t deadline = xhci_time_deadline_us(1000000u);
    while ((op_regs->usb_sts & USB_STS_CNR) &&
           !xhci_time_expired(deadline))
      __asm__ volatile("pause");
    if (op_regs->usb_sts & USB_STS_CNR) {
      xhci_time_log_elapsed("controller_not_ready", wait_start, "timeout");
      serial_print("[xHCI] ERROR: controller not ready after reset; skipping this controller\n");
      xhci_log_controller_registers("cnr-timeout");
      xhci_time_log_elapsed("controller_init", controller_init_start, "cnr-timeout");
      (void)bootlog_checkpoint();
      return;
    }
    xhci_time_log_elapsed("controller_not_ready", wait_start, "ready");
    xhci_log_controller_registers("controller-ready");
  }

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
  // C=0 initially; command_ring_cycle/CRCR.RCS start at 1. TC=1 toggles both
  // consumer and producer cycle states when this Link TRB is traversed.
  command_ring[XHCI_CMD_RING_TRBS - 1].control =
      make_trb_control(TRB_TYPE_LINK, 0) | (1u << 1);

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
  xhci_log_controller_registers("running");

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
  uint32_t connected_ports = 0;
  uint32_t ready_ports = 0;
  uint32_t failed_ports = 0;
  uint32_t addressed_devices = 0;
  uint64_t port_scan_start = xhci_time_now_ticks();

  /*
   * Power every root port once. Only incur the power-good delay if we actually
   * changed a port from unpowered to powered; do not run a huge CPU spin loop
   * or repeatedly write PORTSC while waiting.
   */
  serial_print("[xHCI] Powering on root ports...\n");
  int powered_any_port = 0;
  for (uint32_t i = 0; i < num_ports; i++) {
    volatile uint32_t *portsc =
        (uint32_t *)((uint64_t)op_regs + 0x400 + (i * 0x10));
    uint32_t ps = *portsc;
    uint32_t port_id = i + 1u;

    if (!(ps & PORTSC_PP)) {
      xhci_log_portsc(port_id, "[xHCI][PORT] Power-on before", ps);
      *portsc = xhci_portsc_rw_config(ps) | PORTSC_PP;
      ps = *portsc;
      xhci_log_portsc(port_id, "[xHCI][PORT] Power-on after", ps);
      powered_any_port = 1;
    }
  }

  if (powered_any_port) {
    serial_print("[xHCI] Waiting 100 ms for root-port power-good...\n");
    uint64_t power_good_start = xhci_time_now_ticks();
    xhci_time_delay_us(100000u);
    xhci_time_log_elapsed("root_port_power_good", power_good_start, "complete");
  }

  for (uint32_t i = 0; i < num_ports; i++) {
    volatile uint32_t *portsc =
        (uint32_t *)((uint64_t)op_regs + 0x400 + (i * 0x10));
    uint32_t ps = *portsc;

    uint32_t port_id = i + 1;
    if (ps & PORTSC_CCS) {
      ++connected_ports;
      xhci_log_portsc(port_id, "[xHCI] Device detected", ps);
    } else {
      xhci_log_portsc(port_id, "[xHCI] Port", ps);
      continue;
    }

    // If the port is already usable, do NOT reset it. On some real controllers
    // a reset here can knock the link into Disabled/Recovery and it won't come
    // back.
    uint32_t speed = 0;
    uint32_t ready = 0;

    // Use the more robust helper that:
    // - powers on the port
    // - clears W1C change bits
    // - tries warm/cold reset in multiple passes
    // - forces the port into U0 if needed
    ready = (uint32_t)xhci_force_port_ready(portsc, port_id, &speed);
    ps = *portsc;
    xhci_log_portsc(port_id, "[xHCI] Final", ps);

    if (ready) {
      ++ready_ports;
      serial_print("[xHCI] Port ready\n");
    } else {
      ++failed_ports;
      serial_print("[xHCI] Port NOT ready after reset(s)\n");
      serial_print("[xHCI] Stuck state: PLS=");
      xhci_print_u32_dec(xhci_port_pls(ps));
      serial_print(" speed=");
      xhci_print_u32_dec(speed);
      serial_print("\n");
      (void)bootlog_checkpoint();
      (void)bootlog_flush();
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

    ++addressed_devices;
    serial_print("[xHCI] Device addressed port=");
    xhci_print_u32_dec(port_id);
    serial_print(" slot=");
    xhci_print_u32_dec(slot_id);
    serial_print(" speed=");
    xhci_print_u32_dec(speed);
    serial_print("\n");
  }

  serial_print("[xHCI][SUMMARY] ports=");
  xhci_print_u32_dec(num_ports);
  serial_print(" connected=");
  xhci_print_u32_dec(connected_ports);
  serial_print(" ready=");
  xhci_print_u32_dec(ready_ports);
  serial_print(" failed=");
  xhci_print_u32_dec(failed_ports);
  serial_print(" addressed=");
  xhci_print_u32_dec(addressed_devices);
  serial_print("\n");
  xhci_time_log_elapsed("root_port_scan", port_scan_start, "finished");
  xhci_time_log_elapsed("controller_init", controller_init_start, "finished");
  xhci_log_controller_registers("init-complete");
  (void)bootlog_checkpoint();
  (void)bootlog_flush();
}