#ifndef VEXOS_WINDOW_CHROME_H
#define VEXOS_WINDOW_CHROME_H

#include <stdint.h>
#include "window_manager.h"

typedef void (*window_chrome_icon_fn)(uint32_t x, uint32_t y,
                                      uint32_t size, app_id_t app);

void window_chrome_draw(const desktop_window_t *window, const char *title,
                        const char *subtitle, uint8_t focused,
                        uint8_t has_scroll, uint8_t has_save,
                        window_chrome_icon_fn draw_icon);

#endif
