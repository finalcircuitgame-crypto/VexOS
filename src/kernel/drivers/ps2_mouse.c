#include "../../include/input.h"
#include "../../include/ps2_mouse.h"
#include <stdint.h>

#define PS2_DATA_PORT   0x60u
#define PS2_STATUS_PORT 0x64u
#define PS2_WRITE_LIMIT 100000u
#define PS2_PACKET_SIZE 3u

static uint8_t g_ps2_mouse_initialized;
static uint8_t g_ps2_mouse_packet[PS2_PACKET_SIZE];
static uint8_t g_ps2_mouse_packet_index;

extern void serial_print(const char *str);
extern void desktop_debug_set_input_status(const char *status);

static inline uint8_t ps2_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static inline void ps2_outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int ps2_wait_input_empty(void) {
  for (uint32_t i = 0; i < PS2_WRITE_LIMIT; ++i) {
    if ((ps2_inb(PS2_STATUS_PORT) & 0x02u) == 0u)
      return 1;
    __asm__ volatile("pause");
  }
  return 0;
}

static int ps2_wait_output_full(uint8_t require_aux, uint8_t *value) {
  if (!value)
    return 0;
  for (uint32_t i = 0; i < PS2_WRITE_LIMIT; ++i) {
    uint8_t status = ps2_inb(PS2_STATUS_PORT);
    if (status & 0x01u) {
      uint8_t data = ps2_inb(PS2_DATA_PORT);
      if (!require_aux || (status & 0x20u)) {
        *value = data;
        return 1;
      }
    }
    __asm__ volatile("pause");
  }
  return 0;
}

static int ps2_write_command(uint8_t command) {
  if (!ps2_wait_input_empty())
    return 0;
  ps2_outb(PS2_STATUS_PORT, command);
  return 1;
}

static int ps2_write_data(uint8_t data) {
  if (!ps2_wait_input_empty())
    return 0;
  ps2_outb(PS2_DATA_PORT, data);
  return 1;
}

static int ps2_write_aux(uint8_t data) {
  return ps2_write_command(0xD4u) && ps2_write_data(data);
}

static void ps2_restore_ports(void) {
  (void)ps2_write_command(0xAEu);
  (void)ps2_write_command(0xA8u);
}

static int ps2_aux_expect_ack(void) {
  uint8_t response = 0;
  if (!ps2_wait_output_full(1u, &response))
    return 0;
  return response == 0xFAu;
}

int ps2_mouse_init(void) {
  if (g_ps2_mouse_initialized)
    return 1;

  uint8_t initial_status = ps2_inb(PS2_STATUS_PORT);
  if (initial_status == 0xFFu || initial_status == 0x00u) {
    serial_print("[PS2] No usable i8042 controller detected\n");
    return 0;
  }

  /* Temporarily disable both ports so the controller command byte can be
     changed without racing keyboard scancodes or auxiliary packets. */
  if (!ps2_write_command(0xADu) || !ps2_write_command(0xA7u)) {
    ps2_restore_ports();
    serial_print("[PS2] Controller did not accept port-disable commands\n");
    return 0;
  }

  for (uint32_t i = 0; i < 32u; ++i) {
    if ((ps2_inb(PS2_STATUS_PORT) & 0x01u) == 0u)
      break;
    (void)ps2_inb(PS2_DATA_PORT);
  }

  if (!ps2_write_command(0x20u)) {
    serial_print("[PS2] Could not request controller command byte\n");
    return 0;
  }

  uint8_t command_byte = 0;
  if (!ps2_wait_output_full(0u, &command_byte)) {
    ps2_restore_ports();
    serial_print("[PS2] Controller command-byte read timed out\n");
    return 0;
  }

  /* Poll AUX data in software: clear IRQ12 enable and the AUX clock-disable
     bit. Also leave the first port clock enabled for a PS/2 keyboard. */
  command_byte &= (uint8_t)~((1u << 1) | (1u << 4) | (1u << 5));
  if (!ps2_write_command(0x60u) || !ps2_write_data(command_byte) ||
      !ps2_write_command(0xA8u) || !ps2_write_command(0xAEu)) {
    ps2_restore_ports();
    serial_print("[PS2] Could not enable controller ports\n");
    return 0;
  }

  if (!ps2_write_aux(0xF6u) || !ps2_aux_expect_ack()) {
    serial_print("[PS2] Auxiliary device rejected defaults; trackpad unavailable\n");
    return 0;
  }
  if (!ps2_write_aux(0xF4u) || !ps2_aux_expect_ack()) {
    serial_print("[PS2] Auxiliary device did not enable data reporting\n");
    return 0;
  }

  g_ps2_mouse_packet_index = 0;
  g_ps2_mouse_initialized = 1u;
  serial_print("[PS2] Auxiliary mouse initialized in polling mode\n");
  return 1;
}

static void ps2_mouse_consume_byte(uint8_t value) {
  if (g_ps2_mouse_packet_index == 0u) {
    /* Bit 3 is always one in the first byte; use it to regain packet sync. */
    if ((value & 0x08u) == 0u)
      return;
    g_ps2_mouse_packet[0] = value;
    g_ps2_mouse_packet_index = 1u;
    return;
  }

  g_ps2_mouse_packet[g_ps2_mouse_packet_index++] = value;
  if (g_ps2_mouse_packet_index < PS2_PACKET_SIZE)
    return;

  uint8_t flags = g_ps2_mouse_packet[0];
  g_ps2_mouse_packet_index = 0u;

  /* Overflow makes the signed displacement unusable for this packet. */
  if (flags & 0xC0u)
    return;

  int32_t dx = (int32_t)(int8_t)g_ps2_mouse_packet[1];
  int32_t dy = -(int32_t)(int8_t)g_ps2_mouse_packet[2];
  uint32_t buttons = (uint32_t)(flags & 0x07u);
  /* Always publish packets so a zero-motion button release is not lost. */
  input_pointer_add_rel(dx, dy, buttons);
  desktop_debug_set_input_status("[PS/2] Touchpad packets received");
}

void ps2_mouse_poll(void) {
  if (!g_ps2_mouse_initialized)
    return;

  /* The laptop's internal PS/2 touchpad commonly shares the i8042 output
     buffer with the keyboard. Consume only auxiliary bytes here. */
  for (uint32_t processed = 0; processed < 32u; ++processed) {
    uint8_t status = ps2_inb(PS2_STATUS_PORT);
    if ((status & 0x01u) == 0u)
      break;
    uint8_t data = ps2_inb(PS2_DATA_PORT);
    if (status & 0x20u)
      ps2_mouse_consume_byte(data);
    /* PS/2 keyboard input is not handled by this driver. Drain its byte so
       it cannot block queued auxiliary touchpad packets in the shared buffer. */
  }
}
