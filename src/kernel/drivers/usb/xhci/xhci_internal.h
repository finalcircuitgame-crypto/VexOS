#ifndef XHCI_INTERNAL_H
#define XHCI_INTERNAL_H

#include "usb/xhci.h"
#include "xhci_rules.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations provided by the kernel.
void serial_print(const char *str);

// Shared xHCI MMIO pointers/state.
extern volatile xhci_cap_regs_t *cap_regs;
extern volatile xhci_op_regs_t *op_regs;
extern volatile xhci_runtime_regs_t *runtime_regs;
extern volatile uint32_t *doorbell_regs;

extern xhci_trb_t *command_ring;
extern uint32_t command_ring_index;
extern uint8_t command_ring_cycle;

extern xhci_trb_t *event_ring;
extern uint32_t event_ring_index;
extern uint8_t event_ring_cycle;

// Device state shared across modules.
typedef struct xhci_device_state {
  uint32_t slot_id;
  uint32_t port_id;
  uint64_t ep0_setup_trb, ep0_data_trb, ep0_status_trb;
  uint32_t control_length;
  uint32_t reports;
  uint8_t ep0_failed;
  uint8_t intr_failed;
  uint8_t intr_pending;
  uint8_t intr_burst;
  uint8_t intr_mult;
  uint32_t intr_esit;

  uint32_t ep0_mps;
  xhci_trb_t *ep0_ring;
  uint32_t ep0_index;
  uint8_t ep0_cycle;

  uint8_t *dev_ctx;

  uint8_t speed_code;

  uint8_t hid_ifnum;
  uint8_t hid_subclass;
  uint8_t hid_proto;
  uint16_t hid_report_desc_len;

  uint8_t hid_has_report_id;
  uint8_t hid_mouse_report_id;
  uint8_t hid_mouse_valid;
  uint16_t hid_buttons_bitoff;
  uint16_t hid_buttons_bits;
  uint16_t hid_x_bitoff;
  uint8_t hid_x_bits;
  uint8_t hid_x_is_relative;
  uint16_t hid_y_bitoff;
  uint8_t hid_y_bits;
  uint8_t hid_y_is_relative;
  uint16_t hid_wheel_bitoff;
  uint8_t hid_wheel_bits;
  uint8_t hid_wheel_is_relative;

  uint8_t hid_abs_have_last;
  int32_t hid_abs_last_x;
  int32_t hid_abs_last_y;

  uint8_t intr_epaddr;
  uint8_t intr_dci;
  uint16_t intr_mps;
  uint8_t intr_interval;

  xhci_trb_t *intr_ring;
  uint32_t intr_index;
  uint8_t intr_cycle;

  uint8_t *intr_buf;

  uint8_t kbd_prev_mod;
  uint8_t kbd_prev_keys[6];
} xhci_device_state_t;

extern xhci_device_state_t g_devs[256];
extern uint32_t g_ctx_size;

// Helpers provided by xhci.c
uint32_t trb_type(uint32_t control);
uint32_t trb_cc(uint32_t status);
uint32_t trb_slot_id(uint32_t control);
uint32_t make_trb_control(uint32_t type, uint8_t cycle);

void xhci_print_u32_dec(uint32_t v);
void xhci_print_i32_dec(int32_t v);
void xhci_print_hex32(uint32_t v);
void xhci_print_hex8(uint8_t v);

// Rings/EP0 helpers that need to be shared across modules.
xhci_trb_t *xhci_alloc_tr_ring(void);
void xhci_cmd_ring_push(const xhci_trb_t *trb);
int xhci_ep0_control_in(xhci_device_state_t *dev, uint64_t setup, void *buf,
                        uint32_t len);
int xhci_ep0_control_no_data_out(xhci_device_state_t *dev, uint64_t setup);

extern uint64_t xhci_last_command;
extern uint8_t xhci_running;
uint64_t xhci_now_ms(void);
int xhci_deadline(uint64_t start, uint32_t ms, uint32_t spins);
void xhci_delay_ms(uint32_t ms);
void xhci_log_device(xhci_device_state_t *dev, const char *stage);
void xhci_ring_enqueue(xhci_trb_t *ring, uint32_t *index, uint8_t *cycle, const xhci_trb_t *trb);
int xhci_recover_ep0(xhci_device_state_t *dev);
void xhci_note_port_event(uint32_t port);
void xhci_service_ports(void);
// Event ring + polling.
void xhci_event_ring_update_erdp(void);
int xhci_poll_event(xhci_trb_t *out);

int xhci_wait_for_command_completion(uint32_t *out_slot_id, uint32_t *out_cc);
int xhci_wait_for_transfer_event(uint32_t slot_id, uint32_t *out_cc);

// HID support.
uint32_t xhci_hid_get_bits_u32(const uint8_t *buf, uint32_t bitoff, uint32_t bits);
int32_t xhci_hid_get_bits_s32(const uint8_t *buf, uint32_t bitoff, uint32_t bits);

int xhci_cmd_configure_intr_in_ep(xhci_device_state_t *dev);
int xhci_hid_set_protocol_boot(xhci_device_state_t *dev);
int xhci_hid_set_idle(xhci_device_state_t *dev);
void xhci_hid_try_parse_mouse_report_desc(xhci_device_state_t *dev);
void xhci_hid_start_polling(xhci_device_state_t *dev);

// Doorbells used by multiple modules.
void xhci_ring_doorbell_cmd(void);
void xhci_ring_doorbell_ep(uint32_t slot_id, uint32_t dci);

#endif
