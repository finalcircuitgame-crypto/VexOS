#ifndef VEXOS_WINDOW_COMPOSITOR_H
#define VEXOS_WINDOW_COMPOSITOR_H

#include <stdint.h>
#include "../window_manager.h"

typedef void (*window_compositor_draw_fn)(void);

/* Register one renderer and its live visibility flag for each app ID. */
void window_compositor_init(void);
void window_compositor_register(app_id_t app, const uint8_t *visible,
                                window_compositor_draw_fn draw);
void window_compositor_draw(void);

#endif
