#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

typedef struct {
  int32_t x;
  int32_t y;
  int32_t dx;
  int32_t dy;
  uint32_t buttons;
  uint8_t is_absolute;
} input_pointer_state_t;

void input_init(uint32_t screen_w, uint32_t screen_h);

void input_pointer_set_abs(int32_t x, int32_t y, uint32_t buttons);
void input_pointer_set_abs_scaled(int32_t x, int32_t y, int32_t x_max,
                                  int32_t y_max, uint32_t buttons);
void input_pointer_add_rel(int32_t dx, int32_t dy, uint32_t buttons);

input_pointer_state_t input_pointer_get(void);

void input_kbd_push_char(char c);
int input_kbd_pop_char(char *out);

#endif
