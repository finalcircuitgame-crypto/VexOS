#include "../../include/input.h"

static input_pointer_state_t g_ptr;
static uint32_t g_w;
static uint32_t g_h;

static char g_kbd_q[256];
static uint32_t g_kbd_r;
static uint32_t g_kbd_w;

#define KBD_EVENT_MASK 127u
static input_kbd_event_t g_kbd_events[128];
static uint32_t g_kbd_event_r;
static uint32_t g_kbd_event_w;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
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

  g_kbd_r = 0;
  g_kbd_w = 0;
  g_kbd_event_r = 0;
  g_kbd_event_w = 0;
}

void input_pointer_set_abs(int32_t x, int32_t y, uint32_t buttons) {
  g_ptr.dx = x - g_ptr.x;
  g_ptr.dy = y - g_ptr.y;
  g_ptr.x = clamp_i32(x, 0, (int32_t)g_w - 1);
  g_ptr.y = clamp_i32(y, 0, (int32_t)g_h - 1);
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
  g_ptr.dx = dx;
  g_ptr.dy = dy;
  g_ptr.x = clamp_i32(g_ptr.x + dx, 0, (int32_t)g_w - 1);
  g_ptr.y = clamp_i32(g_ptr.y + dy, 0, (int32_t)g_h - 1);
  g_ptr.buttons = buttons;
  g_ptr.is_absolute = 0;
}

input_pointer_state_t input_pointer_get(void) { return g_ptr; }

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
  uint32_t next = (g_kbd_event_w + 1u) & KBD_EVENT_MASK;
  if (next == g_kbd_event_r)
    return;
  g_kbd_events[g_kbd_event_w].usage = usage;
  g_kbd_events[g_kbd_event_w].pressed = pressed ? 1u : 0u;
  g_kbd_event_w = next;
}

int input_kbd_pop_event(input_kbd_event_t *out) {
  if (!out || g_kbd_event_r == g_kbd_event_w)
    return 0;
  *out = g_kbd_events[g_kbd_event_r];
  g_kbd_event_r = (g_kbd_event_r + 1u) & KBD_EVENT_MASK;
  return 1;
}


#ifdef VEXOS_CI_INPUT_TEST
int input_ci_self_test(void) {
  input_kbd_event_t event;
  input_pointer_state_t pointer;
  char character = 0;

  input_init(640u, 480u);
  input_pointer_set_abs_scaled(32767, 32767, 32767, 32767, 1u);
  pointer = input_pointer_get();
  if (pointer.x != 639 || pointer.y != 479 ||
      pointer.buttons != 1u || !pointer.is_absolute) {
    return 0;
  }

  input_pointer_add_rel(10, -1000, 5u);
  pointer = input_pointer_get();
  if (pointer.x != 639 || pointer.y != 0 ||
      pointer.dx != 10 || pointer.dy != -1000 ||
      pointer.buttons != 5u || pointer.is_absolute) {
    return 0;
  }

  input_pointer_add_rel(-700, 900, 0u);
  pointer = input_pointer_get();
  if (pointer.x != 0 || pointer.y != 479)
    return 0;

  input_kbd_push_char('z');
  input_kbd_push_char('\n');
  if (!input_kbd_pop_char(&character) || character != 'z')
    return 0;
  if (!input_kbd_pop_char(&character) || character != '\n')
    return 0;
  if (input_kbd_pop_char(&character))
    return 0;

  input_kbd_push_event(0x04u, 1);
  input_kbd_push_event(0x04u, 0);
  if (!input_kbd_pop_event(&event) || event.usage != 0x04u ||
      event.pressed != 1u) {
    return 0;
  }
  if (!input_kbd_pop_event(&event) || event.usage != 0x04u ||
      event.pressed != 0u) {
    return 0;
  }
  if (input_kbd_pop_event(&event))
    return 0;

  return 1;
}
#endif
