#include "usb/xhci.h"
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

volatile xhci_cap_regs_t *cap_regs;
volatile xhci_op_regs_t *op_regs;
volatile xhci_runtime_regs_t *runtime_regs;
volatile uint32_t *doorbell_regs;

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
  // Doorbell 0, target=0 (Command Ring)
  __asm__ volatile("sfence" ::: "memory");
  doorbell_regs[0] = 0;
  (void)doorbell_regs[0];
}


static void xhci_ring_doorbell_ep0(uint32_t slot_id);
static void xhci_ep0_ring_push(xhci_device_state_t *dev, const xhci_trb_t *trb);
void xhci_print_u32_dec(uint32_t v);
void xhci_print_hex32(uint32_t v);

xhci_trb_t *xhci_alloc_tr_ring(void);

void xhci_cmd_ring_push(const xhci_trb_t *trb);

int xhci_ep0_control_in(xhci_device_state_t *dev, uint64_t setup, void *buf, uint32_t len) {
  if (!dev || dev->ep0_failed || !buf || !len || len > 4096) return 0;
  dev->control_length = len;
  xhci_log_device(dev, "control IN");
  serial_print(" request="); xhci_print_hex32((uint32_t)setup);
  serial_print(" length="); xhci_print_u32_dec(len); serial_print("\n");
  /* 6.4.1.2: TRT=3 for IN, IDT on Setup, IOC only on Status.
     ISP reports a short Data stage; we still wait for the Status stage. */
  xhci_trb_t t = {setup, 8, (TRB_TYPE_SETUP_STAGE<<10) | (3u<<16) | (1u<<6)};
  dev->ep0_setup_trb = (uint64_t)&dev->ep0_ring[dev->ep0_index];
  xhci_ep0_ring_push(dev, &t);
  t = (xhci_trb_t){(uint64_t)buf, len, (TRB_TYPE_DATA_STAGE<<10) | (1u<<16) | (1u<<2)};
  dev->ep0_data_trb = (uint64_t)&dev->ep0_ring[dev->ep0_index];
  xhci_ep0_ring_push(dev, &t);
  t = (xhci_trb_t){0, 0, (TRB_TYPE_STATUS_STAGE<<10) | (1u<<5)};
  dev->ep0_status_trb = (uint64_t)&dev->ep0_ring[dev->ep0_index];
  xhci_ep0_ring_push(dev, &t);
  xhci_ring_doorbell_ep0(dev->slot_id);
  uint32_t cc = 0;
  if (!xhci_wait_for_transfer_event(dev->slot_id, &cc) || cc != 1) {
    if (cc == 6) (void)xhci_recover_ep0(dev);
    else dev->ep0_failed = 1;
    return 0;
  }
  return 1;
}

int xhci_ep0_control_no_data_out(xhci_device_state_t *dev, uint64_t setup) {
  if (!dev || dev->ep0_failed) return 0;
  xhci_log_device(dev, "control OUT");
  serial_print(" request="); xhci_print_hex32((uint32_t)setup); serial_print("\n");
  dev->control_length = 0; dev->ep0_data_trb = 0;
  xhci_trb_t t = {setup, 8, (TRB_TYPE_SETUP_STAGE<<10) | (1u<<6)};
  dev->ep0_setup_trb = (uint64_t)&dev->ep0_ring[dev->ep0_index];
  xhci_ep0_ring_push(dev, &t);
  t = (xhci_trb_t){0, 0, (TRB_TYPE_STATUS_STAGE<<10) | (1u<<16) | (1u<<5)};
  dev->ep0_status_trb = (uint64_t)&dev->ep0_ring[dev->ep0_index];
  xhci_ep0_ring_push(dev, &t);
  xhci_ring_doorbell_ep0(dev->slot_id);
  uint32_t cc = 0;
  if (!xhci_wait_for_transfer_event(dev->slot_id, &cc) || cc != 1) {
    if (cc == 6) (void)xhci_recover_ep0(dev);
    else dev->ep0_failed = 1;
    return 0;
  }
  return 1;
}

static int xhci_ep0_get_config_and_set_config(xhci_device_state_t *dev) {
  uint8_t *buf = (uint8_t *)PMM_AllocatePage();
  if (!buf) return 0;
  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;

  uint64_t setup9 = 0;
  setup9 |= 0x80ULL;
  setup9 |= 0x06ULL << 8;
  setup9 |= 0x0200ULL << 16;
  setup9 |= 0x0000ULL << 32;
  setup9 |= 9ULL << 48;

  if (!xhci_ep0_control_in(dev, setup9, buf, 9) || dev->control_length<9 || buf[0]!=9 || buf[1]!=2) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(Configuration 9) failed\n");
    if (!dev->ep0_failed) PMM_FreePage(buf);
    return 0;
  }

  uint16_t total_len = (uint16_t)(buf[2] | (buf[3] << 8));
  if (total_len < 9 || total_len > 4096) { PMM_FreePage(buf); return 0; }

  uint64_t setupFull = 0;
  setupFull |= 0x80ULL;
  setupFull |= 0x06ULL << 8;
  setupFull |= 0x0200ULL << 16;
  setupFull |= 0x0000ULL << 32;
  setupFull |= ((uint64_t)total_len) << 48;

  for (uint32_t i = 0; i < 4096; i++)
    buf[i] = 0;
  if (!xhci_ep0_control_in(dev, setupFull, buf, total_len) || dev->control_length < total_len) {
    serial_print("[xHCI] EP0 GET_DESCRIPTOR(Configuration full) failed\n");
    if (!dev->ep0_failed) PMM_FreePage(buf);
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
  dev->intr_interval = 0; dev->intr_burst=0; dev->intr_mult=0; dev->intr_esit=0;

  dev->kbd_prev_mod = 0;
  for (uint32_t i = 0; i < 6; i++) {
    dev->kbd_prev_keys[i] = 0;
  }

  uint8_t cur_ifnum = 0xFF;
  uint8_t cur_ifclass = 0;
  uint8_t cur_ifsub = 0;
  uint8_t cur_ifproto = 0, cur_alt=0;
  uint16_t cur_report_len=0; uint8_t last_ep=0;

  uint32_t off = 0;
  while (off + 2 <= total_len) {
    uint8_t bLength = buf[off + 0];
    uint8_t bType = buf[off + 1];
    if (bLength < 2 || off + bLength > total_len) {
      serial_print("[xHCI] Malformed configuration descriptor\n");
      PMM_FreePage(buf); return 0;
    }

    if (bType == 4 && bLength >= 9) {
      uint8_t ifnum = buf[off + 2];
      uint8_t alt = buf[off + 3];
      uint8_t nendp = buf[off + 4];
      uint8_t cls = buf[off + 5];
      uint8_t sub = buf[off + 6];
      uint8_t proto = buf[off + 7];

      cur_ifnum = ifnum; cur_alt=alt; cur_report_len=0; last_ep=0;
      cur_ifclass = cls;
      cur_ifsub = sub;
      cur_ifproto = proto;

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
        cur_report_len = rep_len;
      }
    } else if (bType == 5 && bLength >= 7) {
      uint8_t epaddr = buf[off + 2];
      uint8_t attr = buf[off + 3];
      uint16_t mps = (uint16_t)(buf[off + 4] | (buf[off + 5] << 8));
      uint8_t interval = buf[off + 6];

      last_ep=epaddr;
      uint8_t xfertype = attr & 0x3u;
      uint8_t dir_in = (epaddr & 0x80u) ? 1u : 0u;
      if (dev->intr_epaddr == 0 && cur_ifclass == 3 &&
          (cur_ifsub == 0 || cur_ifsub == 1) && cur_alt==0 && dir_in && (epaddr&15) && xfertype == 3) {
        dev->intr_epaddr = epaddr;
        uint8_t epnum = (uint8_t)(epaddr & 0x0Fu);
        dev->intr_dci = (uint8_t)(2u * (uint32_t)epnum + 1u);
        dev->hid_ifnum=cur_ifnum; dev->hid_subclass=cur_ifsub; dev->hid_proto=cur_ifproto;
        dev->hid_report_desc_len=cur_report_len;
        dev->intr_mps = mps & 0x7ff;
        if (dev->speed_code==3) dev->intr_burst=(mps>>11)&3;
        dev->intr_esit=dev->intr_mps*(dev->intr_burst+1);
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

    if (bType==0x30 && bLength>=6 && last_ep==dev->intr_epaddr && dev->speed_code>=4) {
      dev->intr_burst=buf[off+2];
      dev->intr_esit=buf[off+4] | (buf[off+5]<<8);
    }
    off += bLength;
  }

  if (cfg_value == 0) { PMM_FreePage(buf); return 0; }
  uint64_t setcfg = 0;
  setcfg |= 0x00ULL;
  setcfg |= 0x09ULL << 8;
  setcfg |= ((uint64_t)cfg_value) << 16;
  setcfg |= 0x0000ULL << 32;
  setcfg |= 0ULL << 48;

  if (!xhci_ep0_control_no_data_out(dev, setcfg)) {
    serial_print("[xHCI] EP0 SET_CONFIGURATION failed\n");
    if (!dev->ep0_failed) PMM_FreePage(buf);
    return 0;
  }

  serial_print("[USB] SET_CONFIGURATION done\n");
  PMM_FreePage(buf);
  return 1;
}

uint64_t xhci_last_command;
uint8_t xhci_running;
static uint64_t tsc_per_ms = 1000000;
static inline uint64_t read_tsc(void) {
  uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi<<32) | lo;
}
static inline void timer_out(uint16_t port, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port)); }
static inline uint8_t timer_in(uint16_t port) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static void calibrate_timeout_clock(void) {
  /* PIT channel 2, 10ms one-shot. Does not require IRQs or alter IRQ0. */
  uint8_t saved = timer_in(0x61);
  timer_out(0x61, saved & ~3u);
  timer_out(0x43, 0xb0); timer_out(0x42, 11932 & 255); timer_out(0x42, 11932 >> 8);
  uint64_t start = read_tsc(); timer_out(0x61, (saved & ~2u) | 1u);
  uint32_t spins = 0;
  while (!(timer_in(0x61) & 0x20) && ++spins < 10000000) __asm__ volatile("pause");
  uint64_t elapsed = read_tsc() - start; timer_out(0x61, saved);
  if (spins < 10000000 && elapsed >= 10000) tsc_per_ms = elapsed / 10;
  else serial_print("[xHCI] Timeout clock calibration failed; bounded fallback\n");
}
uint64_t xhci_now_ms(void) { return read_tsc() / tsc_per_ms; }
int xhci_deadline(uint64_t start, uint32_t ms, uint32_t spins) {
  return xhci_now_ms() - start >= ms || spins >= 100000000;
}
void xhci_delay_ms(uint32_t ms) {
  uint64_t start = xhci_now_ms(); uint32_t spins = 0;
  while (!xhci_deadline(start, ms, spins++)) __asm__ volatile("pause");
}
void xhci_log_device(xhci_device_state_t *dev, const char *stage) {
  serial_print("[xHCI] "); serial_print(stage); serial_print(" port="); xhci_print_u32_dec(dev->port_id);
  serial_print(" slot="); xhci_print_u32_dec(dev->slot_id);
  serial_print(" speed="); xhci_print_u32_dec(dev->speed_code);
}
void xhci_ring_enqueue(xhci_trb_t *ring, uint32_t *index, uint8_t *cycle, const xhci_trb_t *trb) {
  volatile xhci_trb_t *dst = &ring[*index];
  dst->data = trb->data; dst->status = trb->status;
  __asm__ volatile("sfence" ::: "memory");
  dst->control = (trb->control & ~1u) | *cycle;
  if (++*index == 255) {
    uint32_t chain = (trb_type(trb->control) == TRB_TYPE_SETUP_STAGE || trb_type(trb->control) == TRB_TYPE_DATA_STAGE) ? (1u<<4) : 0;
    __asm__ volatile("sfence" ::: "memory");
    ring[255].control = make_trb_control(TRB_TYPE_LINK, *cycle) | (1u<<1) | chain;
    *index = 0; *cycle ^= 1;
  }
}
void xhci_cmd_ring_push(const xhci_trb_t *trb) {
  xhci_last_command = (uint64_t)&command_ring[command_ring_index];
  xhci_ring_enqueue(command_ring, &command_ring_index, &command_ring_cycle, trb);
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
  __asm__ volatile("sfence" ::: "memory");
  doorbell_regs[slot_id] = dci;
  (void)doorbell_regs[slot_id];
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
static const uint32_t PORTSC_PP = 1u << 9; // Port Power
static const uint32_t PORTSC_SPEED_MASK = 0xFu << 10;
// RW1C bits (write 1 to clear).
static const uint32_t PORTSC_WPR = 1u << 31; // Warm Port Reset (USB3)

static uint32_t xhci_port_speed(uint32_t portsc) {
  return (portsc & PORTSC_SPEED_MASK) >> 10;
}

static uint32_t xhci_port_pls(uint32_t portsc) { return (portsc >> 5) & 0xFu; }

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

static int xhci_port_ready(uint32_t ps) {
  return (ps & PORTSC_CCS) && (ps & PORTSC_PED) &&
         (xhci_port_speed(ps) != 0) && (xhci_port_pls(ps) == 0);
}

static uint8_t port_major[256], port_slot_type[256], port_speed_class[256][16];
static uint8_t port_pending[256];
static uint32_t port_slots[256], num_ports;
static uint8_t servicing_ports;
static int wait_reg(volatile uint32_t *reg, uint32_t mask, uint32_t wanted, uint32_t ms) {
  uint64_t start = xhci_now_ms(); uint32_t spins = 0;
  do {
    if ((*reg & mask) == wanted) return 1;
    __asm__ volatile("pause");
  } while (!xhci_deadline(start, ms, spins++));
  serial_print("[xHCI] Register timeout mask="); xhci_print_hex32(mask);
  serial_print(" value="); xhci_print_hex32(*reg); serial_print("\n"); return 0;
}
static void xhci_port_power_on(volatile uint32_t *reg, uint32_t *ps) {
  if (!(*ps & PORTSC_PP)) *reg = xhci_port_write_value(*ps, PORTSC_PP);
  *ps = *reg;
}
static void xhci_port_clear_w1c(volatile uint32_t *reg, uint32_t *ps) {
  *reg = xhci_port_write_value(*ps, *ps & XHCI_PORT_CHANGES);
  *ps = *reg;
}
static int xhci_force_port_ready(volatile uint32_t *reg, uint32_t port, uint32_t *speed) {
  uint32_t ps = *reg;
  xhci_port_power_on(reg, &ps);
  if (!(ps & PORTSC_CCS)) return 0;
  /* USB2 attach debounce (USB2 7.1.7.3): require 100ms continuous connection. */
  uint64_t stable = xhci_now_ms(), start = stable; uint32_t spins = 0;
  while (xhci_now_ms() - stable < 100) {
    ps = *reg;
    if (!(ps & PORTSC_CCS)) return 0;
    if (ps & (1u<<17)) { xhci_port_clear_w1c(reg, &ps); stable = xhci_now_ms(); }
    if (xhci_deadline(start, 1000, spins++)) return 0;
  }
  for (uint32_t attempt = 0; attempt < 2; attempt++) {
    ps = *reg;
    if (!(ps & PORTSC_CCS) || (ps & (1u<<3))) return 0;
    uint8_t usb3 = port_major[port] >= 3 || (!port_major[port] && xhci_port_speed(ps) >= 4);
    if (!usb3 || !xhci_port_ready(ps)) {
      uint32_t action = usb3 ? PORTSC_WPR : PORTSC_PR;
      xhci_log_portsc(port, usb3 ? "[xHCI] USB3 warm reset" : "[xHCI] USB2 reset", ps);
      /* Never write PED back as 1: it DISABLES a USB2 port. */
      *reg = xhci_port_write_value(ps, action);
      if (!wait_reg(reg, action, 0, 1000)) continue;
    }
    uint64_t start_ready = xhci_now_ms(); uint32_t ready_spins = 0;
    while (!xhci_deadline(start_ready, 1000, ready_spins++)) {
      ps = *reg;
      if (!(ps & PORTSC_CCS)) return 0;
      if (xhci_port_ready(ps)) {
        *speed = xhci_port_speed(ps);
        xhci_port_clear_w1c(reg, &ps);
        if (!usb3) xhci_delay_ms(10); /* USB2 reset recovery, 7.1.7.5. */
        xhci_log_portsc(port, "[xHCI] Port ready", *reg);
        return xhci_port_ready(*reg);
      }
      __asm__ volatile("pause");
    }
    xhci_log_portsc(port, "[xHCI] Reset retry", *reg);
  }
  xhci_log_portsc(port, "[xHCI] Reset failed", *reg); return 0;
}
static void read_supported_protocols(void) {
  uint32_t off = ((cap_regs->hcc_params1 >> 16) & 0xffff) * 4;
  for (uint32_t n = 0; off && n < 256 && off + 16 < XHCI_MMIO_MAP_SIZE; n++) {
    volatile uint32_t *ext = (volatile uint32_t *)((uint64_t)cap_regs + off);
    uint32_t header = ext[0], next = (header >> 8) & 255;
    if ((header & 255) == 2) {
      uint32_t ports = ext[2], first = ports & 255, count = (ports >> 8) & 255;
      uint32_t major = header >> 24, psic = ports >> 28;
      if (off + 16 + psic * 4 >= XHCI_MMIO_MAP_SIZE) break;
      for (uint32_t p = first; p && p < first + count && p <= num_ports; p++) {
        port_major[p] = major; port_slot_type[p] = ext[3] & 31;
        if (!psic) {
          for (uint32_t id = 1; id <= 5; id++) port_speed_class[p][id] = id;
        } else for (uint32_t i = 0; i < psic; i++) {
          uint32_t psi = ext[4+i], id = psi & 15, exp = (psi>>4)&3;
          uint64_t rate = psi>>16; while (exp--) rate *= 1000;
          port_speed_class[p][id] = rate <= 1500000 ? 2 : (rate <= 12000000 ? 1 : (rate <= 480000000 ? 3 : (rate <= 5000000000ull ? 4 : 5)));
        }
      }
      serial_print("[xHCI] Supported protocol USB"); xhci_print_u32_dec(major);
      serial_print(" first_port="); xhci_print_u32_dec(first); serial_print(" count="); xhci_print_u32_dec(count);
      serial_print(" slot_type="); xhci_print_u32_dec(ext[3]&31); serial_print("\n");
    }
    if (!next) break;
    off += next*4;
  }
}

static int xhci_bios_handoff(void) {
  // Walk xHCI extended capabilities to find USB Legacy Support (Cap ID = 1)
  // HCCPARAMS1 bits 31:16 give xECP (offset in 4-byte units).
  uint32_t hcc1 = cap_regs->hcc_params1;
  uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
  if (xecp == 0)
    return 1;

  uint32_t off = xecp * 4;
  for (uint32_t n = 0; off && n < 256 && off + 8 < XHCI_MMIO_MAP_SIZE; n++) {
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
      uint64_t start = xhci_now_ms();
      for (uint32_t spins = 0; !xhci_deadline(start, 1000, spins); spins++) {
        uint32_t r = ext[0];
        if ((r & (1u << 16)) == 0) {
          serial_print("[xHCI] BIOS ownership released\n");
          ext[1] = (ext[1] & 0xffff0000u);
          return 1;
        }
      }
      serial_print("[xHCI] BIOS ownership timeout; controller left untouched\n");
      return 0;
    }

    if (next == 0)
      break;
    off += (uint32_t)next * 4;
  }
  return 1;
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

static int xhci_cmd_enable_slot(uint32_t port, uint32_t *out_slot_id) {
  serial_print("[xHCI] Enable Slot port="); xhci_print_u32_dec(port); serial_print("\n");
  xhci_trb_t cmd;
  cmd.data = 0;
  cmd.status = 0;
  cmd.control = make_trb_control(TRB_TYPE_ENABLE_SLOT_CMD, command_ring_cycle) | ((uint32_t)port_slot_type[port]<<16);

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
  if (!ring) return NULL;
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

xhci_device_state_t g_devs[256];

static void xhci_ring_doorbell_ep0(uint32_t slot_id) {
  // Doorbell target is DCI. For EP0, DCI=1.
  doorbell_regs[slot_id] = 1;
}

static void xhci_ep0_ring_push(xhci_device_state_t *dev,
                               const xhci_trb_t *trb) {
  xhci_ring_enqueue(dev->ep0_ring, &dev->ep0_index, &dev->ep0_cycle, trb);
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
  if (!input_ctx) return 0;
  for (uint32_t i = 0; i < 4096; i++)
    input_ctx[i] = 0;

  xhci_input_control_ctx_t *icc = (xhci_input_control_ctx_t *)input_ctx;
  icc->add_flags = (1u << 1);
  icc->drop_flags = 0;

  xhci_ep_ctx_32_t *ep0 = (xhci_ep_ctx_32_t *)(input_ctx + (2 * g_ctx_size));
  *ep0 = *(xhci_ep_ctx_32_t *)(dev->dev_ctx + g_ctx_size);
  ep0->dword1 = (ep0->dword1 & 0xffffu) | (new_mps << 16);

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

  PMM_FreePage(input_ctx);
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
  uint8_t *buf = PMM_AllocatePage(); int ok=0;
  if (!buf) return 0;
  for (uint32_t i=0;i<4096;i++) buf[i]=0;
  if (!xhci_ep0_control_in(dev,0x0008000001000680ull,buf,8) || dev->control_length<8) goto out;
  uint32_t mps=buf[7];
  if (dev->speed_code>=4) mps = mps==9 ? 512 : 0;
  if ((dev->speed_code==1 && mps!=8 && mps!=16 && mps!=32 && mps!=64) ||
      (dev->speed_code==2 && mps!=8) || (dev->speed_code==3 && mps!=64) || !mps) {
    xhci_log_device(dev,"invalid descriptor EP0 MPS"); serial_print("\n"); goto out;
  }
  if (mps!=dev->ep0_mps && !xhci_cmd_evaluate_ctx_update_ep0_mps(dev,mps)) goto out;
  if (!xhci_ep0_control_in(dev,0x0012000001000680ull,buf,18) || dev->control_length<18 || buf[0]!=18 || buf[1]!=1) goto out;
  xhci_log_device(dev,"Device descriptor"); serial_print(" VID:PID=");
  xhci_print_hex16(buf[8] | (buf[9]<<8)); serial_print(":"); xhci_print_hex16(buf[10] | (buf[11]<<8));
  serial_print(" EP0_MPS="); xhci_print_u32_dec(dev->ep0_mps); serial_print(" class="); xhci_print_u32_dec(buf[4]); serial_print("\n"); ok=1;
out:
  /* A timed-out transfer may still own its buffer: quarantine until disabled. */
  if (!dev->ep0_failed) PMM_FreePage(buf);
  return ok;
}

static int xhci_cmd_address_device(uint32_t slot_id, uint32_t port_id,
                                   uint32_t speed_code) {
  // Allocate Device Context and EP0 transfer ring
  uint8_t *dev_ctx = (uint8_t *)PMM_AllocatePage();
  if (!dev_ctx) return 0;
  for (uint32_t i = 0; i < 4096; i++)
    dev_ctx[i] = 0;

  xhci_trb_t *ep0_ring = xhci_alloc_tr_ring();
  if (!ep0_ring) { PMM_FreePage(dev_ctx); return 0; }
  g_devs[slot_id].slot_id=slot_id; g_devs[slot_id].port_id=port_id;
  g_devs[slot_id].dev_ctx=dev_ctx; g_devs[slot_id].ep0_ring=ep0_ring;

  // Write DCBAA entry for this slot
  dcbaa[slot_id] = (uint64_t)(uintptr_t)dev_ctx;

  // Allocate Input Context (must hold Input Control + Slot + EP0)
  uint8_t *input_ctx = (uint8_t *)PMM_AllocatePage();
  if (!input_ctx) return 0;
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

  serial_print("[xHCI] Address context port="); xhci_print_u32_dec(port_id);
  serial_print(" slot="); xhci_print_u32_dec(slot_id); serial_print(" speed_id="); xhci_print_u32_dec(speed_code); serial_print("\n");
  // EP0 Context at index 2
  xhci_ep_ctx_32_t *ep0 = (xhci_ep_ctx_32_t *)(input_ctx + (2 * g_ctx_size));

  uint32_t speed_class = port_speed_class[port_id][speed_code];
  if (!port_major[port_id]) speed_class = speed_code;
  uint32_t mps = xhci_initial_mps(speed_class);

  ep0->dword1 = ((mps & 0xFFFFu) << 16) | ((4u & 0x7u) << 3) | (3u << 1);

  uint64_t trdp = (uint64_t)(uintptr_t)ep0_ring;
  trdp &= ~0xFULL;
  trdp |= 1u; // DCS = 1
  ep0->tr_deq_lo = (uint32_t)(trdp & 0xFFFFFFFFu);
  ep0->tr_deq_hi = (uint32_t)((trdp >> 32) & 0xFFFFFFFFu);

  ep0->dword0 = 0;
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

  PMM_FreePage(input_ctx);
  if (cc != 1 || evt_slot != slot_id)
    return 0;

  // Save device state for EP0 transfers (per-slot)
  xhci_device_state_t *dev = &g_devs[slot_id];
  dev->slot_id = slot_id;
  dev->port_id = port_id;
  dev->reports = 0; dev->ep0_failed = 0; dev->intr_failed = 0; dev->intr_pending = 0;
  dev->ep0_mps = mps;
  dev->ep0_ring = ep0_ring;
  dev->ep0_index = 0;
  dev->ep0_cycle = 1;
  dev->dev_ctx = dev_ctx;
  dev->speed_code = (uint8_t)speed_class;
  dev->intr_buf = NULL;
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
    return 0;
  }

  if (!xhci_ep0_get_config_and_set_config(dev)) {
    serial_print("[xHCI] Config descriptor / set config failed\n");
    return 0;
  }

  // Basic HID bring-up: try parsing report descriptor first, then choose protocol.
  if (dev->hid_ifnum != 0xFF) {
    xhci_hid_try_parse_mouse_report_desc(dev);

    if (dev->hid_subclass == 1) {
      if (dev->hid_proto == 1) {
        // Keyboard: we only support boot protocol for now.
        serial_print("[HID] SET_PROTOCOL(boot) for keyboard\n");
        if (!xhci_hid_set_protocol_boot(dev)) return 0;
      } else if (dev->hid_proto == 2) {
        // Mouse: only use boot protocol if we couldn't parse the report descriptor.
        if (!dev->hid_mouse_valid) {
          serial_print("[HID] SET_PROTOCOL(boot) for mouse (fallback)\n");
          if (!xhci_hid_set_protocol_boot(dev)) return 0;
        } else {
          serial_print("[HID] Using report protocol for mouse\n");
        }
      }
    }

    serial_print("[HID] SET_IDLE\n");
    if (!xhci_hid_set_idle(dev)) serial_print("[HID] SET_IDLE unsupported or failed\n");
    if (dev->ep0_failed) return 0;

    if (xhci_cmd_configure_intr_in_ep(dev)) {
      serial_print("[HID] Interrupt IN armed\n");
      xhci_hid_start_polling(dev);
    } else return 0;
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
                PAGE_WRITE | PAGE_PRESENT | (1ull<<3) | (1ull<<4));
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
  calibrate_timeout_clock();
  if (!xhci_bios_handoff()) return;

  // Context size (HCCPARAMS1.CSZ): 0=32B contexts, 1=64B contexts
  g_ctx_size = (cap_regs->hcc_params1 & (1u << 2)) ? 64u : 32u;
  serial_print("[xHCI] Context size: ");
  xhci_print_u32_dec(g_ctx_size);
  serial_print("\n");

  // 2) Reset controller
  serial_print("[xHCI] Resetting Controller...\n");
  op_regs->usb_cmd &= ~USB_CMD_RS;
  serial_print("[xHCI] Waiting for halt...\n");
  if (!wait_reg((volatile uint32_t *)((uint64_t)op_regs + 4), USB_STS_HCH, USB_STS_HCH, 1000)) return;

  serial_print("[xHCI] Issuing Reset...\n");
  op_regs->usb_cmd |= USB_CMD_HCRST;
  if (!wait_reg((volatile uint32_t *)((uint64_t)op_regs), USB_CMD_HCRST, 0, 1000)) return;
  serial_print("[xHCI] Waiting for CNR...\n");
  if (!wait_reg((volatile uint32_t *)((uint64_t)op_regs + 4), 1u<<11, 0, 1000)) return;

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
  if (!dcbaa) return;
  for (int i = 0; i < 512; i++)
    dcbaa[i] = 0;

  // Max Scratchpad Buffers = (MaxScratchpadBuffersHi << 5) |
  // MaxScratchpadBuffersLo
  uint32_t hcs2 = cap_regs->hcs_params2;
  uint32_t max_sp_lo = (hcs2 >> 27) & 0x1F;
  uint32_t max_sp_hi = (hcs2 >> 21) & 0x1F;
  uint32_t scratchpad_count = (max_sp_hi << 5) | max_sp_lo;

  if (scratchpad_count > 512) { serial_print("[xHCI] Scratchpad array exceeds one page\n"); return; }
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
    if (!sp_array) return;
    for (uint32_t i = 0; i < 512; i++)
      sp_array[i] = 0;

    for (uint32_t i = 0; i < scratchpad_count; i++) {
      void *sp_buf = PMM_AllocatePage();
      if (!sp_buf) return;
      sp_array[i] = (uint64_t)(uintptr_t)sp_buf;
    }

    dcbaa[0] = (uint64_t)(uintptr_t)sp_array;
  }

  op_regs->dcbaap = (uint64_t)(uintptr_t)dcbaa;

  // 5) Command Ring: 256 TRBs with last TRB as Link TRB
  command_ring = (xhci_trb_t *)PMM_AllocatePage();
  if (!command_ring) return;
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
  if (!event_ring) return;
  for (int i = 0; i < XHCI_EVT_RING_TRBS; i++) {
    event_ring[i].data = 0;
    event_ring[i].status = 0;
    event_ring[i].control = 0;
  }

  erst = (xhci_erst_entry_t *)PMM_AllocatePage();
  if (!erst) return;
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
  runtime_regs->interrupters[0].iman = 1u; /* clear IP, IE=0: no IRQ handler installed */
  op_regs->usb_cmd = (op_regs->usb_cmd & ~USB_CMD_INTE) | USB_CMD_RS;
  if (!wait_reg((volatile uint32_t *)((uint64_t)op_regs + 4), USB_STS_HCH, 0, 1000)) return;
  xhci_running = 1;
  serial_print("[xHCI] Controller Running\n");

  // Prime ERDP
  xhci_event_ring_update_erdp();

  // 8) Ports info
  num_ports = (cap_regs->hcs_params1 >> 24) & 0xFF;
  serial_print("[xHCI] Number of ports: ");
  {
    char s[4];
    s[0] = (num_ports / 10) + '0';
    s[1] = (num_ports % 10) + '0';
    s[2] = 0;
    serial_print(s);
  }
  serial_print("\n");

  read_supported_protocols();
  for (uint32_t p = 1; p <= num_ports; p++) {
    volatile uint32_t *reg = (volatile uint32_t *)((uint64_t)op_regs + 0x400 + (p-1)*16);
    uint32_t ps = *reg; xhci_port_power_on(reg, &ps); port_pending[p] = 1;
  }
  xhci_service_ports();
}

int xhci_recover_ep0(xhci_device_state_t *dev) {
  xhci_log_device(dev, "recover stalled EP0"); serial_print("\n");
  xhci_trb_t cmd = {0, 0, (14u<<10) | (1u<<16) | (dev->slot_id<<24)};
  uint32_t slot, cc;
  xhci_cmd_ring_push(&cmd); xhci_ring_doorbell_cmd();
  if (!xhci_wait_for_command_completion(&slot, &cc) || cc != 1) { dev->ep0_failed=1; return 0; }
  cmd = (xhci_trb_t){(uint64_t)&dev->ep0_ring[dev->ep0_index] | dev->ep0_cycle, 0, (16u<<10) | (1u<<16) | (dev->slot_id<<24)};
  xhci_cmd_ring_push(&cmd); xhci_ring_doorbell_cmd();
  if (!xhci_wait_for_command_completion(&slot, &cc) || cc != 1) { dev->ep0_failed=1; return 0; }
  return 1;
}
static void disable_slot(uint32_t slot) {
  if (!slot || !xhci_running) return;
  xhci_trb_t cmd = {0,0,(10u<<10) | (slot<<24)}; uint32_t returned, cc;
  xhci_cmd_ring_push(&cmd); xhci_ring_doorbell_cmd();
  if (!xhci_wait_for_command_completion(&returned,&cc) || cc != 1) return;
  xhci_device_state_t *dev = &g_devs[slot];
  /* Disable Slot completion guarantees controller no longer accesses these. */
  if (dev->dev_ctx) PMM_FreePage(dev->dev_ctx);
  if (dev->ep0_ring) PMM_FreePage(dev->ep0_ring);
  if (dev->intr_ring) PMM_FreePage(dev->intr_ring);
  if (dev->intr_buf) PMM_FreePage(dev->intr_buf);
  dcbaa[slot] = 0;
  for (uint32_t i=0;i<sizeof(*dev);i++) ((uint8_t *)dev)[i]=0;
}
void xhci_note_port_event(uint32_t port) {
  if (port && port <= num_ports) port_pending[port] = 1;
}
void xhci_service_ports(void) {
  if (!xhci_running || servicing_ports) return;
  servicing_ports = 1;
  for (uint32_t p = 1; p <= num_ports && xhci_running; p++) {
    if (!port_pending[p]) continue;
    port_pending[p] = 0;
    volatile uint32_t *reg = (volatile uint32_t *)((uint64_t)op_regs + 0x400 + (p-1)*16);
    uint32_t ps = *reg;
    xhci_log_portsc(p, "[xHCI] Port status", ps);
    if (port_slots[p]) {
      if (!(ps & PORTSC_CCS) || (ps & (1u<<17)) || !(ps & PORTSC_PED)) {
        disable_slot(port_slots[p]); port_slots[p] = 0;
      } else { xhci_port_clear_w1c(reg, &ps); continue; }
    }
    if (!(ps & PORTSC_CCS)) { xhci_port_clear_w1c(reg, &ps); continue; }
    uint32_t speed=0, slot=0;
    if (!xhci_force_port_ready(reg,p,&speed)) continue;
    if (!port_speed_class[p][speed] && port_major[p]) {
      serial_print("[xHCI] Unsupported protocol speed ID\n"); continue;
    }
    if (!xhci_cmd_enable_slot(p,&slot)) continue;
    if (!xhci_cmd_address_device(slot,p,speed)) {
      serial_print("[xHCI] Enumeration aborted, disabling slot\n"); disable_slot(slot); continue;
    }
    port_slots[p] = slot;
    xhci_log_device(&g_devs[slot], "enumeration complete"); serial_print("\n");
  }
  servicing_ports = 0;
}
