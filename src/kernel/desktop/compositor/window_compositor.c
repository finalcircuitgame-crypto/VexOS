#include "window_compositor.h"

static const uint8_t *g_visible[APP_COUNT];
static window_compositor_draw_fn g_draw[APP_COUNT];

void window_compositor_init(void) {
  for (uint32_t i = 0; i < APP_COUNT; ++i) {
    g_visible[i] = 0;
    g_draw[i] = 0;
  }
}

void window_compositor_register(app_id_t app, const uint8_t *visible,
                                window_compositor_draw_fn draw) {
  if ((uint32_t)app >= APP_COUNT)
    return;
  g_visible[app] = visible;
  g_draw[app] = draw;
}

void window_compositor_draw(void) {
  /* The window manager stores back-to-front order. */
  for (uint32_t i = 0; i < wm_count(); ++i) {
    app_id_t app = wm_app_at(i);
    if ((uint32_t)app >= APP_COUNT)
      continue;
    if (!g_visible[app] || !*g_visible[app] || !g_draw[app])
      continue;
    g_draw[app]();
  }
}
