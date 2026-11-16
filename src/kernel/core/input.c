#include "../../include/input.h"
#include "../../include/interrupts.h"

extern void serial_print(const char *str);

static void input_log_arrow(uint8_t usage, int pressed) {
  const char *key = 0;
  switch (usage) {
  case 0x4F: key = "RIGHT"; break;
  case 0x50: key = "LEFT"; break;
  case 0x51: key = "DOWN"; break;
  case 0x52: key = "UP"; break;
  default: return;
  }

  char digits[21];
  uint32_t count = 0;
  uint64_t tick = Timer_Ticks();
  do {
    digits[count++] = (char)('0' + (tick % 10u));
    tick /= 10u;
  } while (tick && count < sizeof(digits));

  serial_print("[USB][HID] tick=");
  while (count)
    serial_print((char[]){digits[--count], 0});
  serial_print(" key=");
  serial_print(key);
  serial_print(pressed ? " state=DOWN\\n" : " state=UP\\n");
}

static input_pointer_state_t g_ptr;
static uint32_t g_w;
static uint32_t g_h;
static int32_t g_pending_dx;
static int32_t g_pending_dy;

static char g_kbd_q[256];
static uint32_t g_kbd_r;
static uint32_t g_kbd_w;

#define KBD_EVENT_MASK 127u
static input_kbd_event_t g_kbd_events[128];
static uint8_t g_kbd_down[256];
static uint32_t g_kbd_event_r;
static uint32_t g_kbd_event_w;

#ifdef VEXOS_CI_INPUT_TEST
extern void serial_print(const char *str);

static void input_ci_trace_arrow(uint8_t usage, int pressed) {
  switch (usage) {
  case 0x4F:
    serial_print(pressed ? "[CI] USB arrow RIGHT down\n" : "[CI] USB arrow RIGHT released\n");
    break;
  case 0x50:
    serial_print(pressed ? "[CI] USB arrow LEFT down\n" : "[CI] USB arrow LEFT released\n");
    break;
  case 0x51:
    serial_print(pressed ? "[CI] USB arrow DOWN down\n" : "[CI] USB arrow DOWN released\n");
    break;
  case 0x52:
    serial_print(pressed ? "[CI] USB arrow UP down\n" : "[CI] USB arrow UP released\n");
    break;
  default:
    break;
  }
}
#else
#define input_ci_trace_arrow(usage, pressed) ((void)0)
#endif

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static int32_t saturating_add_i32(int32_t a, int32_t b) {
  int64_t sum = (int64_t)a + (int64_t)b;
  if (sum > INT32_MAX) return INT32_MAX;
  if (sum < INT32_MIN) return INT32_MIN;
  return (int32_t)sum;
}

void input_init(uint32_t screen_w, uint32_t screen_h) {
  g_w = screen_w;
  g_h = screen_h;
  g_ptr.x = (int32_t)(screen_w / 2);
  g_ptr.y = (int32_t)(screen_h / 2);
  g_ptr.dx = 0;
  g_ptr.dy = 0;
  g_ptr.buttons = 0;
  g_ptr.is_absolute = 0;
  g_pending_dx = 0;
  g_pending_dy = 0;

  g_kbd_r = 0;
  g_kbd_w = 0;
  g_kbd_event_r = 0;
  g_kbd_event_w = 0;
  for (uint32_t i = 0; i < 256u; ++i)
    g_kbd_down[i] = 0;
}

void input_pointer_set_abs(int32_t x, int32_t y, uint32_t buttons) {
  int32_t nx = clamp_i32(x, 0, (int32_t)g_w - 1);
  int32_t ny = clamp_i32(y, 0, (int32_t)g_h - 1);
  int32_t dx = nx - g_ptr.x, dy = ny - g_ptr.y;
  g_ptr.dx = dx; g_ptr.dy = dy;
  g_ptr.x = nx; g_ptr.y = ny;
  g_pending_dx = saturating_add_i32(g_pending_dx, dx);
  g_pending_dy = saturating_add_i32(g_pending_dy, dy);
  g_ptr.buttons = buttons;
  g_ptr.is_absolute = 1;
}

void input_pointer_set_abs_scaled(int32_t x, int32_t y, int32_t x_max,
                                  int32_t y_max, uint32_t buttons) {
  if (x_max <= 0)
    x_max = 1;
  if (y_max <= 0)
    y_max = 1;

  int32_t px = (int32_t)(((int64_t)x * (int64_t)((int32_t)g_w - 1)) / x_max);
  int32_t py = (int32_t)(((int64_t)y * (int64_t)((int32_t)g_h - 1)) / y_max);
  input_pointer_set_abs(px, py, buttons);
}

void input_pointer_add_rel(int32_t dx, int32_t dy, uint32_t buttons) {
  g_ptr.dx = dx; g_ptr.dy = dy;
  g_ptr.x = clamp_i32(g_ptr.x + dx, 0, (int32_t)g_w - 1);
  g_ptr.y = clamp_i32(g_ptr.y + dy, 0, (int32_t)g_h - 1);
  g_pending_dx = saturating_add_i32(g_pending_dx, dx);
  g_pending_dy = saturating_add_i32(g_pending_dy, dy);
  g_ptr.buttons = buttons;
  g_ptr.is_absolute = 0;
}

input_pointer_state_t input_pointer_get(void) { return g_ptr; }

void input_pointer_take_motion(int32_t *dx, int32_t *dy) {
  if (dx) *dx = g_pending_dx;
  if (dy) *dy = g_pending_dy;
  g_pending_dx = 0; g_pending_dy = 0;
}

void input_kbd_push_char(char c) {
  uint32_t next = (g_kbd_w + 1u) & 0xFFu;
  if (next == (g_kbd_r & 0xFFu)) {
    return;
  }
  g_kbd_q[g_kbd_w & 0xFFu] = c;
  g_kbd_w = next;
}

int input_kbd_pop_char(char *out) {
  if (!out) {
    return 0;
  }
  if ((g_kbd_r & 0xFFu) == (g_kbd_w & 0xFFu)) {
    return 0;
  }
  *out = g_kbd_q[g_kbd_r & 0xFFu];
  g_kbd_r = (g_kbd_r + 1u) & 0xFFu;
  return 1;
}

void input_kbd_push_event(uint8_t usage, int pressed) {
  /* Keep the physical HID state even if the transition queue is full.
     Doom can reconcile arrow-key state after draining queued events, so a
     dropped key-up can never leave movement stuck indefinitely. */
  g_kbd_down[usage] = pressed ? 1u : 0u;
  input_log_arrow(usage, pressed);
  input_ci_trace_arrow(usage, pressed);

  uint32_t next = (g_kbd_event_w + 1u) & KBD_EVENT_MASK;
  if (next == g_kbd_event_r)
    return;
  g_kbd_events[g_kbd_event_w].usage = usage;
  g_kbd_events[g_kbd_event_w].pressed = pressed ? 1u : 0u;
  g_kbd_event_w = next;
}

uint32_t input_kbd_arrow_state(void) {
  /* USB HID keyboard usages: Right=0x4F, Left=0x50, Down=0x51, Up=0x52. */
  return (g_kbd_down[0x4Fu] ? 1u : 0u) |
         (g_kbd_down[0x50u] ? 2u : 0u) |
         (g_kbd_down[0x51u] ? 4u : 0u) |
         (g_kbd_down[0x52u] ? 8u : 0u);
}

int input_kbd_pop_event(input_kbd_event_t *out) {
  if (!out || g_kbd_event_r == g_kbd_event_w)
    return 0;
  *out = g_kbd_events[g_kbd_event_r];
  g_kbd_event_r = (g_kbd_event_r + 1u) & KBD_EVENT_MASK;
  return 1;
}

uint32_t input_kbd_clear_events(void) {
  uint32_t pending = (g_kbd_event_w - g_kbd_event_r) & KBD_EVENT_MASK;
  g_kbd_event_r = g_kbd_event_w;
  return pending;
}
