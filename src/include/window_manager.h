#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <stdint.h>

/* Bounded window lifecycle, geometry, focus, and z-order primitives. */
typedef struct {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} wm_rect_t;

void wm_clamp_rect(wm_rect_t *rect, uint32_t screen_width,
                   uint32_t screen_height, uint32_t min_width,
                   uint32_t min_height);
void wm_resize_rect(wm_rect_t *rect, int32_t delta_width,
                    int32_t delta_height, uint32_t screen_width,
                    uint32_t screen_height, uint32_t min_width,
                    uint32_t min_height);
void wm_init(uint32_t app_count);
void wm_open(uint32_t app_id);
void wm_close(uint32_t app_id);
void wm_show(uint32_t app_id);
void wm_hide(uint32_t app_id);
void wm_raise(uint32_t app_id);
int wm_is_open(uint32_t app_id);
int wm_is_visible(uint32_t app_id);
int wm_is_focused(uint32_t app_id);
uint32_t wm_focused_app(void);
uint32_t wm_z_count(void);
uint32_t wm_z_at(uint32_t index);

#ifdef VEXOS_CI_INPUT_TEST
int wm_ci_self_test(uint32_t restore_app_count);
#endif

#endif
