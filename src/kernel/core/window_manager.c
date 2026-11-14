#include "../../include/window_manager.h"

#define WM_MAX_APPS 16u
#define WM_NO_FOCUS 0xFFFFFFFFu

static uint8_t g_app_open[WM_MAX_APPS];
static uint8_t g_z_order[WM_MAX_APPS];
static uint32_t g_app_count;
static uint32_t g_z_count;
static uint32_t g_focused_app = WM_NO_FOCUS;

void wm_init(uint32_t app_count) {
  if (app_count == 0u || app_count > WM_MAX_APPS) {
    g_app_count = 0u;
    g_z_count = 0u;
    g_focused_app = WM_NO_FOCUS;
    for (uint32_t i = 0; i < WM_MAX_APPS; ++i) {
      g_app_open[i] = 0u;
      g_z_order[i] = 0u;
    }
    return;
  }

  g_app_count = app_count;
  g_z_count = app_count;
  g_focused_app = WM_NO_FOCUS;
  for (uint32_t i = 0; i < WM_MAX_APPS; ++i) {
    g_app_open[i] = 0u;
    g_z_order[i] = 0u;
  }
  for (uint32_t i = 0; i < app_count; ++i)
    g_z_order[i] = (uint8_t)i;
}

void wm_open(uint32_t app_id) {
  if (app_id < g_app_count)
    g_app_open[app_id] = 1u;
}

void wm_close(uint32_t app_id) {
  if (app_id < g_app_count)
    g_app_open[app_id] = 0u;
}

void wm_raise(uint32_t app_id) {
  if (app_id >= g_app_count)
    return;

  uint32_t found = g_z_count;
  for (uint32_t i = 0; i < g_z_count; ++i) {
    if (g_z_order[i] == app_id) {
      found = i;
      break;
    }
  }

  if (found < g_z_count) {
    for (uint32_t i = found + 1u; i < g_z_count; ++i)
      g_z_order[i - 1u] = g_z_order[i];
    --g_z_count;
  }

  if (g_z_count < g_app_count)
    g_z_order[g_z_count++] = (uint8_t)app_id;
  g_focused_app = app_id;
}

int wm_is_open(uint32_t app_id) {
  return app_id < g_app_count && g_app_open[app_id] != 0u;
}

int wm_is_focused(uint32_t app_id) {
  return app_id < g_app_count && g_focused_app == app_id;
}

uint32_t wm_focused_app(void) {
  return g_focused_app;
}

uint32_t wm_z_count(void) {
  return g_z_count;
}

uint32_t wm_z_at(uint32_t index) {
  if (index >= g_z_count)
    return WM_NO_FOCUS;
  return (uint32_t)g_z_order[index];
}

#ifdef VEXOS_CI_INPUT_TEST
int wm_ci_self_test(uint32_t restore_app_count) {
  if (restore_app_count < 3u || restore_app_count > WM_MAX_APPS)
    return 0;

  wm_init(restore_app_count);
  if (wm_z_count() != restore_app_count ||
      wm_focused_app() != WM_NO_FOCUS ||
      wm_is_open(0u) || wm_is_open(1u)) {
    goto fail;
  }

  for (uint32_t i = 0; i < restore_app_count; ++i) {
    if (wm_z_at(i) != i)
      goto fail;
  }

  wm_open(1u);
  wm_raise(1u);
  if (!wm_is_open(1u) || !wm_is_focused(1u) ||
      wm_z_at(wm_z_count() - 1u) != 1u) {
    goto fail;
  }

  wm_open(2u);
  wm_raise(2u);
  if (!wm_is_focused(2u) || wm_z_at(wm_z_count() - 1u) != 2u ||
      wm_z_at(wm_z_count() - 2u) != 1u) {
    goto fail;
  }

  wm_close(1u);
  if (wm_is_open(1u) || !wm_is_open(2u) || !wm_is_focused(2u))
    goto fail;

  wm_raise(1u);
  if (!wm_is_focused(1u) || wm_z_at(wm_z_count() - 1u) != 1u)
    goto fail;

  wm_init(restore_app_count);
  wm_raise(0u);
  return 1;

fail:
  wm_init(restore_app_count);
  wm_raise(0u);
  return 0;
}
#endif
