#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <stdint.h>

/* Small, bounded core for desktop window visibility, focus, and z-order. */
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
