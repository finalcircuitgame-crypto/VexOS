#include "xhci_internal.h"
#include "pmm.h"

uint32_t xhci_hid_get_bits_u32(const uint8_t *buf, uint32_t bitoff,
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

int32_t xhci_hid_get_bits_s32(const uint8_t *buf, uint32_t bitoff,
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
  typedef struct {
    uint32_t id;
    uint8_t used;
    uint16_t buttons_bitoff;
    uint16_t buttons_bits;
    uint16_t x_bitoff;
    uint8_t x_bits;
    uint8_t x_relative;
    uint16_t y_bitoff;
    uint8_t y_bits;
    uint8_t y_relative;
    uint16_t wheel_bitoff;
    uint8_t wheel_bits;
    uint8_t wheel_relative;
  } mouse_report_t;

  typedef struct {
    uint32_t id;
    uint32_t bitpos;
    uint8_t used;
  } input_report_position_t;

  typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
  } hid_global_state_t;

  mouse_report_t reports[16] = {{0}};
  input_report_position_t positions[16] = {{0}};
  hid_global_state_t global_stack[8];
  uint32_t global_depth = 0;

  uint32_t usage_page = 0;
  uint32_t report_size = 0;
  uint32_t report_count = 0;
  uint32_t report_id = 0;
  uint32_t usages[16];
  uint32_t usage_n = 0;
  uint32_t usage_min = 0;
  uint32_t usage_max = 0;
  uint8_t has_range = 0;
  uint8_t has_report_id = 0;

  /* Track the full collection nesting; a later vendor collection is not a mouse. */
  uint8_t mouse_collection_stack[32] = {0};
  uint32_t collection_depth = 0;

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
    uint8_t prefix = d[i++];
    if (prefix == 0xFEu) {
      if (i + 2u > len)
        break;
      uint8_t long_size = d[i];
      i += 2u;
      if (i + long_size > len)
        break;
      i += long_size;
      continue;
    }

    uint8_t size_code = prefix & 0x3u;
    uint8_t item_type = (prefix >> 2) & 0x3u;
    uint8_t tag = (prefix >> 4) & 0xFu;
    uint32_t item_size = size_code == 3u ? 4u : (uint32_t)size_code;
    if (i + item_size > len)
      break;

    uint32_t value = 0;
    for (uint32_t k = 0; k < item_size; ++k)
      value |= (uint32_t)d[i + k] << (8u * k);
    i += item_size;

    if (item_type == 1u) {
      /* Global items. Report positions remain separated by Report ID. */
      if (tag == 0x0u) {
        usage_page = value;
      } else if (tag == 0x7u) {
        report_size = value;
      } else if (tag == 0x8u) {
        if (value == 0u || value > 255u)
          break;
        report_id = value;
        has_report_id = 1u;
      } else if (tag == 0x9u) {
        report_count = value;
      } else if (tag == 0xAu) {
        if (global_depth >= 8u)
          break;
        global_stack[global_depth].usage_page = usage_page;
        global_stack[global_depth].report_size = report_size;
        global_stack[global_depth].report_count = report_count;
        global_stack[global_depth].report_id = report_id;
        ++global_depth;
      } else if (tag == 0xBu) {
        if (global_depth != 0u) {
          --global_depth;
          usage_page = global_stack[global_depth].usage_page;
          report_size = global_stack[global_depth].report_size;
          report_count = global_stack[global_depth].report_count;
          report_id = global_stack[global_depth].report_id;
        }
      }
      continue;
    }

    if (item_type == 2u) {
      /* Local items are consumed by the next Main item. */
      if (tag == 0x0u) {
        if (usage_n < 16u)
          usages[usage_n++] = value;
      } else if (tag == 0x1u) {
        usage_min = value;
        has_range = 1u;
      } else if (tag == 0x2u) {
        usage_max = value;
        has_range = 1u;
      }
      continue;
    }

    if (item_type != 0u)
      continue;

    if (tag == 0xAu) {
      uint32_t collection_usage = usage_n ? usages[0] : 0u;
      uint8_t parent_is_mouse =
          collection_depth != 0u
              ? mouse_collection_stack[collection_depth - 1u]
              : 0u;
      uint8_t is_mouse_application =
          ((value & 0xFFu) == 1u && usage_page == 0x01u &&
           collection_usage == 0x02u) ? 1u : 0u;

      if (collection_depth >= 32u)
        break;
      mouse_collection_stack[collection_depth++] =
          (uint8_t)(parent_is_mouse || is_mouse_application);
      usage_n = 0u;
      has_range = 0u;
      continue;
    }

    if (tag == 0xCu) {
      if (collection_depth != 0u)
        --collection_depth;
      usage_n = 0u;
      has_range = 0u;
      continue;
    }

    /*
     * Only Input items contribute to an input report's bit offsets.
     * Output and Feature reports have independent layouts, even when their
     * Report ID matches an input report.
     */
    if (tag == 0x8u) {
      uint32_t count = report_count;
      uint32_t size_bits = report_size;
      uint64_t total_bits = (uint64_t)count * (uint64_t)size_bits;
      if (count == 0u || size_bits == 0u || total_bits > 65535u) {
        usage_n = 0u;
        has_range = 0u;
        continue;
      }

      int32_t position_index = -1;
      for (uint32_t n = 0; n < 16u; ++n) {
        if (positions[n].used && positions[n].id == report_id) {
          position_index = (int32_t)n;
          break;
        }
      }
      if (position_index < 0) {
        for (uint32_t n = 0; n < 16u; ++n) {
          if (!positions[n].used) {
            positions[n].used = 1u;
            positions[n].id = report_id;
            positions[n].bitpos = 0u;
            position_index = (int32_t)n;
            break;
          }
        }
      }

      if (position_index >= 0) {
        uint32_t base_bit = positions[(uint32_t)position_index].bitpos;
        uint8_t in_mouse_collection =
            collection_depth != 0u
                ? mouse_collection_stack[collection_depth - 1u]
                : 0u;
        uint8_t is_constant = (value & 0x1u) ? 1u : 0u;

        if (in_mouse_collection && !is_constant) {
          int32_t report_index = -1;
          for (uint32_t n = 0; n < 16u; ++n) {
            if (reports[n].used && reports[n].id == report_id) {
              report_index = (int32_t)n;
              break;
            }
          }
          if (report_index < 0) {
            for (uint32_t n = 0; n < 16u; ++n) {
              if (!reports[n].used) {
                reports[n].used = 1u;
                reports[n].id = report_id;
                report_index = (int32_t)n;
                break;
              }
            }
          }

          if (report_index >= 0) {
            mouse_report_t *report = &reports[(uint32_t)report_index];
            uint8_t is_relative = (value & (1u << 2)) ? 1u : 0u;

            if (usage_page == 0x09u && report->buttons_bits == 0u &&
                base_bit <= 65535u) {
              uint32_t button_bits = (uint32_t)total_bits;
              if (button_bits > 32u)
                button_bits = 32u;
              report->buttons_bitoff = (uint16_t)base_bit;
              report->buttons_bits = (uint16_t)button_bits;
            }

            if (size_bits <= 32u) {
              for (uint32_t field = 0; field < count; ++field) {
                uint32_t field_usage = 0u;
                uint32_t field_usage_page = usage_page;

                if (has_range) {
                  uint64_t ranged_usage = (uint64_t)usage_min + field;
                  if (ranged_usage <= usage_max)
                    field_usage = (uint32_t)ranged_usage;
                } else if (field < usage_n) {
                  field_usage = usages[field];
                }

                /* Four-byte Usage items may carry their own Usage Page. */
                if (field_usage > 0xFFFFu) {
                  field_usage_page = field_usage >> 16;
                  field_usage &= 0xFFFFu;
                }

                uint64_t field_bit = (uint64_t)base_bit +
                                     (uint64_t)field * size_bits;
                if (field_bit > 65535u)
                  continue;

                if (field_usage_page == 0x01u) {
                  if (field_usage == 0x30u && report->x_bits == 0u) {
                    report->x_bitoff = (uint16_t)field_bit;
                    report->x_bits = (uint8_t)size_bits;
                    report->x_relative = is_relative;
                  } else if (field_usage == 0x31u && report->y_bits == 0u) {
                    report->y_bitoff = (uint16_t)field_bit;
                    report->y_bits = (uint8_t)size_bits;
                    report->y_relative = is_relative;
                  } else if (field_usage == 0x38u &&
                             report->wheel_bits == 0u) {
                    report->wheel_bitoff = (uint16_t)field_bit;
                    report->wheel_bits = (uint8_t)size_bits;
                    report->wheel_relative = is_relative;
                  }
                }
              }
            }
          }
        }

        positions[(uint32_t)position_index].bitpos =
            base_bit + (uint32_t)total_bits;
      }

      usage_n = 0u;
      has_range = 0u;
      continue;
    }

    /* Output and Feature items reset local state but do not move Input offsets. */
    if (tag == 0x9u || tag == 0xBu) {
      usage_n = 0u;
      has_range = 0u;
    }
  }

  int32_t selected = -1;
  uint32_t selected_score = 0u;
  for (uint32_t n = 0; n < 16u; ++n) {
    if (!reports[n].used || reports[n].x_bits == 0u ||
        reports[n].y_bits == 0u)
      continue;

    uint32_t score = 1u;
    if (reports[n].buttons_bits)
      score += 4u;
    if (reports[n].wheel_bits)
      score += 2u;
    if (reports[n].x_relative && reports[n].y_relative)
      score += 1u;
    if (selected < 0 || score > selected_score) {
      selected = (int32_t)n;
      selected_score = score;
    }
  }

  if (selected >= 0) {
    const mouse_report_t *report = &reports[(uint32_t)selected];
    dev->hid_has_report_id = has_report_id;
    dev->hid_mouse_report_id = (uint8_t)report->id;
    dev->hid_buttons_bitoff = report->buttons_bitoff;
    dev->hid_buttons_bits = report->buttons_bits;
    dev->hid_x_bitoff = report->x_bitoff;
    dev->hid_x_bits = report->x_bits;
    dev->hid_x_is_relative = report->x_relative;
    dev->hid_y_bitoff = report->y_bitoff;
    dev->hid_y_bits = report->y_bits;
    dev->hid_y_is_relative = report->y_relative;
    dev->hid_wheel_bitoff = report->wheel_bitoff;
    dev->hid_wheel_bits = report->wheel_bits;
    dev->hid_wheel_is_relative = report->wheel_relative;
    dev->hid_mouse_valid = 1u;
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
    PMM_FreePage(buf);
    return;
  }

  xhci_hid_parse_report_desc_mouse(dev, buf, dev->hid_report_desc_len);
  PMM_FreePage(buf);
}

static void xhci_intr_ring_push(xhci_device_state_t *dev, const xhci_trb_t *trb) {
  dev->intr_ring[dev->intr_index] = *trb;
  dev->intr_ring[dev->intr_index].control &= ~1u;
  dev->intr_ring[dev->intr_index].control |= (uint32_t)(dev->intr_cycle & 1u);

  dev->intr_index++;
  if (dev->intr_index == 255) {
    // Publish completed entries before exposing the Link TRB again.
    xhci_dma_write_barrier();
    dev->intr_ring[255].control ^= 1u;
    if (dev->intr_ring[255].control & (1u << 1))
      dev->intr_cycle ^= 1u;
    dev->intr_index = 0;
  }

  // xhci_hid_start_polling() rings the endpoint doorbell on return.
  xhci_dma_write_barrier();
}


/*
 * Convert USB endpoint descriptor bInterval into the xHCI Endpoint Context
 * Interval exponent. xHCI expresses periodic intervals as 125 us * 2^N.
 *
 * Low/full-speed interrupt bInterval is in 1 ms frames (1..255), so convert
 * 8 * bInterval microframes and round down to the nearest power of two.
 * High/SuperSpeed periodic bInterval is already an exponent (1..16), so
 * the xHCI field receives bInterval - 1.
 */
static uint32_t xhci_hid_context_interval(const xhci_device_state_t *dev) {
  uint32_t b_interval = dev->intr_interval;
  if (b_interval == 0)
    b_interval = 1;

  if (dev->speed_code == 1u || dev->speed_code == 2u) {
    uint32_t microframes = b_interval * 8u;
    uint32_t interval = 0;

    while (interval < 10u && (1u << (interval + 1u)) <= microframes)
      interval++;

    if (interval < 3u)
      interval = 3u;
    if (interval > 10u)
      interval = 10u;
    return interval;
  }

  if (b_interval > 16u)
    b_interval = 16u;
  return b_interval - 1u;
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

  /*
   * Interrupt IN endpoint: CErr=3 (bits 2:1), EP Type=7.
   * Dword 1 bit 0 is reserved and must remain clear.
   */
  ep->dword1 = ((uint32_t)(dev->intr_mps & 0xFFFFu) << 16) |
               ((7u & 0x7u) << 3) | (3u << 1);

  uint64_t trdp = (uint64_t)(uintptr_t)dev->intr_ring;
  trdp &= ~0xFULL;
  trdp |= 1u;
  ep->tr_deq_lo = (uint32_t)(trdp & 0xFFFFFFFFu);
  ep->tr_deq_hi = (uint32_t)((trdp >> 32) & 0xFFFFFFFFu);

  uint32_t interval = xhci_hid_context_interval(dev);
  ep->dword0 = (interval & 0xFFu) << 16;
  /*
   * This driver's interrupt TDs request one full endpoint packet. Match the
   * Average TRB Length to that actual transfer size; 8 is the special
   * control-endpoint value and under-reports larger HID packets (e.g. M65's
   * 64-byte interrupt-IN endpoint).
   */
  uint32_t avg_trb_len = (uint32_t)(dev->intr_mps & 0xFFFFu);
  ep->dword4 = avg_trb_len | (avg_trb_len << 16);

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
  /* HID SET_PROTOCOL: wValue=1 selects the standardized Boot Protocol. */
  setup |= 0x0001ULL << 16;
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
  /* Keep HID idle reporting disabled; relative report semantics vary
     by device, and this driver relies on change reports for all HID input. */
  setup |= 0x0000ULL << 16;
  setup |= ((uint64_t)dev->hid_ifnum) << 32;
  setup |= 0ULL << 48;
  return xhci_ep0_control_no_data_out(dev, setup);
}

void xhci_hid_start_polling(xhci_device_state_t *dev) {
  static uint32_t rearm_log_count;
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

  if (rearm_log_count < 16u) {
    serial_print("[HID][REARM] slot=");
    xhci_print_u32_dec(dev->slot_id);
    serial_print(" ep=");
    xhci_print_u32_dec(dci);
    serial_print(" mps=");
    xhci_print_u32_dec(dev->intr_mps);
    serial_print("\n");
  }
  rearm_log_count++;
}
