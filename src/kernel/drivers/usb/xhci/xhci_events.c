#include "xhci_internal.h"
#include "input.h"

extern void desktop_debug_set_usb_status(const char *status);

static uint32_t g_xhci_mouse_report_log_count;
static uint32_t g_xhci_transfer_event_log_count;

/* Shared by the polling loop and synchronous command/control waiters. */
static void xhci_handle_transfer_event(const xhci_trb_t *event);

static int xhci_kbd_has_key(const uint8_t keys[6], uint8_t k) {
  if (!k)
    return 1;
  for (uint32_t i = 0; i < 6; i++) {
    if (keys[i] == k)
      return 1;
  }
  return 0;
}

static void xhci_debug_append_u32(char *out, uint32_t capacity,
                                      uint32_t *used, uint32_t value) {
  char digits[11];
  uint32_t count = 0;
  do {
    digits[count++] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value && count < sizeof(digits));
  while (count && *used + 1u < capacity)
    out[(*used)++] = digits[--count];
  out[*used] = '\0';
}

static void xhci_debug_append_hex8(char *out, uint32_t capacity,
                                   uint32_t *used, uint8_t value) {
  static const char hex[] = "0123456789ABCDEF";
  if (*used + 2u >= capacity)
    return;
  out[(*used)++] = hex[(value >> 4) & 0x0Fu];
  out[(*used)++] = hex[value & 0x0Fu];
  out[*used] = '\0';
}

static void xhci_debug_mouse_report(const xhci_device_state_t *dev,
                                    uint32_t completion_code,
                                    uint32_t transfer_length) {
  char line[160] = "USB mouse report: slot=";
  uint32_t used = 23u;
  xhci_debug_append_u32(line, sizeof(line), &used, dev->slot_id);
  const char *ep_text = " ep=";
  while (*ep_text && used + 1u < sizeof(line))
    line[used++] = *ep_text++;
  line[used] = '\0';
  xhci_debug_append_u32(line, sizeof(line), &used, dev->intr_dci);
  const char *cc_text = " cc=";
  while (*cc_text && used + 1u < sizeof(line))
    line[used++] = *cc_text++;
  line[used] = '\0';
  xhci_debug_append_u32(line, sizeof(line), &used, completion_code);
  const char *len_text = " len=";
  while (*len_text && used + 1u < sizeof(line))
    line[used++] = *len_text++;
  line[used] = '\0';
  xhci_debug_append_u32(line, sizeof(line), &used, transfer_length);
  const char *data_text = " data=";
  while (*data_text && used + 1u < sizeof(line))
    line[used++] = *data_text++;
  line[used] = '\0';

  uint32_t shown = dev->intr_buf
      ? (transfer_length < 4u ? transfer_length : 4u) : 0u;
  for (uint32_t i = 0; i < shown; ++i) {
    if (i) {
      if (used + 1u >= sizeof(line))
        break;
      line[used++] = ' ';
      line[used] = '\0';
    }
    xhci_debug_append_hex8(line, sizeof(line), &used, dev->intr_buf[i]);
  }

  input_pointer_state_t pointer = input_pointer_get();
  const char *pos_text = " pos=";
  while (*pos_text && used + 1u < sizeof(line))
    line[used++] = *pos_text++;
  line[used] = '\0';
  xhci_debug_append_u32(line, sizeof(line), &used,
                        (uint32_t)(pointer.x < 0 ? 0 : pointer.x));
  if (used + 1u < sizeof(line)) {
    line[used++] = ',';
    line[used] = '\0';
  }
  xhci_debug_append_u32(line, sizeof(line), &used,
                        (uint32_t)(pointer.y < 0 ? 0 : pointer.y));

  /*
   * Keep USB diagnostics independent from the PS/2 touchpad status. Also
   * mirror a bounded sample to COM1/NetLog so the receiver can prove whether
   * interrupt reports are actually arriving after enumeration.
   */
  if (g_xhci_mouse_report_log_count < 24u) {
    serial_print("[HID][REPORT] ");
    serial_print(line);
    serial_print("\n");
  }
  g_xhci_mouse_report_log_count++;
  desktop_debug_set_usb_status(line);
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
  if (!event_ring || !runtime_regs) {
    return;
  }

  uint64_t next = (uint64_t)(uintptr_t)&event_ring[event_ring_index];
  runtime_regs->interrupters[0].erdp = next | (1ULL << 3);
}

int xhci_poll_event(xhci_trb_t *out) {
  if (!event_ring || !runtime_regs || !out) {
    return 0;
  }

  /*
   * Event TRBs are written by the controller. Read the cycle bit first:
   * xHCI publishes the rest of the event before handing ownership over by
   * changing Cycle. Do not copy stale fields before checking that handoff.
   */
  volatile xhci_trb_t *entry = (volatile xhci_trb_t *)&event_ring[event_ring_index];
  uint32_t control = entry->control;
  if ((control & 1u) != (uint32_t)event_ring_cycle) {
    return 0;
  }

  /* Acquire the controller's DMA writes before consuming the event payload. */
  xhci_dma_read_barrier();
  out->data = entry->data;
  out->status = entry->status;
  out->control = control;

  event_ring_index++;
  if (event_ring_index >= 256) {
    event_ring_index = 0;
    event_ring_cycle ^= 1;
  }

  xhci_event_ring_update_erdp();
  return 1;
}

/*
 * The event ring has one global dequeue index. The desktop poll loop disables
 * interrupts around consume+dispatch; synchronous EP0/command waiters must do
 * the same, otherwise a PIT preemption can let a second consumer overtake the
 * first event before its HID completion has been handled.
 */
static int xhci_poll_event_for_waiter(xhci_trb_t *out) {
  uint64_t flags;
  __asm__ volatile("pushfq; cli; popq %0" : "=r"(flags) :: "memory");

  int received = xhci_poll_event(out);
  if (received && trb_type(out->control) == TRB_TYPE_TRANSFER_EVENT)
    xhci_handle_transfer_event(out);

  if (flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");
  return received;
}

int xhci_wait_for_command_completion(uint32_t *out_slot_id, uint32_t *out_cc) {
  /* Commands normally complete quickly; keep a bounded 100 ms failure path. */
  uint64_t start = xhci_time_now_ticks();
  uint64_t deadline = xhci_time_deadline_us(100000u);
  while (!xhci_time_expired(deadline)) {
    xhci_service_background();
    xhci_trb_t evt;
    if (xhci_poll_event_for_waiter(&evt)) {
      if (trb_type(evt.control) == TRB_TYPE_COMMAND_COMPLETION) {
        uint32_t slot = trb_slot_id(evt.control);
        uint32_t cc = trb_cc(evt.status);
        if (out_slot_id)
          *out_slot_id = slot;
        if (out_cc)
          *out_cc = cc;

        serial_print("[xHCI][CMD] completion slot=");
        xhci_print_u32_dec(slot);
        serial_print(" cc=");
        xhci_print_u32_dec(cc);
        serial_print(" status=");
        xhci_print_hex32(evt.status);
        serial_print(" control=");
        xhci_print_hex32(evt.control);
        serial_print("\n");
        xhci_time_log_elapsed("command_completion", start,
                              cc == 1u ? "success" : "device-error");
        return 1;
      }
    } else {
      __asm__ volatile("pause");
    }
  }

  serial_print("[xHCI][CMD] TIMEOUT waiting for command completion");
  if (op_regs) {
    serial_print(" USBSTS=");
    xhci_print_hex32(op_regs->usb_sts);
    serial_print(" USBCMD=");
    xhci_print_hex32(op_regs->usb_cmd);
  }
  if (runtime_regs) {
    serial_print(" IMAN=");
    xhci_print_hex32(runtime_regs->interrupters[0].iman);
    serial_print(" ERDP=");
    xhci_print_hex32((uint32_t)runtime_regs->interrupters[0].erdp);
  }
  serial_print(" event_index=");
  xhci_print_u32_dec(event_ring_index);
  serial_print(" cycle=");
  xhci_print_u32_dec(event_ring_cycle);
  serial_print("\n");
  xhci_time_log_elapsed("command_completion", start, "timeout");
  return 0;
}

int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc) {
  /* Slow/absent USB devices must not stall boot for an unbounded spin budget. */
  uint64_t start = xhci_time_now_ticks();
  uint64_t deadline = xhci_time_deadline_us(250000u);
  while (!xhci_time_expired(deadline)) {
    xhci_service_background();
    xhci_trb_t evt;
    if (xhci_poll_event(&evt)) {
      const uint32_t event_type = trb_type(evt.control);
      const uint32_t event_slot = trb_slot_id(evt.control);
      const uint32_t event_dci = (evt.control >> 16) & 0x1Fu;
      if (event_type == TRB_TYPE_TRANSFER_EVENT &&
          event_slot == slot_id && event_dci == 1u) {
        uint32_t cc = trb_cc(evt.status);
        if (out_cc)
          *out_cc = cc;

        serial_print("[xHCI][XFER] completion slot=");
        xhci_print_u32_dec(slot_id);
        serial_print(" cc=");
        xhci_print_u32_dec(cc);
        serial_print(" status=");
        xhci_print_hex32(evt.status);
        serial_print(" control=");
        xhci_print_hex32(evt.control);
        serial_print("\n");
        xhci_time_log_elapsed("transfer_completion", start,
                              (cc == 1u || cc == 13u) ? "success" : "device-error");
        return 1;
      }
    } else {
      __asm__ volatile("pause");
    }
  }

  serial_print("[xHCI][XFER] TIMEOUT slot=");
  xhci_print_u32_dec(slot_id);
  if (op_regs) {
    serial_print(" USBSTS=");
    xhci_print_hex32(op_regs->usb_sts);
  }
  if (runtime_regs) {
    serial_print(" IMAN=");
    xhci_print_hex32(runtime_regs->interrupters[0].iman);
    serial_print(" ERDP=");
    xhci_print_hex32((uint32_t)runtime_regs->interrupters[0].erdp);
  }
  serial_print(" event_index=");
  xhci_print_u32_dec(event_ring_index);
  serial_print(" cycle=");
  xhci_print_u32_dec(event_ring_cycle);
  serial_print("\n");
  xhci_time_log_elapsed("transfer_completion", start, "timeout");
  return 0;
}

static void xhci_handle_transfer_event(const xhci_trb_t *event) {
  if (!event || trb_type(event->control) != TRB_TYPE_TRANSFER_EVENT)
    return;

  uint32_t cc = trb_cc(event->status);
  uint32_t slot_id = trb_slot_id(event->control);
  uint32_t ep_id = (event->control >> 16) & 0x1Fu;

  /* Log completions even when a synchronous waiter services the shared ring. */
  if (g_xhci_transfer_event_log_count < 32u) {
    serial_print("[xHCI][EVENT] transfer slot=");
    xhci_print_u32_dec(slot_id);
    serial_print(" ep=");
    xhci_print_u32_dec(ep_id);
    serial_print(" cc=");
    xhci_print_u32_dec(cc);
    serial_print(" status=");
    xhci_print_hex32(event->status);
    serial_print(" control=");
    xhci_print_hex32(event->control);
    serial_print("\n");
  }
  g_xhci_transfer_event_log_count++;

  if (slot_id == 0u || slot_id >= 256u)
    return;

  xhci_device_state_t *dev = &g_devs[slot_id];
  if (dev->slot_id != slot_id || dev->intr_dci == 0u ||
      ep_id != (uint32_t)dev->intr_dci)
    return;

  /* Acquire the report written by the controller before reading its bytes. */
  xhci_dma_read_barrier();

  uint32_t remaining = event->status & 0xFFFFFFu;
  uint32_t xfer_len = dev->intr_mps;
  if (dev->intr_mps && remaining <= (uint32_t)dev->intr_mps)
    xfer_len = (uint32_t)dev->intr_mps - remaining;

  if ((cc == 1u || cc == 13u) && dev->intr_buf) {
    if (dev->hid_mouse_valid) {
      const uint8_t *report = dev->intr_buf;
      uint32_t base_bit = 0u;
      if (dev->hid_has_report_id) {
        if (xfer_len < 1u)
          goto rearm;
        if (report[0] != dev->hid_mouse_report_id)
          goto rearm;
        base_bit = 8u;
      }

      uint32_t buttons = 0u;
      if (dev->hid_buttons_bits) {
        buttons = xhci_hid_get_bits_u32(
            report, (uint32_t)dev->hid_buttons_bitoff + base_bit,
            (uint32_t)dev->hid_buttons_bits);
      }

      int32_t dx = 0;
      int32_t dy = 0;
      int32_t wheel = 0;
      if (dev->hid_x_bits) {
        uint32_t off = (uint32_t)dev->hid_x_bitoff + base_bit;
        uint32_t bits = (uint32_t)dev->hid_x_bits;
        dx = dev->hid_x_is_relative
            ? xhci_hid_get_bits_s32(report, off, bits)
            : (int32_t)xhci_hid_get_bits_u32(report, off, bits);
      }
      if (dev->hid_y_bits) {
        uint32_t off = (uint32_t)dev->hid_y_bitoff + base_bit;
        uint32_t bits = (uint32_t)dev->hid_y_bits;
        dy = dev->hid_y_is_relative
            ? xhci_hid_get_bits_s32(report, off, bits)
            : (int32_t)xhci_hid_get_bits_u32(report, off, bits);
      }
      if (dev->hid_wheel_bits) {
        uint32_t off = (uint32_t)dev->hid_wheel_bitoff + base_bit;
        uint32_t bits = (uint32_t)dev->hid_wheel_bits;
        wheel = dev->hid_wheel_is_relative
            ? xhci_hid_get_bits_s32(report, off, bits)
            : (int32_t)xhci_hid_get_bits_u32(report, off, bits);
      }

      if (dev->hid_x_is_relative && dev->hid_y_is_relative) {
        input_pointer_add_rel(dx, dy, buttons);
      } else {
        input_pointer_set_abs_scaled(dx, dy, 0x7FFF, 0x7FFF, buttons);
      }
      (void)wheel;
    } else if (dev->hid_proto == 2u && dev->hid_boot_protocol) {
      /* Standard HID boot mouse report: buttons, signed X, signed Y, wheel. */
      uint8_t buttons = dev->intr_buf[0];
      int8_t dx = (int8_t)dev->intr_buf[1];
      int8_t dy = (int8_t)dev->intr_buf[2];
      int8_t wheel = 0;
      if (xfer_len >= 4u)
        wheel = (int8_t)dev->intr_buf[3];

      input_pointer_add_rel((int32_t)dx, (int32_t)dy, (uint32_t)buttons);
      (void)wheel;
    } else if (dev->hid_proto == 1u && xfer_len >= 8u) {
      const uint8_t *report = dev->intr_buf;
      uint8_t mod = report[0];
      uint8_t shift =
          (uint8_t)(((mod & 0x02u) || (mod & 0x20u)) ? 1u : 0u);
      const uint8_t *keys = &report[2];

      for (uint32_t i = 0; i < 6u; ++i) {
        uint8_t key = keys[i];
        if (key && !xhci_kbd_has_key(dev->kbd_prev_keys, key)) {
          input_kbd_push_event(key, 1);
          char ch = xhci_kbd_keycode_to_ascii(key, shift);
          if (ch)
            input_kbd_push_char(ch);
        }
      }
      for (uint32_t i = 0; i < 6u; ++i) {
        uint8_t key = dev->kbd_prev_keys[i];
        if (key && !xhci_kbd_has_key(keys, key))
          input_kbd_push_event(key, 0);
      }
      for (uint32_t bit = 0; bit < 8u; ++bit) {
        uint8_t mask = (uint8_t)(1u << bit);
        if ((mod & mask) != (dev->kbd_prev_mod & mask))
          input_kbd_push_event((uint8_t)(0xE0u + bit),
                               (mod & mask) != 0);
      }

      dev->kbd_prev_mod = mod;
      for (uint32_t i = 0; i < 6u; ++i)
        dev->kbd_prev_keys[i] = keys[i];
    }
  }

rearm:
  if (dev->hid_mouse_valid ||
      (dev->hid_proto == 2u && dev->hid_boot_protocol))
    xhci_debug_mouse_report(dev, cc, xfer_len);

  if (dev->intr_ring && dev->intr_mps)
    xhci_hid_start_polling(dev);
}

static void xhci_poll_events_impl(void) {
  if (!event_ring || !runtime_regs)
    return;

  /* Bound polling work while dispatching every consumed transfer completion. */
  for (uint32_t processed = 0; processed < 64u; ++processed) {
    xhci_trb_t event;
    if (!xhci_poll_event(&event))
      break;
    xhci_handle_transfer_event(&event);
  }
}

/*
 * The event ring has one global dequeue index and cycle state. Polling is
 * reached from the desktop loop, input syscalls, and the main loop; make the
 * operation atomic against the PIT scheduler at the common entry point so
 * callers cannot race and advance the event-ring consumer twice.
 *
 * Preserve the caller's IF state: syscall/interrupt contexts already have
 * interrupts disabled, while ordinary desktop code must restore them.
 */
void xhci_poll_events(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; cli; popq %0" : "=r"(flags) :: "memory");

  xhci_poll_events_impl();

  if (flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");
}
