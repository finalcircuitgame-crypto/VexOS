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

int xhci_poll_event(xhci_trb_t *out) {
  xhci_trb_t trb = event_ring[event_ring_index];

  if ((trb.control & 1u) != (uint32_t)event_ring_cycle) {
    return 0;
  }

  *out = trb;

  event_ring_index++;
  if (event_ring_index >= 256) {
    event_ring_index = 0;
    event_ring_cycle ^= 1;
  }

  xhci_event_ring_update_erdp();
  return 1;
}

int xhci_wait_for_command_completion(uint32_t *out_slot_id, uint32_t *out_cc) {
  for (uint32_t spins = 0; spins < 5000000; spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt)) {
      continue;
    }

    if (trb_type(evt.control) == TRB_TYPE_COMMAND_COMPLETION) {
      if (out_slot_id) {
        *out_slot_id = trb_slot_id(evt.control);
      }
      if (out_cc) {
        *out_cc = trb_cc(evt.status);
      }
      return 1;
    }
  }
  return 0;
}

int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc) {
  uint32_t seen_other = 0;
  for (uint32_t spins = 0; spins < 50000000; spins++) {
    xhci_trb_t evt;
    if (!xhci_poll_event(&evt)) {
      continue;
    }

    if (trb_type(evt.control) == TRB_TYPE_TRANSFER_EVENT) {
      uint32_t cc = trb_cc(evt.status);
      uint32_t eslot = trb_slot_id(evt.control);
      if (eslot == slot_id) {
        if (out_cc) {
          *out_cc = cc;
        }
        return 1;
      }

      (void)seen_other;
    }
  }
  return 0;
}

void xhci_poll_events() {
  if (!event_ring) {
    return;
  }

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

    if (dev->intr_dci == 0) {
      continue;
    }

    if (ep_id != (uint32_t)dev->intr_dci) {
      continue;
    }

    uint32_t trb_len_remaining = evt.status & 0xFFFFFFu;
    uint32_t xfer_len = dev->intr_mps;
    if (dev->intr_mps && trb_len_remaining <= (uint32_t)dev->intr_mps) {
      xfer_len = (uint32_t)dev->intr_mps - trb_len_remaining;
    }

    if ((cc == 1 || cc == 13) && dev->intr_buf) {
      if (dev->hid_mouse_valid) {
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
            buttons = 0;
            for (uint32_t i = 0; i < (uint32_t)dev->hid_buttons_bits && i < 32;
                 i++) {
              uint32_t b = (uint32_t)dev->hid_buttons_bitoff + base_bit + i;
              uint32_t byte = b >> 3;
              uint32_t bit = b & 7u;
              uint32_t bitv = (uint32_t)((r[byte] >> bit) & 1u);
              buttons |= (bitv << i);
            }
          }

          int32_t dx = 0;
          int32_t dy = 0;
          int32_t wheel = 0;
          {
            uint32_t bits = (uint32_t)dev->hid_x_bits;
            uint32_t bitoff = (uint32_t)dev->hid_x_bitoff + base_bit;
            uint32_t u = 0;
            for (uint32_t i = 0; i < bits && i < 32; i++) {
              uint32_t b = bitoff + i;
              uint32_t byte = b >> 3;
              uint32_t bit = b & 7u;
              uint32_t bitv = (uint32_t)((r[byte] >> bit) & 1u);
              u |= (bitv << i);
            }
            if (bits && bits < 32) {
              uint32_t sign = 1u << (bits - 1);
              if (u & sign) {
                uint32_t mask = ~((1u << bits) - 1u);
                u |= mask;
              }
            }
            dx = (int32_t)u;
          }
          {
            uint32_t bits = (uint32_t)dev->hid_y_bits;
            uint32_t bitoff = (uint32_t)dev->hid_y_bitoff + base_bit;
            uint32_t u = 0;
            for (uint32_t i = 0; i < bits && i < 32; i++) {
              uint32_t b = bitoff + i;
              uint32_t byte = b >> 3;
              uint32_t bit = b & 7u;
              uint32_t bitv = (uint32_t)((r[byte] >> bit) & 1u);
              u |= (bitv << i);
            }
            if (bits && bits < 32) {
              uint32_t sign = 1u << (bits - 1);
              if (u & sign) {
                uint32_t mask = ~((1u << bits) - 1u);
                u |= mask;
              }
            }
            dy = (int32_t)u;
          }
          if (dev->hid_wheel_bits) {
            uint32_t bits = (uint32_t)dev->hid_wheel_bits;
            uint32_t bitoff = (uint32_t)dev->hid_wheel_bitoff + base_bit;
            uint32_t u = 0;
            for (uint32_t i = 0; i < bits && i < 32; i++) {
              uint32_t b = bitoff + i;
              uint32_t byte = b >> 3;
              uint32_t bit = b & 7u;
              uint32_t bitv = (uint32_t)((r[byte] >> bit) & 1u);
              u |= (bitv << i);
            }
            if (bits && bits < 32) {
              uint32_t sign = 1u << (bits - 1);
              if (u & sign) {
                uint32_t mask = ~((1u << bits) - 1u);
                u |= mask;
              }
            }
            wheel = (int32_t)u;
          }

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
      } else if (dev->hid_proto == 2) {
          uint8_t buttons = dev->intr_buf[0];
          int8_t dx = (int8_t)dev->intr_buf[1];
          int8_t dy = (int8_t)dev->intr_buf[2];
          int8_t wheel = 0;
          if (dev->intr_mps >= 4) {
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
}
