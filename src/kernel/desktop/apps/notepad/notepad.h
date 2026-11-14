#ifndef VEXOS_NOTEPAD_H
#define VEXOS_NOTEPAD_H

#include <stdint.h>
#include "../../window_manager.h"

typedef struct {
  const desktop_window_t *window;
  const char *filename;
  const char *text;
  uint32_t text_length;
  uint32_t scroll;
  uint8_t focused;
  void (*draw_chrome)(const desktop_window_t *window, const char *title,
                      const char *subtitle, uint8_t focused,
                      uint8_t has_scroll, uint8_t has_save);
} notepad_render_context_t;

void notepad_draw(const notepad_render_context_t *context);
void notepad_init(uint32_t screen_width, uint32_t screen_height);
desktop_window_t *notepad_window(void);
const char *notepad_filename(void);
const char *notepad_text(void);
uint32_t notepad_text_length(void);
uint32_t notepad_scroll(void);
uint8_t notepad_handle_char(char value);
void notepad_scroll_up(void);
void notepad_scroll_down(void);


#endif
