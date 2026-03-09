#include "xhci_internal.h"
#include "pmm.h"

static uint32_t xhci_hid_get_bits_u32(const uint8_t *buf, uint32_t bitoff,
                                     uint32_t bits) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < bits && i < 32; i++) {
    uint32_t b = bitoff + i;
    uint32_t byte = b >> 3;
    uint32_t bit = b & 7u;
    uint32_t bitv = (uint32_t)((buf[byte] >> bit) & 1u);
    v |= (bitv << i);
  }
  return v;
}

static int32_t xhci_hid_get_bits_s32(const uint8_t *buf, uint32_t bitoff,
                                    uint32_t bits) {
  if (bits == 0 || bits > 32) {
    return 0;
  }
  uint32_t u = xhci_hid_get_bits_u32(buf, bitoff, bits);
  if (bits < 32) {
    uint32_t sign = 1u << (bits - 1);
    if (u & sign) {
      uint32_t mask = ~((1u << bits) - 1u);
      u |= mask;
    }
  }
  return (int32_t)u;
}

static void xhci_hid_parse_report_desc_mouse(xhci_device_state_t *dev,
                                             const uint8_t *d, uint32_t len) {
  uint32_t usage_page = 0;
  uint32_t report_size = 0;
  uint32_t report_count = 0;
  uint32_t bitpos = 0;

  uint32_t report_id = 0;
  uint8_t has_report_id = 0;

  uint32_t usages[16];
  uint32_t usage_n = 0;
  uint32_t usage_min = 0;
  uint32_t usage_max = 0;
  uint8_t has_range = 0;

  uint8_t in_mouse_app = 0;
  uint8_t in_mouse_phys = 0;
  uint8_t seen_mouse_usage = 0;

  uint8_t cur_is_relative = 0;

  dev->hid_mouse_valid = 0;
  dev->hid_has_report_id = 0;
  dev->hid_mouse_report_id = 0;
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

  for (uint32_t i = 0; i < len;) {
    uint8_t p = d[i++];
    if (p == 0xFE) {
      if (i + 2 > len)
        break;
      uint8_t sz = d[i];
      i += 2;
      if (i + sz > len)
        break;
      i += sz;
      continue;
    }

    uint8_t sz_code = p & 0x3u;
    uint8_t type = (p >> 2) & 0x3u;
    uint8_t tag = (p >> 4) & 0xFu;
    uint32_t sz = (sz_code == 3) ? 4u : (uint32_t)sz_code;
    if (i + sz > len)
      break;

    uint32_t v = 0;
    for (uint32_t k = 0; k < sz; k++) {
      v |= ((uint32_t)d[i + k]) << (8u * k);
    }
    i += sz;

    if (type == 1) {
      if (tag == 0x0) {
        usage_page = v;
      } else if (tag == 0x7) {
        report_size = v;
      } else if (tag == 0x9) {
        report_count = v;
      } else if (tag == 0x8) {
        report_id = v;
        has_report_id = 1;
        bitpos = 0;
      }
    } else if (type == 2) {
      if (tag == 0x0) {
        if (usage_n < 16) {
          usages[usage_n++] = v;
        }
      } else if (tag == 0x1) {
        usage_min = v;
        has_range = 1;
      } else if (tag == 0x2) {
        usage_max = v;
        has_range = 1;
      }
    } else if (type == 0) {
      if (tag == 0xA) {
        uint32_t u = 0;
        if (usage_n) {
          u = usages[0];
        }
        if (usage_page == 0x01 && u == 0x02) {
          seen_mouse_usage = 1;
        }
        if (seen_mouse_usage) {
          if (!in_mouse_app) {
            in_mouse_app = 1;
          } else {
            in_mouse_phys = 1;
          }
        }
        usage_n = 0;
        has_range = 0;
      } else if (tag == 0xC) {
        if (in_mouse_phys) {
          in_mouse_phys = 0;
        } else if (in_mouse_app) {
          in_mouse_app = 0;
        }
        usage_n = 0;
        has_range = 0;
      } else if (tag == 0x8 || tag == 0x9) {
        cur_is_relative = (uint8_t)((v & (1u << 2)) ? 1u : 0u);
        uint32_t count = report_count;
        uint32_t size_bits = report_size;
        if (count == 0 || size_bits == 0) {
          usage_n = 0;
          has_range = 0;
          continue;
        }

        uint8_t in_mouse = (in_mouse_app || in_mouse_phys) ? 1u : 0u;
        if (in_mouse) {
          for (uint32_t f = 0; f < count; f++) {
            uint32_t u = 0;
            if (has_range) {
              uint32_t uu = usage_min + f;
              if (uu <= usage_max) {
                u = uu;
              }
            } else if (f < usage_n) {
              u = usages[f];
            }

            if (usage_page == 0x09) {
              if (dev->hid_buttons_bits == 0) {
                dev->hid_buttons_bitoff = (uint16_t)(bitpos);
                dev->hid_buttons_bits = (uint16_t)(count * size_bits);
              }
              break;
            }

            if (usage_page == 0x01) {
              if (u == 0x30 && dev->hid_x_bits == 0) {
                dev->hid_x_bitoff = (uint16_t)(bitpos + (f * size_bits));
                dev->hid_x_bits = (uint8_t)size_bits;
                dev->hid_x_is_relative = cur_is_relative;
              } else if (u == 0x31 && dev->hid_y_bits == 0) {
                dev->hid_y_bitoff = (uint16_t)(bitpos + (f * size_bits));
                dev->hid_y_bits = (uint8_t)size_bits;
                dev->hid_y_is_relative = cur_is_relative;
              } else if (u == 0x38 && dev->hid_wheel_bits == 0) {
                dev->hid_wheel_bitoff = (uint16_t)(bitpos + (f * size_bits));
                dev->hid_wheel_bits = (uint8_t)size_bits;
                dev->hid_wheel_is_relative = cur_is_relative;
              }
            }
          }
        }

        bitpos += count * size_bits;
        usage_n = 0;
        has_range = 0;
      }
    }
  }

  if (dev->hid_x_bits && dev->hid_y_bits) {
    dev->hid_has_report_id = has_report_id ? 1u : 0u;
    dev->hid_mouse_report_id = (uint8_t)report_id;
    dev->hid_mouse_valid = 1;
  }
}

void xhci_hid_try_parse_mouse_report_desc(xhci_device_state_t *dev) {
  if (dev->hid_ifnum == 0xFF) {
    return;
  }

  if (dev->hid_report_desc_len == 0 || dev->hid_report_desc_len > 4096) {
    return;
  }

  uint8_t *buf = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++) {
    buf[i] = 0;
  }

  uint64_t setup = 0;
  setup |= 0x81ULL;
  setup |= 0x06ULL << 8;
  setup |= 0x2200ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= ((uint64_t)dev->hid_report_desc_len) << 48;

  if (!xhci_ep0_control_in(dev, setup, buf, dev->hid_report_desc_len)) {
    return;
  }

  xhci_hid_parse_report_desc_mouse(dev, buf, dev->hid_report_desc_len);
}

static void xhci_intr_ring_push(xhci_device_state_t *dev, const xhci_trb_t *trb) {
  dev->intr_ring[dev->intr_index] = *trb;
  dev->intr_ring[dev->intr_index].control &= ~1u;
  dev->intr_ring[dev->intr_index].control |= (uint32_t)(dev->intr_cycle & 1u);

  dev->intr_index++;
  if (dev->intr_index == 255) {
    dev->intr_index = 0;
    dev->intr_cycle ^= 1;
  }
}

int xhci_cmd_configure_intr_in_ep(xhci_device_state_t *dev) {
  if (dev->intr_epaddr == 0 || dev->intr_mps == 0) {
    serial_print("[xHCI] No interrupt IN endpoint found; skipping HID polling\n");
    return 0;
  }

  const uint32_t dci = (uint32_t)dev->intr_dci;
  if (dci == 0) {
    serial_print("[xHCI] Interrupt IN endpoint DCI is 0; skipping HID polling\n");
    return 0;
  }
  const uint32_t ctx_index = dci + 1;

  dev->intr_ring = xhci_alloc_tr_ring();
  dev->intr_index = 0;
  dev->intr_cycle = 1;

  uint8_t *input_ctx = (uint8_t *)PMM_AllocatePage();
  for (uint32_t i = 0; i < 4096; i++) {
    input_ctx[i] = 0;
  }

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

  xhci_input_control_ctx_t *icc = (xhci_input_control_ctx_t *)input_ctx;
  icc->add_flags = (1u << 0) | (1u << dci);
  icc->drop_flags = 0;

  xhci_slot_ctx_32_t *slot = (xhci_slot_ctx_32_t *)(input_ctx + g_ctx_size);
  if (dev->dev_ctx) {
    xhci_slot_ctx_32_t *cur = (xhci_slot_ctx_32_t *)(dev->dev_ctx + (0 * g_ctx_size));
    *slot = *cur;
  } else {
    slot->dword0 = 0;
    slot->dword1 = 0;
    slot->dword2 = 0;
    slot->dword3 = 0;
    for (int i = 0; i < 4; i++) {
      slot->rsvd[i] = 0;
    }
  }
  slot->dword0 &= ~(0x1Fu << 27);
  slot->dword0 |= ((dci & 0x1Fu) << 27);

  xhci_ep_ctx_32_t *ep =
      (xhci_ep_ctx_32_t *)(input_ctx + (ctx_index * g_ctx_size));

  ep->dword1 = ((uint32_t)(dev->intr_mps & 0xFFFFu) << 16) | (0u << 8) |
               ((7u & 0x7u) << 3) | 3u;

  uint64_t trdp = (uint64_t)(uintptr_t)dev->intr_ring;
  trdp &= ~0xFULL;
  trdp |= 1u;
  ep->tr_deq_lo = (uint32_t)(trdp & 0xFFFFFFFFu);
  ep->tr_deq_hi = (uint32_t)((trdp >> 32) & 0xFFFFFFFFu);

  uint32_t interval = dev->intr_interval;
  ep->dword0 = (interval & 0xFFu) << 16;
  ep->dword4 = (uint32_t)8u | ((uint32_t)(dev->intr_mps & 0xFFFFu) << 16);

  xhci_trb_t cmd;
  cmd.data = (uint64_t)(uintptr_t)input_ctx;
  cmd.status = 0;
  cmd.control = make_trb_control(TRB_TYPE_CONFIGURE_EP_CMD, command_ring_cycle) |
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

  return (cc == 1);
}

int xhci_hid_set_protocol_boot(xhci_device_state_t *dev) {
  if (dev->hid_ifnum == 0xFF) {
    return 0;
  }
  uint64_t setup = 0;
  setup |= 0x21ULL;
  setup |= 0x0BULL << 8;
  setup |= 0x0000ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= 0ULL << 48;

  return xhci_ep0_control_no_data_out(dev, setup);
}

int xhci_hid_set_idle(xhci_device_state_t *dev) {
  if (dev->hid_ifnum == 0xFF) {
    return 0;
  }
  uint64_t setup = 0;
  setup |= 0x21ULL;
  setup |= 0x0AULL << 8;
  setup |= 0x0000ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= 0ULL << 48;
  return xhci_ep0_control_no_data_out(dev, setup);
}

void xhci_hid_start_polling(xhci_device_state_t *dev) {
  if (!dev->intr_ring) {
    return;
  }

  if (!dev->intr_buf) {
    dev->intr_buf = (uint8_t *)PMM_AllocatePage();
    for (uint32_t i = 0; i < 4096; i++) {
      dev->intr_buf[i] = 0;
    }
  }

  const uint32_t dci = (uint32_t)dev->intr_dci;
  if (dci == 0) {
    return;
  }
  xhci_trb_t trb;
  trb.data = (uint64_t)(uintptr_t)dev->intr_buf;
  trb.status = (uint32_t)(dev->intr_mps & 0xFFFFu);
  trb.control = make_trb_control(TRB_TYPE_NORMAL, dev->intr_cycle) | (1u << 5) |
                (1u << 2);
  xhci_intr_ring_push(dev, &trb);
  xhci_ring_doorbell_ep(dev->slot_id, dci);
}
