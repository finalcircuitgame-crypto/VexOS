#include "xhci_internal.h"
#include "input.h"

static int xhci_kbd_has_key(const uint8_t keys[6], uint8_t k) {
  if (!k)
    return 1;
  for (uint32_t i = 0; i < 6; i++) {
    if (keys[i] == k)
      return 1;
  }
  return 0;
}

static char xhci_kbd_keycode_to_ascii(uint8_t key, uint8_t shift) {
  if (key >= 0x04 && key <= 0x1D) {
    char c = (char)('a' + (key - 0x04));
    if (shift) {
      c = (char)('A' + (key - 0x04));
    }
    return c;
  }
  if (key >= 0x1E && key <= 0x27) {
    static const char k_no_shift[10] = {'1','2','3','4','5','6','7','8','9','0'};
    static const char k_shift[10] = {'!','@','#','$','%','^','&','*','(',')'};
    uint8_t idx = (uint8_t)(key - 0x1E);
    return shift ? k_shift[idx] : k_no_shift[idx];
  }
  switch (key) {
  case 0x28: return '\n';
  case 0x2A: return '\b';
  case 0x2B: return '\t';
  case 0x2C: return ' ';
  case 0x2D: return shift ? '_' : '-';
  case 0x2E: return shift ? '+' : '=';
  case 0x2F: return shift ? '{' : '[';
  case 0x30: return shift ? '}' : ']';
  case 0x31: return shift ? '|' : '\\';
  case 0x33: return shift ? ':' : ';';
  case 0x34: return shift ? '"' : '\'';
  case 0x35: return shift ? '~' : '`';
  case 0x36: return shift ? '<' : ',';
  case 0x37: return shift ? '>' : '.';
  case 0x38: return shift ? '?' : '/';
  default: return 0;
  }
}

void xhci_event_ring_update_erdp(void) {
  uint64_t next = (uint64_t)(uintptr_t)&event_ring[event_ring_index];
  runtime_regs->interrupters[0].erdp = next | (1ULL << 3);
}

static void dispatch_event(const xhci_trb_t *evt);
int xhci_poll_event(xhci_trb_t *out) {
  if (!event_ring || !runtime_regs) return 0;
  volatile xhci_trb_t *src = &event_ring[event_ring_index];
  uint32_t control = src->control;
  if ((control & 1u) != event_ring_cycle) return 0;
  __asm__ volatile("lfence" ::: "memory");
  out->control = control; out->data = src->data; out->status = src->status;
  if (++event_ring_index >= 256) { event_ring_index=0; event_ring_cycle^=1; }
  xhci_event_ring_update_erdp();
  runtime_regs->interrupters[0].iman = 1u; /* IP W1C, interrupts remain disabled */
  return 1;
}
int xhci_wait_for_command_completion(uint32_t *out_slot_id, uint32_t *out_cc) {
  uint64_t expected = xhci_last_command, start = xhci_now_ms();
  for (uint32_t spins=0; !xhci_deadline(start,1000,spins); spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt)) { __asm__ volatile("pause"); continue; }
    if (trb_type(evt.control)==TRB_TYPE_COMMAND_COMPLETION && (evt.data & ~15ull)==expected) {
      uint32_t cc=trb_cc(evt.status), slot=trb_slot_id(evt.control);
      serial_print("[xHCI] Command completion type=");
      xhci_print_u32_dec(trb_type(((xhci_trb_t *)expected)->control));
      serial_print(" slot="); xhci_print_u32_dec(slot);
      serial_print(" cc="); xhci_print_u32_dec(cc); serial_print("\n");
      if (out_slot_id) *out_slot_id=slot;
      if (out_cc) *out_cc=cc;
      return 1;
    }
    dispatch_event(&evt);
  }
  serial_print("[xHCI] Command timeout type=");
  xhci_print_u32_dec(trb_type(((xhci_trb_t *)expected)->control));
  serial_print("; controller stopped to prevent stale command reuse\n");
  op_regs->usb_cmd &= ~USB_CMD_RS;
  xhci_running=0;
  return 0;
}
int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc) {
  xhci_device_state_t *dev = &g_devs[slot_id]; uint64_t start=xhci_now_ms();
  for (uint32_t spins=0; !xhci_deadline(start,1000,spins); spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt)) { __asm__ volatile("pause"); continue; }
    uint32_t dci=(evt.control>>16)&31;
    uint64_t ptr=evt.data & ~15ull;
    if (trb_type(evt.control)==TRB_TYPE_TRANSFER_EVENT && trb_slot_id(evt.control)==slot_id && dci==1 &&
        (ptr==dev->ep0_setup_trb || ptr==dev->ep0_data_trb || ptr==dev->ep0_status_trb)) {
      uint32_t cc=trb_cc(evt.status), remaining=evt.status & 0xffffff;
      xhci_log_device(dev,"EP0 completion"); serial_print(" dci=1 cc="); xhci_print_u32_dec(cc);
      serial_print(" residual="); xhci_print_u32_dec(remaining); serial_print(" trb_low="); xhci_print_hex32((uint32_t)ptr); serial_print("\n");
      if (cc==13 && ptr==dev->ep0_data_trb) {
        if (remaining>dev->control_length) { if(out_cc)*out_cc=2; return 1; }
        dev->control_length-=remaining;
        continue; /* Short Data stage is not completion of the control request. */
      }
      if (cc!=1 || ptr==dev->ep0_status_trb) {
        if (out_cc) *out_cc=cc;
        return 1;
      }
      continue;
    }
    dispatch_event(&evt); /* Never lose HID input or port changes during enumeration. */
  }
  xhci_log_device(dev,"EP0 timeout"); serial_print(" dci=1\n");
  return 0;
}
void xhci_poll_events(void) {
  if (!xhci_running) return;
  xhci_trb_t evt;
  for (uint32_t budget=0; budget<256 && xhci_poll_event(&evt); budget++) dispatch_event(&evt);
  xhci_service_ports();
}
static void dispatch_event(const xhci_trb_t *event) {
  xhci_trb_t evt=*event;
  if (trb_type(evt.control)==TRB_TYPE_PORT_STATUS_CHANGE) {
    uint32_t port=(uint32_t)(evt.data>>24)&255;
    serial_print("[xHCI] Port Status Change Event port="); xhci_print_u32_dec(port); serial_print("\n");
    xhci_note_port_event(port); return;
  }
  if (trb_type(evt.control)!=TRB_TYPE_TRANSFER_EVENT) return;
    uint32_t cc = trb_cc(evt.status);
    uint32_t slot_id = trb_slot_id(evt.control);
    uint32_t ep_id = (evt.control >> 16) & 0x1Fu;

    if (slot_id == 0 || slot_id >= 256) {
      return;
    }

    xhci_device_state_t *dev = &g_devs[slot_id];
    if (dev->slot_id != slot_id) {
      return;
    }

    if (dev->intr_dci == 0) {
      return;
    }

    if (ep_id != (uint32_t)dev->intr_dci) {
      return;
    }

    uint32_t trb_len_remaining = evt.status & 0xFFFFFFu;
    uint32_t xfer_len = dev->intr_mps;
    if (dev->intr_mps && trb_len_remaining <= (uint32_t)dev->intr_mps) {
      xfer_len = (uint32_t)dev->intr_mps - trb_len_remaining;
    }

    dev->intr_pending = 0;
    if (cc != 1 && cc != 13) {
      xhci_log_device(dev,"HID endpoint stopped"); serial_print(" dci="); xhci_print_u32_dec(ep_id);
      serial_print(" cc="); xhci_print_u32_dec(cc); serial_print("\n");
      dev->intr_failed=1; return;
    }
    if (++dev->reports <= 4 || dev->reports % 256 == 0) {
      xhci_log_device(dev,"HID report"); serial_print(" dci="); xhci_print_u32_dec(ep_id);
      serial_print(" bytes="); xhci_print_u32_dec(xfer_len);
      serial_print(" count="); xhci_print_u32_dec(dev->reports); serial_print("\n");
    }
    if ((cc == 1 || cc == 13) && dev->intr_buf) {
      if (dev->hid_mouse_valid) {
          uint32_t base = dev->hid_has_report_id ? 8 : 0;
          if (base + dev->hid_x_bitoff + dev->hid_x_bits > xfer_len*8 ||
              base + dev->hid_y_bitoff + dev->hid_y_bits > xfer_len*8 ||
              base + dev->hid_buttons_bitoff + dev->hid_buttons_bits > xfer_len*8 ||
              base + dev->hid_wheel_bitoff + dev->hid_wheel_bits > xfer_len*8) goto rearm;
          const uint8_t *r = dev->intr_buf;
          uint32_t base_bit = 0;
          if (dev->hid_has_report_id) {
            if (xfer_len < 1) {
              goto rearm;
            }
            if (r[0] != dev->hid_mouse_report_id) {
              goto rearm;
            }
            base_bit = 8;
          }

          uint32_t buttons = 0;
          if (dev->hid_buttons_bits) {
            buttons = xhci_hid_get_bits_u32(r, (uint32_t)dev->hid_buttons_bitoff + base_bit, (uint32_t)dev->hid_buttons_bits);
          }

          int32_t dx = 0;
          int32_t dy = 0;
          int32_t wheel = 0;
          if (dev->hid_x_bits) {
            uint32_t off = (uint32_t)dev->hid_x_bitoff + base_bit;
            uint32_t sz = (uint32_t)dev->hid_x_bits;
            dx = dev->hid_x_is_relative ? xhci_hid_get_bits_s32(r, off, sz) : (int32_t)xhci_hid_get_bits_u32(r, off, sz);
          }
          if (dev->hid_y_bits) {
            uint32_t off = (uint32_t)dev->hid_y_bitoff + base_bit;
            uint32_t sz = (uint32_t)dev->hid_y_bits;
            dy = dev->hid_y_is_relative ? xhci_hid_get_bits_s32(r, off, sz) : (int32_t)xhci_hid_get_bits_u32(r, off, sz);
          }
          if (dev->hid_wheel_bits) {
            uint32_t off = (uint32_t)dev->hid_wheel_bitoff + base_bit;
            uint32_t sz = (uint32_t)dev->hid_wheel_bits;
            wheel = dev->hid_wheel_is_relative ? xhci_hid_get_bits_s32(r, off, sz) : (int32_t)xhci_hid_get_bits_u32(r, off, sz);
          }

          (void)wheel;
          if (dev->hid_x_is_relative && dev->hid_y_is_relative) {
            input_pointer_add_rel(dx, dy, buttons);
          } else {
            int32_t x = dx;
            int32_t y = dy;
            int32_t ddx = 0;
            int32_t ddy = 0;
            if (dev->hid_abs_have_last) {
              ddx = x - dev->hid_abs_last_x;
              ddy = y - dev->hid_abs_last_y;
            }
            dev->hid_abs_last_x = x;
            dev->hid_abs_last_y = y;
            dev->hid_abs_have_last = 1;

            // QEMU usb-tablet typically uses 16-bit logical coords; map to screen.
            input_pointer_set_abs_scaled(x, y, 0x7FFF, 0x7FFF, buttons);

            (void)ddx;
            (void)ddy;
          }
      } else if (dev->hid_proto == 2 && xfer_len >= 3) {
          uint8_t buttons = dev->intr_buf[0];
          int8_t dx = (int8_t)dev->intr_buf[1];
          int8_t dy = (int8_t)dev->intr_buf[2];
          int8_t wheel = 0;
          if (xfer_len >= 4) {
            wheel = (int8_t)dev->intr_buf[3];
          }

          input_pointer_add_rel((int32_t)dx, (int32_t)dy, (uint32_t)buttons);

          (void)wheel;
      } else if (dev->hid_proto == 1) {
          if (xfer_len >= 8) {
            const uint8_t *r = dev->intr_buf;
            uint8_t mod = r[0];
            uint8_t shift = (uint8_t)(((mod & 0x02u) || (mod & 0x20u)) ? 1u : 0u);
            const uint8_t *keys = &r[2];

            for (uint32_t i = 0; i < 6; i++) {
              uint8_t k = keys[i];
              if (!k)
                continue;
              if (!xhci_kbd_has_key(dev->kbd_prev_keys, k)) {
                char c = xhci_kbd_keycode_to_ascii(k, shift);
                if (c) {
                  input_kbd_push_char(c);
                }
              }
            }

            dev->kbd_prev_mod = mod;
            for (uint32_t i = 0; i < 6; i++) {
              dev->kbd_prev_keys[i] = keys[i];
            }
          }
      }
    }

rearm:
    if (dev->intr_ring && dev->intr_mps) {
      xhci_hid_start_polling(dev);
    }
}
