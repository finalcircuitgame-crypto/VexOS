#include "../../include/window_manager.h"

#define WM_MAX_APPS 16u
#define WM_NO_FOCUS 0xFFFFFFFFu

void wm_clamp_rect(wm_rect_t *rect, uint32_t screen_width,
                   uint32_t screen_height, uint32_t min_width,
                   uint32_t min_height) {
  if (!rect || screen_width == 0u || screen_height == 0u)
    return;

  if (screen_width > 0x7FFFFFFFu)
    screen_width = 0x7FFFFFFFu;
  if (screen_height > 0x7FFFFFFFu)
    screen_height = 0x7FFFFFFFu;

  if (min_width == 0u)
    min_width = 1u;
  if (min_height == 0u)
    min_height = 1u;
  if (min_width > screen_width)
    min_width = screen_width;
  if (min_height > screen_height)
    min_height = screen_height;

  int64_t width = rect->w;
  int64_t height = rect->h;
  if (width < (int64_t)min_width)
    width = min_width;
  if (height < (int64_t)min_height)
    height = min_height;
  if (width > (int64_t)screen_width)
    width = screen_width;
  if (height > (int64_t)screen_height)
    height = screen_height;

  int64_t max_x = (int64_t)screen_width - width;
  int64_t max_y = (int64_t)screen_height - height;
  int64_t x = rect->x;
  int64_t y = rect->y;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x > max_x)
    x = max_x;
  if (y > max_y)
    y = max_y;

  rect->x = (int32_t)x;
  rect->y = (int32_t)y;
  rect->w = (int32_t)width;
  rect->h = (int32_t)height;
}

static uint8_t g_app_open[WM_MAX_APPS];
static uint8_t g_app_visible[WM_MAX_APPS];
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
      g_app_visible[i] = 0u;
      g_z_order[i] = 0u;
    }
    return;
  }

  g_app_count = app_count;
  g_z_count = app_count;
  g_focused_app = WM_NO_FOCUS;
  for (uint32_t i = 0; i < WM_MAX_APPS; ++i) {
    g_app_open[i] = 0u;
    g_app_visible[i] = 0u;
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
  if (app_id < g_app_count) {
    g_app_open[app_id] = 0u;
    g_app_visible[app_id] = 0u;
  }
}

void wm_show(uint32_t app_id) {
  if (app_id < g_app_count && g_app_open[app_id])
    g_app_visible[app_id] = 1u;
}

void wm_hide(uint32_t app_id) {
  if (app_id < g_app_count)
    g_app_visible[app_id] = 0u;
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

void wm_resize_rect(wm_rect_t *rect, int32_t delta_width,
                    int32_t delta_height, uint32_t screen_width,
                    uint32_t screen_height, uint32_t min_width,
                    uint32_t min_height) {
  if (!rect || screen_width == 0u || screen_height == 0u)
    return;

  int64_t width = (int64_t)rect->w + (int64_t)delta_width;
  int64_t height = (int64_t)rect->h + (int64_t)delta_height;
  if (width < 1)
    width = 1;
  if (height < 1)
    height = 1;
  if (width > 0x7FFFFFFFll)
    width = 0x7FFFFFFFll;
  if (height > 0x7FFFFFFFll)
    height = 0x7FFFFFFFll;

  rect->w = (int32_t)width;
  rect->h = (int32_t)height;
  wm_clamp_rect(rect, screen_width, screen_height, min_width, min_height);
}

int wm_is_open(uint32_t app_id) {
  return app_id < g_app_count && g_app_open[app_id] != 0u;
}

int wm_is_visible(uint32_t app_id) {
  return app_id < g_app_count && g_app_open[app_id] != 0u &&
         g_app_visible[app_id] != 0u;
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
  if (!wm_is_open(1u) || wm_is_visible(1u) ||
      !wm_is_focused(1u) || wm_z_at(wm_z_count() - 1u) != 1u) {
    goto fail;
  }

  wm_show(1u);
  if (!wm_is_visible(1u))
    goto fail;
  wm_hide(1u);
  if (!wm_is_open(1u) || wm_is_visible(1u) || !wm_is_focused(1u))
    goto fail;
  wm_show(1u);
  if (!wm_is_visible(1u))
    goto fail;

  wm_open(2u);
  wm_show(2u);
  wm_raise(2u);
  if (!wm_is_visible(2u) || !wm_is_focused(2u) ||
      wm_z_at(wm_z_count() - 1u) != 2u ||
      wm_z_at(wm_z_count() - 2u) != 1u) {
    goto fail;
  }

  wm_hide(1u);
  if (!wm_is_open(1u) || wm_is_visible(1u) ||
      !wm_is_open(2u) || !wm_is_visible(2u) ||
      !wm_is_focused(2u)) {
    goto fail;
  }
  wm_close(1u);
  if (wm_is_open(1u) || wm_is_visible(1u) ||
      !wm_is_open(2u) || !wm_is_visible(2u) ||
      !wm_is_focused(2u)) {
    goto fail;
  }

  wm_raise(1u);
  if (!wm_is_focused(1u) || wm_z_at(wm_z_count() - 1u) != 1u)
    goto fail;

  wm_init(restore_app_count);
  wm_raise(0u);

  wm_rect_t rect = {-120, -80, 500, 700};
  wm_clamp_rect(&rect, 640u, 480u, 320u, 180u);
  if (rect.x != 0 || rect.y != 0 || rect.w != 500 || rect.h != 480)
    goto fail;

  rect.x = 600;
  rect.y = 450;
  rect.w = 400;
  rect.h = 300;
  wm_clamp_rect(&rect, 640u, 480u, 320u, 180u);
  if (rect.x != 240 || rect.y != 180 || rect.w != 400 || rect.h != 300)
    goto fail;

  rect.x = 0;
  rect.y = 0;
  rect.w = 0;
  rect.h = 0;
  wm_clamp_rect(&rect, 200u, 120u, 320u, 180u);
  if (rect.x != 0 || rect.y != 0 || rect.w != 200 || rect.h != 120)
    goto fail;

  rect.x = 40;
  rect.y = 30;
  rect.w = 400;
  rect.h = 260;
  wm_resize_rect(&rect, 80, 40, 640u, 480u, 320u, 180u);
  if (rect.x != 40 || rect.y != 30 || rect.w != 480 || rect.h != 300)
    goto fail;
  wm_resize_rect(&rect, -1000, -1000, 640u, 480u, 320u, 180u);
  if (rect.w != 320 || rect.h != 180)
    goto fail;
  wm_resize_rect(&rect, 1000, 1000, 640u, 480u, 320u, 180u);
  if (rect.w != 640 || rect.h != 480 || rect.x != 0 || rect.y != 0)
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
