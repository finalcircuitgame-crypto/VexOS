#ifndef VEXOS_TERMINAL_H
#define VEXOS_TERMINAL_H

#include <stdint.h>
#include "../../window_manager.h"

typedef enum {
  TERMINAL_CLICK_NONE = 0,
  TERMINAL_CLICK_CLOSE,
  TERMINAL_CLICK_FOCUS
} terminal_click_result_t;

typedef struct {
  const desktop_window_t *window;
  uint32_t tick;
  uint8_t focused;
  void (*draw_chrome)(const desktop_window_t *window, const char *title,
                      const char *subtitle, uint8_t focused,
                      uint8_t has_scroll, uint8_t has_save);
} terminal_render_context_t;

void terminal_init(uint32_t screen_width, uint32_t screen_height);
desktop_window_t *terminal_window(void);
void terminal_append(const char *text);
const char *terminal_line(void);
uint32_t terminal_line_length(void);
void terminal_clear_line(void);
void terminal_clear_output(void);

/* Returns 1 when input changed, 2 when Enter submits, or 0 when ignored. */
uint8_t terminal_handle_char(char value);
terminal_click_result_t terminal_handle_click(int32_t x, int32_t y);
void terminal_pointer_move(int32_t x, int32_t y);
void terminal_pointer_release(void);
void terminal_draw(const terminal_render_context_t *context);

#endif
