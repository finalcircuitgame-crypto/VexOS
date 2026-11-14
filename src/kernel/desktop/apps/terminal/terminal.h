#ifndef VEXOS_TERMINAL_H
#define VEXOS_TERMINAL_H
#include <stdint.h>
#include "../../window_manager.h"
typedef enum { TERMINAL_CLICK_NONE=0, TERMINAL_CLICK_CLOSE, TERMINAL_CLICK_FOCUS } terminal_click_result_t;
typedef struct { const desktop_window_t *window; uint32_t tick; uint8_t focused; void (*draw_chrome)(const desktop_window_t *,const char *,const char *,uint8_t,uint8_t,uint8_t); } terminal_render_context_t;
void terminal_init(uint32_t,uint32_t); desktop_window_t *terminal_window(void); void terminal_append(const char *);
const char *terminal_line(void); uint32_t terminal_line_length(void); void terminal_clear_line(void); void terminal_clear_output(void);
uint8_t terminal_handle_char(char); terminal_click_result_t terminal_handle_click(int32_t,int32_t);
void terminal_pointer_move(int32_t,int32_t); void terminal_pointer_release(void); void terminal_draw(const terminal_render_context_t *);
#endif
