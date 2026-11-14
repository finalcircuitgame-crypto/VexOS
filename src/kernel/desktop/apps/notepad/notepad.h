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

#endif
