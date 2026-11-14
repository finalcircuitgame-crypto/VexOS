#include "window_manager.h"

static app_id_t g_z_order[APP_COUNT];
static uint32_t g_z_order_count;
static app_id_t g_focused_app;

void wm_init(void) {
  for (uint32_t i = 0; i < APP_COUNT; ++i)
    g_z_order[i] = (app_id_t)i;
  g_z_order_count = APP_COUNT;
  g_focused_app = APP_SHELL;
}

void wm_raise(app_id_t app) {
  if ((uint32_t)app >= APP_COUNT)
    return;

  uint32_t found = g_z_order_count;
  for (uint32_t i = 0; i < g_z_order_count; ++i) {
    if (g_z_order[i] == app) {
      found = i;
      break;
    }
  }

  if (found < g_z_order_count) {
    for (uint32_t i = found + 1u; i < g_z_order_count; ++i)
      g_z_order[i - 1u] = g_z_order[i];
    --g_z_order_count;
  }

  if (g_z_order_count < APP_COUNT)
    g_z_order[g_z_order_count++] = app;

  g_focused_app = app;
}

app_id_t wm_get_focus(void) {
  return g_focused_app;
}

uint32_t wm_count(void) {
  return g_z_order_count;
}

app_id_t wm_app_at(uint32_t index) {
  if (index >= g_z_order_count)
    return APP_COUNT;
  return g_z_order[index];
}
