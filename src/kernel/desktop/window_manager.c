#include "window_manager.h"

static app_id_t g_z_order[APP_COUNT];
static uint32_t g_z_order_count;
static app_id_t g_focused_app;

static int wm_app_id_valid(app_id_t app) {
  return (uint32_t)app < (uint32_t)APP_COUNT;
}

void wm_init(void) {
  g_z_order_count = 0;
  for (uint32_t i = 0; i < (uint32_t)APP_COUNT; ++i) {
    if (g_z_order_count < (uint32_t)APP_COUNT)
      g_z_order[g_z_order_count++] = (app_id_t)i;
  }
  g_focused_app = APP_SHELL;
}

void wm_raise(app_id_t app) {
  if (!wm_app_id_valid(app))
    return;

  uint32_t found = g_z_order_count;
  for (uint32_t i = 0; i < g_z_order_count; ++i) {
    if (g_z_order[i] == app) {
      found = i;
      break;
    }
  }

  /* Never focus a window that could not be represented in the z-order. */
  if (found == g_z_order_count && g_z_order_count >= (uint32_t)APP_COUNT)
    return;

  if (found < g_z_order_count) {
    for (uint32_t i = found + 1u; i < g_z_order_count; ++i)
      g_z_order[i - 1u] = g_z_order[i];
    --g_z_order_count;
  }

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
