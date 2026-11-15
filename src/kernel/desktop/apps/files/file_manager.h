#ifndef VEXOS_FILE_MANAGER_H
#define VEXOS_FILE_MANAGER_H

#include <stdint.h>
#include "../../window_manager.h"

typedef void (*file_manager_chrome_fn)(const desktop_window_t *window,
                                       const char *title,
                                       const char *subtitle,
                                       uint8_t focused,
                                       uint8_t has_scroll,
                                       uint8_t has_save);
typedef int (*file_manager_open_file_fn)(const char *filename);

typedef struct {
  uint8_t focused;
  file_manager_chrome_fn draw_chrome;
} file_manager_render_context_t;

typedef enum {
  FILE_MANAGER_EVENT_NONE = 0,
  FILE_MANAGER_EVENT_CHANGED,
  FILE_MANAGER_EVENT_CLOSED,
  FILE_MANAGER_EVENT_OPEN_FAILED
} file_manager_event_t;

void file_manager_init(uint32_t screen_width, uint32_t screen_height);
void file_manager_open(void);
void file_manager_close(void);
uint8_t file_manager_is_open(void);
desktop_window_t *file_manager_window(void);

void file_manager_draw(const file_manager_render_context_t *context);
file_manager_event_t file_manager_handle_click(
    int32_t x, int32_t y, uint32_t tick,
    file_manager_open_file_fn open_file);
uint8_t file_manager_handle_char(char value);
void file_manager_pointer_move(int32_t x, int32_t y);
void file_manager_pointer_release(void);

#endif
