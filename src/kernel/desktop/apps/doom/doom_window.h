#ifndef VEXOS_DOOM_WINDOW_H
#define VEXOS_DOOM_WINDOW_H
#include <stdint.h>
void doom_window_init(uint32_t screen_width,uint32_t screen_height);
int doom_window_open(void);
void doom_window_app_exited(void);
int doom_window_draw_frame(const uint32_t *src,uint32_t width,uint32_t height);
void doom_window_draw(void);
int doom_window_handle_click(int32_t x,int32_t y);
int doom_window_pointer_update(int32_t x,int32_t y,uint32_t buttons,int32_t dx,int32_t dy);
int doom_window_is_maximized(void);
int doom_window_contains(int32_t x,int32_t y);
int doom_window_pointer_in_content(int32_t x,int32_t y);
int doom_window_close_requested(void);
#endif
