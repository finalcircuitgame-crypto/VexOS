#ifndef VEXOS_WINDOW_MANAGER_H
#define VEXOS_WINDOW_MANAGER_H

#include <stdint.h>

typedef struct {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} desktop_window_t;

typedef enum {
  APP_SHELL = 0,
  APP_EXPLORER = 1,
  APP_VIEWER = 2,
  APP_NOTEPAD = 3,
  APP_SETUP = 4,
  APP_DOOM = 5,
  APP_COUNT = 6
} app_id_t;

/* Internal desktop window-order/focus service. */
void wm_init(void);
void wm_raise(app_id_t app);
app_id_t wm_get_focus(void);
uint32_t wm_count(void);
app_id_t wm_app_at(uint32_t index);

#endif
