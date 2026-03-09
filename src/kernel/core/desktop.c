#include "../../include/desktop.h"
#include "../../include/input.h"
#include "../../include/heap.h"
#include "../../include/hw_profile.h"
#include "../../include/usermode.h"
#include "../../include/task.h"
#include "../../include/simplefs.h"
#include "../../include/ramfs.h"
#include "../../include/png.h"
#include <stdint.h>

static BootInfo *g_bi;
static uint32_t g_tick;
static uint32_t *g_back;
static uint32_t *g_bg;

extern uint8_t _binary_CGA_F08_start[];
extern uint8_t _binary_CGA_F08_end[];

static char g_text[1024];
static uint32_t g_text_len;

static uint8_t g_start_open;
static uint8_t g_show_shell;

static char g_shell_out[4096];
static uint32_t g_shell_out_len;
static char g_shell_line[256];
static uint32_t g_shell_line_len;

static input_pointer_state_t g_last_ptr;
static uint32_t g_rand_state;

typedef struct {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} wm_window_t;

static wm_window_t g_shell_win;
static wm_window_t g_explorer_win;
static wm_window_t g_viewer_win;
static uint8_t g_shell_dragging;
static int32_t g_shell_drag_off_x;
static int32_t g_shell_drag_off_y;
static uint8_t g_explorer_open;
static uint8_t g_viewer_open;
static uint8_t g_explorer_dragging;
static int32_t g_explorer_drag_off_x;
static int32_t g_explorer_drag_off_y;
static uint8_t g_viewer_dragging;
static int32_t g_viewer_drag_off_x;
static int32_t g_viewer_drag_off_y;
static char g_viewer_name[64];
static char g_viewer_text[1024];
static uint32_t g_viewer_text_len;

static uint32_t g_explorer_scroll;
static uint32_t g_viewer_scroll;
static uint32_t g_explorer_selected;
static uint32_t g_explorer_last_click_tick;
static uint32_t g_explorer_last_click_index;

typedef enum {
  APP_SHELL = 0,
  APP_EXPLORER = 1,
  APP_VIEWER = 2,
  APP_NOTEPAD = 3,
  APP_COUNT = 4
} app_id_t;

static uint8_t g_app_open[APP_COUNT];
static app_id_t g_app_z[APP_COUNT];
static uint32_t g_app_z_len;
static app_id_t g_app_focus;

static wm_window_t g_notepad_win;
static uint8_t g_notepad_dragging;
static int32_t g_notepad_drag_off_x;
static int32_t g_notepad_drag_off_y;
static char g_notepad_name[64];
static char g_notepad_text[4096];
static uint32_t g_notepad_text_len;
static uint32_t g_notepad_scroll;

static void shell_append(const char *s);

static uint8_t g_ring3_task_started;

static void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c);
static void draw_string_8x8(uint32_t x, uint32_t y, const char *s,
                            uint32_t fg);

static void launch_ring3_task(void) {
  if (g_ring3_task_started)
    return;
  g_ring3_task_started = 1;

  void *stack = kmalloc(8192);
  if (!stack) {
    shell_append("ring3: no mem\n");
    g_ring3_task_started = 0;
    return;
  }
  void *stack_top = (void *)((uint8_t *)stack + 8192);
  Task_Create(usermode_launch, stack_top);
}

static void draw_notepad_window(const wm_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 0x0E0E10);
  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 16, 0x14141A);
  draw_rect((uint32_t)x, (uint32_t)y + 16, (uint32_t)w, 1, 0x2A3140);
  draw_string_8x8((uint32_t)x + 6, (uint32_t)y + 4, "Notepad", 0xFFFFFF);
  if (g_notepad_name[0]) {
    draw_string_8x8((uint32_t)x + 70u, (uint32_t)y + 4, g_notepad_name,
                    0xD0D6E0);
  }

  // Close
  draw_rect((uint32_t)(x + w - 20), (uint32_t)y + 2, 16, 12, 0x401010);
  draw_string_8x8((uint32_t)(x + w - 16), (uint32_t)y + 4, "X", 0xFFFFFF);

  // Save button
  draw_rect((uint32_t)(x + w - 62), (uint32_t)y + 2, 18, 12, 0x103010);
  draw_string_8x8((uint32_t)(x + w - 59), (uint32_t)y + 4, "S", 0xFFFFFF);

  // Scroll
  draw_rect((uint32_t)(x + w - 40), (uint32_t)y + 2, 8, 12, 0x101018);
  draw_string_8x8((uint32_t)(x + w - 39), (uint32_t)y + 4, "^", 0xD0D6E0);
  draw_rect((uint32_t)(x + w - 30), (uint32_t)y + 2, 8, 12, 0x101018);
  draw_string_8x8((uint32_t)(x + w - 29), (uint32_t)y + 4, "v", 0xD0D6E0);

  uint32_t tx = (uint32_t)x + 8u;
  uint32_t ty = (uint32_t)y + 24u;
  uint32_t max_cols = (w > 16) ? (uint32_t)((w - 16) / 8u) : 0u;
  uint32_t max_rows = (h > 32) ? (uint32_t)((h - 32) / 10u) : 0u;

  uint32_t start_i = 0;
  uint32_t skip = g_notepad_scroll;
  for (uint32_t i = 0; i < g_notepad_text_len && skip; i++) {
    if (g_notepad_text[i] == '\n') {
      skip--;
      start_i = i + 1u;
    }
  }

  uint32_t cx = 0;
  uint32_t cy = 0;
  for (uint32_t i = start_i; i < g_notepad_text_len && cy < max_rows; i++) {
    char c = g_notepad_text[i];
    if (c == '\n') {
      cx = 0;
      cy++;
      continue;
    }
    if (c == '\t') {
      cx = (cx + 4u) & ~3u;
      continue;
    }
    if (c < 32 || c > 126)
      c = '.';
    if (cx < max_cols) {
      char t[2] = {c, 0};
      draw_string_8x8(tx + cx * 8u, ty + cy * 10u, t, 0xFFFFFF);
      cx++;
    }
  }
}

static void draw_explorer_window(const wm_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 0x0E0E10);
  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 16, 0x14141A);
  draw_rect((uint32_t)x, (uint32_t)y + 16, (uint32_t)w, 1, 0x2A3140);
  draw_string_8x8((uint32_t)x + 6, (uint32_t)y + 4, "Explorer", 0xFFFFFF);

  // Close button
  draw_rect((uint32_t)(x + w - 20), (uint32_t)y + 2, 16, 12, 0x401010);
  draw_string_8x8((uint32_t)(x + w - 16), (uint32_t)y + 4, "X", 0xFFFFFF);

  // Scroll buttons
  draw_rect((uint32_t)(x + w - 40), (uint32_t)y + 2, 8, 12, 0x101018);
  draw_string_8x8((uint32_t)(x + w - 39), (uint32_t)y + 4, "^", 0xD0D6E0);
  draw_rect((uint32_t)(x + w - 30), (uint32_t)y + 2, 8, 12, 0x101018);
  draw_string_8x8((uint32_t)(x + w - 29), (uint32_t)y + 4, "v", 0xD0D6E0);

  if (!g_explorer_open)
    return;

  uint32_t list_y = (uint32_t)y + 24u;
  uint32_t row_h = 10u;
  uint32_t max_rows = (h > 34) ? (uint32_t)((h - 34) / (int32_t)row_h) : 0u;
  uint32_t total = simplefs_file_count();
  uint32_t start = g_explorer_scroll;
  if (start > total)
    start = total;
  uint32_t count = total - start;
  if (count > max_rows)
    count = max_rows;

  for (uint32_t i = 0; i < count; i++) {
    char name[64];
    name[0] = 0;
    uint32_t idx = start + i;
    if (simplefs_file_name_at(idx, name, (uint32_t)sizeof(name))) {
      uint32_t fg = 0xD0D6E0;
      if (idx == g_explorer_selected) {
        draw_rect((uint32_t)x + 6u, list_y + i * row_h - 1u, (uint32_t)w - 12u,
                  row_h, 0x182030);
        fg = 0xFFFFFF;
      }
      draw_string_8x8((uint32_t)x + 8u, list_y + i * row_h, name, fg);
    }
  }
}

static void draw_viewer_window(const wm_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 0x0E0E10);
  draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 16, 0x14141A);
  draw_rect((uint32_t)x, (uint32_t)y + 16, (uint32_t)w, 1, 0x2A3140);
  draw_string_8x8((uint32_t)x + 6, (uint32_t)y + 4, "View", 0xFFFFFF);
  if (g_viewer_name[0]) {
    draw_string_8x8((uint32_t)x + 52u, (uint32_t)y + 4, g_viewer_name, 0xD0D6E0);
  }

  // Close button
  draw_rect((uint32_t)(x + w - 20), (uint32_t)y + 2, 16, 12, 0x401010);
  draw_string_8x8((uint32_t)(x + w - 16), (uint32_t)y + 4, "X", 0xFFFFFF);

  if (!g_viewer_open)
    return;

  uint32_t tx = (uint32_t)x + 8u;
  uint32_t ty = (uint32_t)y + 24u;
  uint32_t max_cols = (w > 16) ? (uint32_t)((w - 16) / 8u) : 0u;
  uint32_t max_rows = (h > 32) ? (uint32_t)((h - 32) / 10u) : 0u;

  // Apply vertical scroll by skipping N newlines.
  uint32_t start_i = 0;
  uint32_t skip = g_viewer_scroll;
  for (uint32_t i = 0; i < g_viewer_text_len && skip; i++) {
    if (g_viewer_text[i] == '\n') {
      skip--;
      start_i = i + 1u;
    }
  }

  uint32_t cx = 0;
  uint32_t cy = 0;
  for (uint32_t i = start_i; i < g_viewer_text_len && cy < max_rows; i++) {
    char c = g_viewer_text[i];
    if (c == '\n') {
      cx = 0;
      cy++;
      continue;
    }
    if (c == '\t') {
      cx = (cx + 4u) & ~3u;
      continue;
    }
    if (c < 32 || c > 126)
      c = '.';
    if (cx < max_cols) {
      char t[2] = {c, 0};
      draw_string_8x8(tx + cx * 8u, ty + cy * 10u, t, 0xFFFFFF);
      cx++;
    }
  }
}

static int32_t g_last_cursor_x;
static int32_t g_last_cursor_y;
static uint32_t g_last_cursor_buttons;

static void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c);
static void blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t c, uint8_t a);
static void blend_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t radius, uint32_t c, uint8_t a);
static void draw_glyph_8x8(uint32_t x, uint32_t y, uint8_t ch, uint32_t fg);
static void draw_string_8x8(uint32_t x, uint32_t y, const char *s,
                            uint32_t fg);
static void draw_background(void);
static void draw_text_panel(void);
static void draw_taskbar(void);
static void draw_start_menu(void);
static void draw_shell_window(const wm_window_t *win);
static void draw_explorer_window(const wm_window_t *win);
static void draw_viewer_window(const wm_window_t *win);
static void draw_notepad_window(const wm_window_t *win);
static void draw_desktop_icons(void);
static void draw_widgets(void);

static void blit_rect(uint32_t *dst, const uint32_t *src, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h) {
  if (!g_bi || !dst || !src)
    return;
  if (x >= g_bi->width || y >= g_bi->height)
    return;
  if (x + w > g_bi->width)
    w = g_bi->width - x;
  if (y + h > g_bi->height)
    h = g_bi->height - y;

  for (uint32_t yy = 0; yy < h; yy++) {
    uint32_t *drow = &dst[(y + yy) * g_bi->pitch + x];
    const uint32_t *srow = &src[(y + yy) * g_bi->pitch + x];
    for (uint32_t xx = 0; xx < w; xx++) {
      drow[xx] = srow[xx];
    }
  }
}

static inline void put_px(uint32_t x, uint32_t y, uint32_t c) {
  if (!g_bi)
    return;
  if (x >= g_bi->width || y >= g_bi->height)
    return;
  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;
  dst[y * g_bi->pitch + x] = c;
}

static void draw_shell_window(const wm_window_t *win) {
  if (!g_bi)
    return;

  if (!win)
    return;

  uint32_t bar_h = 32;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;

  int32_t x0 = win->x;
  int32_t y0 = win->y;
  int32_t w0 = win->w;
  int32_t h0 = win->h;

  int32_t min_w = 160;
  int32_t min_h = 96;
  if (w0 < min_w)
    w0 = min_w;
  if (h0 < min_h)
    h0 = min_h;

  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x0 + w0 > (int32_t)g_bi->width)
    x0 = (int32_t)g_bi->width - w0;
  if (y0 + h0 > ((int32_t)g_bi->height - (int32_t)bar_h))
    y0 = ((int32_t)g_bi->height - (int32_t)bar_h) - h0;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;

  uint32_t x = (uint32_t)x0;
  uint32_t y = (uint32_t)y0;
  uint32_t w = (uint32_t)w0;
  uint32_t h = (uint32_t)h0;

  draw_rect(x, y, w, h, 0x0B0B0B);
  draw_rect(x, y, w, 1, 0x404040);
  draw_rect(x, y + h - 1, w, 1, 0x101010);
  draw_rect(x, y, 1, h, 0x404040);
  draw_rect(x + w - 1, y, 1, h, 0x101010);

  draw_rect(x, y, w, 16, 0x202020);
  draw_rect(x, y + 16, w, 1, 0x303030);
  draw_string_8x8(x + 8, y + 4, "Shell", 0xE0E0E0);

  // Close button
  if (w >= 24) {
    uint32_t bx = x + w - 20;
    uint32_t by = y + 2;
    draw_rect(bx, by, 16, 12, 0x303030);
    draw_rect(bx, by, 16, 1, 0x404040);
    draw_rect(bx, by + 11, 16, 1, 0x101010);
    draw_string_8x8(bx + 4, by + 2, "X", 0xE0E0E0);
  }

  uint32_t cols = (w - 16) / 8u;
  uint32_t rows = (h - 32) / 8u;
  if (cols == 0 || rows == 0)
    return;

  uint32_t max_chars = cols * rows;
  uint32_t start = 0;
  if (g_shell_out_len > max_chars) {
    start = g_shell_out_len - max_chars;
  }

  uint32_t cx = 0;
  uint32_t cy = 0;
  for (uint32_t i = start; i < g_shell_out_len; i++) {
    char c = g_shell_out[i];
    if (c == '\n') {
      cx = 0;
      cy++;
      if (cy >= rows)
        break;
      continue;
    }
    if (cx < cols && cy < rows) {
      draw_glyph_8x8(x + 8 + cx * 8u, y + 24 + cy * 8u, (uint8_t)c, 0xE0E0E0);
    }
    cx++;
    if (cx >= cols) {
      cx = 0;
      cy++;
      if (cy >= rows)
        break;
    }
  }

  // Input line at bottom
  draw_rect(x, y + h - 16, w, 16, 0x151515);
  draw_rect(x, y + h - 16, w, 1, 0x303030);
  draw_string_8x8(x + 8, y + h - 12, g_shell_line, 0xFFFFFF);
}

static void draw_taskbar(void) {
  if (!g_bi)
    return;

  uint32_t bar_h = 32;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;
  uint32_t y = g_bi->height - bar_h;

  blend_rect(0, y, g_bi->width, bar_h, 0x0A0D14, 170);
  draw_rect(0, y, g_bi->width, 1, 0x2A3140);

  uint32_t sy = y + 6;
  uint32_t sh = bar_h - 12;

  uint32_t search_w = 260;
  if (search_w + 16 > g_bi->width)
    search_w = g_bi->width > 16 ? (g_bi->width - 16) : 1;
  blend_round_rect(8, sy, search_w, sh, 10, g_start_open ? 0x0F1622 : 0x0B111C,
                   210);
  draw_rect(8, sy, search_w, 1, 0x2A3140);
  draw_rect(8, sy + sh - 1, search_w, 1, 0x0B0E14);
  draw_string_8x8(16, sy + 6, "Search or run a command", 0xA0A8B5);

  uint32_t dock_w = 420;
  if (dock_w + 32 > g_bi->width)
    dock_w = g_bi->width > 32 ? (g_bi->width - 32) : 1;
  uint32_t dock_x = (g_bi->width - dock_w) / 2u;
  blend_round_rect(dock_x, sy, dock_w, sh, 12, 0x0B111C, 210);
  draw_rect(dock_x, sy, dock_w, 1, 0x2A3140);
  draw_rect(dock_x, sy + sh - 1, dock_w, 1, 0x0B0E14);

  uint32_t icon = 20;
  uint32_t gap = 10;
  uint32_t ix = dock_x + 16;
  uint32_t iy = sy + ((sh > icon) ? ((sh - icon) / 2u) : 0u);
  for (uint32_t i = 0; i < 7; i++) {
    uint32_t c = 0x2D80FF;
    if (i == 1)
      c = 0xFFB000;
    else if (i == 2)
      c = 0x00C2FF;
    else if (i == 3)
      c = 0xE8505B;
    else if (i == 4)
      c = 0x8B5CF6;
    else if (i == 5)
      c = 0x22C55E;
    else if (i == 6)
      c = 0xE0E0E0;
    draw_rect(ix, iy, icon, icon, c);
    draw_rect(ix, iy, icon, 1, 0xFFFFFF);
    ix += icon + gap;
    if (ix + icon + 16 > dock_x + dock_w)
      break;
  }
}

static void draw_start_menu(void) {
  if (!g_bi)
    return;
  if (!g_start_open)
    return;

  uint32_t bar_h = 32;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;

  uint32_t x = 8;
  uint32_t w = 360;
  uint32_t h = 220;
  if (w + 16 > g_bi->width)
    w = g_bi->width > 16 ? (g_bi->width - 16) : 1;
  uint32_t y = (g_bi->height - bar_h) - h - 8;
  if ((int32_t)y < 8)
    y = 8;

  blend_round_rect(x, y, w, h, 10, 0x0B111C, 210);
  draw_rect(x, y, w, 1, 0x2A3140);
  draw_rect(x, y + h - 1, w, 1, 0x0B0E14);

  draw_string_8x8(x + 12, y + 14, "Apps", 0xE0E0E0);
  draw_rect(x + 12, y + 30, w - 24, 1, 0x2A3140);

  uint32_t row_y = y + 44;
  draw_string_8x8(x + 16, row_y, "Shell", 0xFFFFFF);
  row_y += 24;
  draw_string_8x8(x + 16, row_y, "Explorer", 0xFFFFFF);
  row_y += 24;
  draw_string_8x8(x + 16, row_y, "Notepad", 0xFFFFFF);
  row_y += 24;
  draw_string_8x8(x + 16, row_y, "Help", 0xFFFFFF);
  row_y += 24;
  draw_string_8x8(x + 16, row_y, "Clear", 0xFFFFFF);
}

static void draw_desktop_icons(void) {
  if (!g_bi)
    return;

  uint32_t x = 24;
  uint32_t y = 140;
  uint32_t icon = 36;
  uint32_t step = 86;

  for (uint32_t i = 0; i < 4; i++) {
    uint32_t c = 0xE0E0E0;
    if (i == 0)
      c = 0x2D80FF;
    else if (i == 1)
      c = 0x00C2FF;
    else if (i == 2)
      c = 0xFFB000;
    else if (i == 3)
      c = 0x22C55E;
    draw_rect(x, y + i * step, icon, icon, c);
    draw_rect(x, y + i * step, icon, 1, 0xFFFFFF);
    draw_rect(x, y + i * step + icon, icon, 1, 0x0B0E14);
  }

  draw_string_8x8(x - 4, y + 44, "File", 0xD0D6E0);
  draw_string_8x8(x - 4, y + step + 44, "Web", 0xD0D6E0);
  draw_string_8x8(x - 4, y + 2u * step + 44, "Music", 0xD0D6E0);
  draw_string_8x8(x - 4, y + 3u * step + 44, "Bin", 0xD0D6E0);
}

static void draw_widgets(void) {
  if (!g_bi)
    return;

  uint32_t bar_h = 32;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;

  uint32_t w = 340;
  uint32_t x = (g_bi->width > (w + 24)) ? (g_bi->width - w - 24) : 8;
  uint32_t y = 96;

  uint32_t card_h1 = 110;
  uint32_t card_h2 = 110;
  uint32_t card_h3 = 180;
  if (y + card_h1 + 16 > g_bi->height - bar_h)
    return;

  blend_round_rect(x, y, w, card_h1, 14, 0x0B111C, 200);
  draw_rect(x, y, w, 1, 0x2A3140);
  draw_rect(x, y + card_h1 - 1, w, 1, 0x0B0E14);
  draw_string_8x8(x + 16, y + 16, "Weather", 0xE0E0E0);
  draw_string_8x8(x + 16, y + 40, "Sunny", 0xFFFFFF);
  draw_string_8x8(x + 240, y + 40, "73F", 0xFFFFFF);

  y += card_h1 + 16;
  blend_round_rect(x, y, w, card_h2, 14, 0x0B111C, 200);
  draw_rect(x, y, w, 1, 0x2A3140);
  draw_rect(x, y + card_h2 - 1, w, 1, 0x0B0E14);
  draw_string_8x8(x + 16, y + 16, "System", 0xE0E0E0);
  draw_string_8x8(x + 16, y + 44, "CPU  38%", 0xFFFFFF);
  draw_string_8x8(x + 16, y + 68, "RAM  22%", 0xFFFFFF);

  y += card_h2 + 16;
  if (y + card_h3 > g_bi->height - bar_h)
    return;
  blend_round_rect(x, y, w, card_h3, 14, 0x0B111C, 200);
  draw_rect(x, y, w, 1, 0x2A3140);
  draw_rect(x, y + card_h3 - 1, w, 1, 0x0B0E14);
  draw_string_8x8(x + 16, y + 16, "Calendar", 0xE0E0E0);
  draw_string_8x8(x + 16, y + 44, "April 2024", 0xFFFFFF);
  draw_rect(x + 16, y + 72, w - 32, 1, 0x2A3140);
  draw_string_8x8(x + 16, y + 92, "23  Team Meeting 11:00", 0xD0D6E0);
  draw_string_8x8(x + 16, y + 116, "24  Project Deadline 3:00", 0xD0D6E0);
}

static void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c) {
  if (!g_bi)
    return;
  if (x >= g_bi->width || y >= g_bi->height)
    return;
  uint32_t x2 = x + w;
  uint32_t y2 = y + h;
  if (x2 > g_bi->width)
    x2 = g_bi->width;
  if (y2 > g_bi->height)
    y2 = g_bi->height;

  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;

  for (uint32_t yy = y; yy < y2; yy++) {
    uint32_t *row = &dst[yy * g_bi->pitch];
    for (uint32_t xx = x; xx < x2; xx++) {
      row[xx] = c;
    }
  }
}

static inline uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t a) {
  uint32_t inv = (uint32_t)(255u - (uint32_t)a);
  uint32_t dr = (dst >> 16) & 0xFFu;
  uint32_t dg = (dst >> 8) & 0xFFu;
  uint32_t db = dst & 0xFFu;
  uint32_t sr = (src >> 16) & 0xFFu;
  uint32_t sg = (src >> 8) & 0xFFu;
  uint32_t sb = src & 0xFFu;

  uint32_t r = (dr * inv + sr * (uint32_t)a) / 255u;
  uint32_t g = (dg * inv + sg * (uint32_t)a) / 255u;
  uint32_t b = (db * inv + sb * (uint32_t)a) / 255u;
  return (r << 16) | (g << 8) | b;
}

static void blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t c, uint8_t a) {
  if (!g_bi)
    return;
  if (a == 0)
    return;
  if (x >= g_bi->width || y >= g_bi->height)
    return;
  uint32_t x2 = x + w;
  uint32_t y2 = y + h;
  if (x2 > g_bi->width)
    x2 = g_bi->width;
  if (y2 > g_bi->height)
    y2 = g_bi->height;

  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;
  for (uint32_t yy = y; yy < y2; yy++) {
    uint32_t *row = &dst[yy * g_bi->pitch];
    for (uint32_t xx = x; xx < x2; xx++) {
      row[xx] = blend_rgb(row[xx], c, a);
    }
  }
}

static void blend_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t radius, uint32_t c, uint8_t a) {
  if (!g_bi)
    return;
  if (a == 0)
    return;
  if (radius == 0) {
    blend_rect(x, y, w, h, c, a);
    return;
  }
  if (x >= g_bi->width || y >= g_bi->height)
    return;

  uint32_t x2 = x + w;
  uint32_t y2 = y + h;
  if (x2 > g_bi->width)
    x2 = g_bi->width;
  if (y2 > g_bi->height)
    y2 = g_bi->height;

  uint32_t rr = radius * radius;
  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;
  for (uint32_t yy = y; yy < y2; yy++) {
    uint32_t *row = &dst[yy * g_bi->pitch];
    for (uint32_t xx = x; xx < x2; xx++) {
      uint32_t inside = 1;

      if (xx < x + radius && yy < y + radius) {
        int32_t dx = (int32_t)xx - (int32_t)(x + radius);
        int32_t dy = (int32_t)yy - (int32_t)(y + radius);
        inside = ((uint32_t)(dx * dx + dy * dy) <= rr);
      } else if (xx >= x2 - radius && yy < y + radius) {
        int32_t dx = (int32_t)xx - (int32_t)(x2 - radius - 1);
        int32_t dy = (int32_t)yy - (int32_t)(y + radius);
        inside = ((uint32_t)(dx * dx + dy * dy) <= rr);
      } else if (xx < x + radius && yy >= y2 - radius) {
        int32_t dx = (int32_t)xx - (int32_t)(x + radius);
        int32_t dy = (int32_t)yy - (int32_t)(y2 - radius - 1);
        inside = ((uint32_t)(dx * dx + dy * dy) <= rr);
      } else if (xx >= x2 - radius && yy >= y2 - radius) {
        int32_t dx = (int32_t)xx - (int32_t)(x2 - radius - 1);
        int32_t dy = (int32_t)yy - (int32_t)(y2 - radius - 1);
        inside = ((uint32_t)(dx * dx + dy * dy) <= rr);
      }

      if (inside) {
        row[xx] = blend_rgb(row[xx], c, a);
      }
    }
  }
}

static void draw_string_8x8(uint32_t x, uint32_t y, const char *s,
                            uint32_t fg) {
  uint32_t cx = x;
  while (s && *s) {
    char c = *s++;
    if (c == '\n') {
      y += 8;
      cx = x;
      continue;
    }
    if (c == '\t') {
      cx += 32;
      continue;
    }
    draw_glyph_8x8(cx, y, (uint8_t)c, fg);
    cx += 8;
  }
}

static int hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w,
                    int32_t h) {
  if (px < x || py < y)
    return 0;
  if (px >= (x + w) || py >= (y + h))
    return 0;
  return 1;
}

static uint32_t shell_u32_to_dec(char out[12], uint32_t v) {
  char tmp[12];
  uint32_t n = 0;
  if (v == 0) {
    out[0] = '0';
    out[1] = 0;
    return 1;
  }
  while (v && n < 11) {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  for (uint32_t i = 0; i < n; i++) {
    out[i] = tmp[n - 1u - i];
  }
  out[n] = 0;
  return n;
}

static void shell_append_hex_u16(uint16_t v) {
  char hex[5];
  for (int i = 0; i < 4; i++) {
    uint8_t nib = (uint8_t)((v >> ((3 - i) * 4)) & 0xFu);
    char c = (nib < 10) ? (char)('0' + nib) : (char)('A' + (nib - 10));
    hex[i] = c;
  }
  hex[4] = 0;
  shell_append(hex);
}

static void shell_run_specs(void) {
  const hw_profile_t *p = hw_profile_get();
  const hw_tuning_t *t = hw_tuning_get();
  char buf[12];

  shell_append("cpu_vendor: ");
  shell_append(p->cpu_vendor[0] ? p->cpu_vendor : "?");
  shell_append("\n");

  shell_append("cpu_brand: ");
  shell_append(p->cpu_brand[0] ? p->cpu_brand : "?");
  shell_append("\n");

  shell_append("ram_usable: ");
  uint32_t ram_mb = (uint32_t)(p->ram_usable_bytes / (1024ull * 1024ull));
  shell_u32_to_dec(buf, ram_mb);
  shell_append(buf);
  shell_append(" MB\n");

  shell_append("fb: ");
  shell_u32_to_dec(buf, p->fb_width);
  shell_append(buf);
  shell_append("x");
  shell_u32_to_dec(buf, p->fb_height);
  shell_append(buf);
  shell_append(" pitch=");
  shell_u32_to_dec(buf, p->fb_pitch);
  shell_append(buf);
  shell_append("\n");

  shell_append("gpu_pci: ");
  if (p->has_pci_display) {
    shell_append("vid=");
    shell_append_hex_u16(p->gpu_vendor_id);
    shell_append(" did=");
    shell_append_hex_u16(p->gpu_device_id);
    shell_append(" sub=");
    shell_append_hex_u16((uint16_t)p->gpu_subclass);
    shell_append(" if=");
    shell_append_hex_u16((uint16_t)p->gpu_prog_if);
    shell_append("\n");
  } else {
    shell_append("none\n");
  }

  shell_append("tune: fps=");
  shell_u32_to_dec(buf, t->target_fps);
  shell_append(buf);
  shell_append(" backbuf=");
  shell_append(t->allow_backbuffer ? "on" : "off");
  shell_append(" heap_slack=");
  shell_u32_to_dec(buf, (uint32_t)(t->heap_slack_bytes / 1024ull));
  shell_append(buf);
  shell_append(" KB\n");
}

static uint32_t shell_next_rand(void) {
  // LCG
  g_rand_state = (g_rand_state * 1664525u) + 1013904223u;
  return g_rand_state;
}

static void shell_append(const char *s) {
  while (s && *s) {
    if (g_shell_out_len + 1u >= (uint32_t)sizeof(g_shell_out)) {
      for (uint32_t i = 0; i < (uint32_t)sizeof(g_shell_out) / 2u; i++) {
        g_shell_out[i] = g_shell_out[i + (uint32_t)sizeof(g_shell_out) / 2u];
      }
      g_shell_out_len = (uint32_t)sizeof(g_shell_out) / 2u;
    }
    g_shell_out[g_shell_out_len++] = *s++;
  }
}

static int streq_n(const char *a, const char *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return 0;
    if (a[i] == 0)
      return 1;
  }
  return 1;
}

static void shell_run_line(void) {
  for (uint32_t i = 0; i < g_shell_line_len; i++) {
    char t[2] = {g_shell_line[i], 0};
    shell_append(t);
  }
  shell_append("\n");
  if (g_shell_line_len == 0) {
    shell_append("> ");
    return;
  }

  if (g_shell_line_len == 4 && streq_n(g_shell_line, "help", 4)) {
    shell_append("help clear cls echo mem ver uptime pos rand specs ring3 fs explorer view\n");
  } else if ((g_shell_line_len == 5 && streq_n(g_shell_line, "clear", 5)) ||
             (g_shell_line_len == 3 && streq_n(g_shell_line, "cls", 3))) {
    g_shell_out_len = 0;
  } else if (g_shell_line_len >= 4 && streq_n(g_shell_line, "echo", 4)) {
    uint32_t i = 4;
    while (i < g_shell_line_len && (g_shell_line[i] == ' '))
      i++;
    for (; i < g_shell_line_len; i++) {
      char c = g_shell_line[i];
      char t[2] = {c, 0};
      shell_append(t);
    }
    shell_append("\n");
  } else if (g_shell_line_len == 3 && streq_n(g_shell_line, "mem", 3)) {
    shell_append("heap ok\n");
  } else if (g_shell_line_len == 3 && streq_n(g_shell_line, "ver", 3)) {
    shell_append("tiny64 ui\n");
  } else if (g_shell_line_len == 6 && streq_n(g_shell_line, "uptime", 6)) {
    char buf[12];
    shell_u32_to_dec(buf, g_tick);
    shell_append(buf);
    shell_append(" ticks\n");
  } else if (g_shell_line_len == 3 && streq_n(g_shell_line, "pos", 3)) {
    char buf[12];
    shell_append("x=");
    shell_u32_to_dec(buf, (uint32_t)g_last_ptr.x);
    shell_append(buf);
    shell_append(" y=");
    shell_u32_to_dec(buf, (uint32_t)g_last_ptr.y);
    shell_append(buf);
    shell_append("\n");
  } else if (g_shell_line_len == 4 && streq_n(g_shell_line, "rand", 4)) {
    char buf[12];
    shell_u32_to_dec(buf, shell_next_rand());
    shell_append(buf);
    shell_append("\n");
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "specs", 5)) {
    shell_run_specs();
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "ring3", 5)) {
    shell_append("launching ring3...\n");
    launch_ring3_task();
  } else if (g_shell_line_len == 2 && streq_n(g_shell_line, "fs", 2)) {
    shell_append("mounting fs...\n");
    if (simplefs_init()) {
      simplefs_list();
    } else {
      shell_append("fs init failed\n");
    }
  } else if (g_shell_line_len == 8 && streq_n(g_shell_line, "explorer", 8)) {
    shell_append("opening explorer...\n");
    if (simplefs_init()) {
      g_explorer_open = 1;
      g_app_open[APP_EXPLORER] = 1;
      g_app_focus = APP_EXPLORER;
    } else {
      shell_append("fs init failed\n");
    }
  } else if (g_shell_line_len >= 5 && streq_n(g_shell_line, "view ", 5)) {
    uint32_t i = 5;
    while (i < g_shell_line_len && g_shell_line[i] == ' ')
      i++;
    if (i >= g_shell_line_len) {
      shell_append("view: missing name\n");
    } else {
      if (simplefs_init()) {
        char name[64];
        uint32_t n = 0;
        while (i < g_shell_line_len && n + 1u < (uint32_t)sizeof(name)) {
          name[n++] = g_shell_line[i++];
        }
        name[n] = 0;
        uint32_t out_len = 0;
        if (simplefs_read_file(name, g_viewer_text, (uint32_t)sizeof(g_viewer_text) - 1u,
                               &out_len)) {
          if (out_len >= (uint32_t)sizeof(g_viewer_text))
            out_len = (uint32_t)sizeof(g_viewer_text) - 1u;
          g_viewer_text[out_len] = 0;
          g_viewer_text_len = out_len;
          // Copy name
          for (uint32_t k = 0; k + 1u < (uint32_t)sizeof(g_viewer_name) && name[k]; k++) {
            g_viewer_name[k] = name[k];
            g_viewer_name[k + 1u] = 0;
          }
          g_viewer_open = 1;
          g_app_open[APP_VIEWER] = 1;
          g_app_focus = APP_VIEWER;
        } else {
          shell_append("view: read failed\n");
        }
      } else {
        shell_append("fs init failed\n");
      }
    }
  } else if (g_shell_line_len == 7 && streq_n(g_shell_line, "notepad", 7)) {
    shell_append("opening notepad...\n");
    g_app_open[APP_NOTEPAD] = 1;
    g_app_focus = APP_NOTEPAD;
  } else if (g_shell_line_len == 4 && streq_n(g_shell_line, "wall", 4)) {
    shell_append("wall: loading new.png...\n");
    if (!g_bg) {
      shell_append("wall: no bg buffer\n");
    } else if (!simplefs_init()) {
      shell_append("wall: fs init failed\n");
    } else {
      uint32_t sz = 0;
      if (!simplefs_file_size("new.png", &sz) || sz == 0) {
        shell_append("wall: new.png not found\n");
      } else if (sz > (8u * 1024u * 1024u)) {
        shell_append("wall: too big\n");
      } else {
        void *png_buf = kmalloc(sz);
        if (!png_buf) {
          shell_append("wall: no mem\n");
        } else {
          uint32_t out_len = 0;
          if (!simplefs_read_file("new.png", png_buf, sz, &out_len) || out_len != sz) {
            shell_append("wall: read failed\n");
          } else {
            ramfs_put_copy("new.png", png_buf, sz);

            uint32_t pw = 0, ph = 0;
            uint8_t *pix = 0;
            if (!png_decode_rgba((const uint8_t *)png_buf, sz, &pw, &ph, &pix)) {
              shell_append("wall: png decode failed\n");
            } else {
              // Clear bg to black
              for (uint32_t y = 0; y < g_bi->height; y++) {
                uint32_t *row = &g_bg[y * g_bi->pitch];
                for (uint32_t x = 0; x < g_bi->width; x++)
                  row[x] = 0x000000;
              }

              // Center image (no scaling v1)
              uint32_t ox = 0;
              uint32_t oy = 0;
              if (pw < g_bi->width)
                ox = (g_bi->width - pw) / 2u;
              if (ph < g_bi->height)
                oy = (g_bi->height - ph) / 2u;

              uint32_t maxw = pw;
              uint32_t maxh = ph;
              if (ox + maxw > g_bi->width)
                maxw = g_bi->width - ox;
              if (oy + maxh > g_bi->height)
                maxh = g_bi->height - oy;

              for (uint32_t y = 0; y < maxh; y++) {
                uint32_t *row = &g_bg[(oy + y) * g_bi->pitch];
                for (uint32_t x = 0; x < maxw; x++) {
                  uint32_t si = (y * pw + x) * 4u;
                  uint8_t r = pix[si + 0];
                  uint8_t g = pix[si + 1];
                  uint8_t b = pix[si + 2];
                  // ignore alpha for now
                  row[ox + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
              }

              shell_append("wall: applied\n");
            }
          }
        }
      }
    }
  } else {
    shell_append("unknown\n");
  }

  g_shell_line_len = 0;
  g_shell_line[0] = 0;
  shell_append("> ");
}

static void draw_background(void) {
  if (!g_bi)
    return;

  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;

  uint32_t h = g_bi->height ? g_bi->height : 1;
  uint32_t w = g_bi->width;
  uint32_t horizon = (h * 52u) / 100u;
  if (horizon >= h)
    horizon = h - 1;

  for (uint32_t y = 0; y < h; y++) {
    uint32_t *row = &dst[y * g_bi->pitch];
    uint32_t r, g, b;
    if (y < horizon) {
      uint32_t t = (y * 255u) / (horizon ? horizon : 1);
      r = 10 + (t * 18u) / 255u;
      g = 14 + (t * 26u) / 255u;
      b = 24 + (t * 60u) / 255u;
    } else {
      uint32_t t = ((y - horizon) * 255u) / ((h - horizon) ? (h - horizon) : 1);
      r = 10 + (t * 10u) / 255u;
      g = 30 + (t * 60u) / 255u;
      b = 50 + (t * 80u) / 255u;
    }
    uint32_t base = (r << 16) | (g << 8) | b;
    for (uint32_t x = 0; x < w; x++) {
      row[x] = base;
    }
  }

  for (uint32_t x = 0; x < w; x++) {
    uint32_t y0 = horizon;
    dst[y0 * g_bi->pitch + x] = 0xB0C8FF;
  }

  uint32_t star_count = 220;
  for (uint32_t i = 0; i < star_count; i++) {
    uint32_t r0 = shell_next_rand();
    uint32_t sx = r0 % (w ? w : 1);
    uint32_t sy = (shell_next_rand() % (horizon ? horizon : 1));
    uint32_t c = 0xE8F0FF;
    if ((r0 & 7u) == 0)
      c = 0xFFFFFF;
    put_px(sx, sy, c);
  }

  for (uint32_t i = 0; i < 3; i++) {
    uint32_t cx = (w * (55u + i * 7u)) / 100u;
    uint32_t cy = (h * (22u + i * 3u)) / 100u;
    uint32_t rr = 140 - i * 30;
    for (int32_t dy = -(int32_t)rr; dy <= (int32_t)rr; dy++) {
      for (int32_t dx = -(int32_t)rr; dx <= (int32_t)rr; dx++) {
        uint32_t dd = (uint32_t)(dx * dx + dy * dy);
        if (dd <= (rr * rr)) {
          uint32_t px = (uint32_t)((int32_t)cx + dx);
          uint32_t py = (uint32_t)((int32_t)cy + dy);
          if (px < w && py < horizon) {
            uint32_t shade = 0xB0C8FF;
            if (dd > (uint32_t)((rr * rr) * 7u / 10u))
              shade = 0x6A88C8;
            put_px(px, py, shade);
          }
        }
      }
    }
  }
}

static void draw_glyph_8x8(uint32_t x, uint32_t y, uint8_t ch, uint32_t fg) {
  uint8_t *font = _binary_CGA_F08_start;
  uint8_t *glyph = &font[((uint32_t)ch) * 8u];
  for (uint32_t gy = 0; gy < 8; gy++) {
    uint8_t row = glyph[gy];
    for (uint32_t gx = 0; gx < 8; gx++) {
      if (row & (uint8_t)(1u << (7u - gx))) {
        put_px(x + gx, y + gy, fg);
      }
    }
  }
}

static void draw_text_panel(void) {
  if (!g_bi)
    return;

  uint32_t panel_h = 96;
  if (panel_h > g_bi->height)
    panel_h = g_bi->height;

  draw_rect(8, 8, g_bi->width - 16, panel_h, 0x101010);
  draw_rect(8, 8, g_bi->width - 16, 1, 0x303030);
  draw_rect(8, 8 + panel_h - 1, g_bi->width - 16, 1, 0x303030);

  uint32_t cols = (g_bi->width - 32) / 8u;
  uint32_t rows = (panel_h - 16) / 8u;
  if (cols == 0 || rows == 0)
    return;

  uint32_t max_chars = cols * rows;
  uint32_t start = 0;
  if (g_text_len > max_chars) {
    start = g_text_len - max_chars;
  }

  uint32_t cx = 0;
  uint32_t cy = 0;
  for (uint32_t i = start; i < g_text_len; i++) {
    char c = g_text[i];
    if (c == '\n') {
      cx = 0;
      cy++;
      if (cy >= rows)
        break;
      continue;
    }
    if (c == '\t') {
      cx = (cx + 4u) & ~3u;
      if (cx >= cols) {
        cx = 0;
        cy++;
      }
      if (cy >= rows)
        break;
      continue;
    }

    if (cx < cols && cy < rows) {
      draw_glyph_8x8(16 + cx * 8u, 16 + cy * 8u, (uint8_t)c, 0xE0E0E0);
    }
    cx++;
    if (cx >= cols) {
      cx = 0;
      cy++;
      if (cy >= rows)
        break;
    }
  }
}

// 16x16 cursor mask: 1=draw, 0=transparent
static const uint16_t k_cursor_bits[16] = {
    0b1000000000000000,
    0b1100000000000000,
    0b1110000000000000,
    0b1111000000000000,
    0b1111100000000000,
    0b1111110000000000,
    0b1111111000000000,
    0b1111111100000000,
    0b1111111110000000,
    0b1111111000000000,
    0b1110011100000000,
    0b1100011100000000,
    0b1000001110000000,
    0b0000001110000000,
    0b0000000111000000,
    0b0000000111000000,
};

static void draw_cursor(int32_t x, int32_t y, uint32_t fg, uint32_t outline) {
  // Outline first
  for (int32_t yy = -1; yy <= 1; yy++) {
    for (int32_t xx = -1; xx <= 1; xx++) {
      if (xx == 0 && yy == 0)
        continue;
      for (uint32_t cy = 0; cy < 16; cy++) {
        uint16_t row = k_cursor_bits[cy];
        for (uint32_t cx = 0; cx < 16; cx++) {
          if (row & (1u << (15 - cx))) {
            put_px((uint32_t)(x + (int32_t)cx + xx),
                   (uint32_t)(y + (int32_t)cy + yy), outline);
          }
        }
      }
    }
  }

  for (uint32_t cy = 0; cy < 16; cy++) {
    uint16_t row = k_cursor_bits[cy];
    for (uint32_t cx = 0; cx < 16; cx++) {
      if (row & (1u << (15 - cx))) {
        put_px((uint32_t)(x + (int32_t)cx), (uint32_t)(y + (int32_t)cy), fg);
      }
    }
  }
}

void desktop_init(BootInfo *bootInfo) {
  g_bi = bootInfo;
  g_tick = 0;
  g_back = 0;
  g_bg = 0;
  g_text_len = 0;
  g_start_open = 0;
  g_show_shell = 1;
  g_shell_out_len = 0;
  g_shell_line_len = 0;
  g_rand_state = 0xC001D00Du;
  g_last_ptr.x = 0;
  g_last_ptr.y = 0;
  g_last_ptr.dx = 0;
  g_last_ptr.dy = 0;
  g_last_ptr.buttons = 0;
  g_last_ptr.is_absolute = 0;
  g_shell_dragging = 0;
  g_shell_drag_off_x = 0;
  g_shell_drag_off_y = 0;
  g_explorer_open = 0;
  g_viewer_open = 0;
  g_explorer_dragging = 0;
  g_viewer_dragging = 0;
  g_viewer_name[0] = 0;
  g_viewer_text[0] = 0;
  g_viewer_text_len = 0;
  g_explorer_scroll = 0;
  g_viewer_scroll = 0;
  g_explorer_selected = 0xFFFFFFFFu;
  g_explorer_last_click_tick = 0;
  g_explorer_last_click_index = 0xFFFFFFFFu;

  for (uint32_t i = 0; i < APP_COUNT; i++) {
    g_app_open[i] = 0;
    g_app_z[i] = (app_id_t)i;
  }
  g_app_z_len = 0;
  g_app_focus = APP_SHELL;

  g_notepad_dragging = 0;
  g_notepad_drag_off_x = 0;
  g_notepad_drag_off_y = 0;
  g_notepad_name[0] = 0;
  g_notepad_text[0] = 0;
  g_notepad_text_len = 0;
  g_notepad_scroll = 0;

  if (g_bi && g_bi->pitch && g_bi->height) {
    uint64_t pixels = (uint64_t)g_bi->pitch * (uint64_t)g_bi->height;
    uint64_t bytes = pixels * 4u;
    if (bytes <= 0xFFFFFFFFu) {
      if (hw_tuning_allow_backbuffer(bytes)) {
        g_back = (uint32_t *)kmalloc((uint32_t)bytes);
        g_bg = (uint32_t *)kmalloc((uint32_t)bytes);
      } else {
        const hw_profile_t *hw = hw_profile_get();
        uint64_t min_free = 16ull * 1024ull * 1024ull;
        if (hw && hw->ram_usable_bytes && (bytes + min_free) <= hw->ram_usable_bytes) {
          g_back = (uint32_t *)kmalloc((uint32_t)bytes);
          g_bg = (uint32_t *)kmalloc((uint32_t)bytes);
        }
      }
    }
  }

  g_last_cursor_x = -1000;
  g_last_cursor_y = -1000;
  g_last_cursor_buttons = 0;

  input_init(bootInfo->width, bootInfo->height);
  shell_append("> ");

  g_shell_win.x = 16;
  g_shell_win.y = 48;
  g_shell_win.w = (int32_t)((g_bi->width * 2u) / 3u);
  if (g_shell_win.w < 220)
    g_shell_win.w = 220;
  g_shell_win.h = (int32_t)(g_bi->height / 2u);
  if (g_shell_win.h < 140)
    g_shell_win.h = 140;

  g_explorer_win.x = g_shell_win.x + 40;
  g_explorer_win.y = g_shell_win.y + 40;
  g_explorer_win.w = 320;
  g_explorer_win.h = 220;

  g_viewer_win.x = g_explorer_win.x + 40;
  g_viewer_win.y = g_explorer_win.y + 40;
  g_viewer_win.w = 420;
  g_viewer_win.h = 280;

  // Make sure the notepad window is always on-screen.
  if (g_notepad_win.x + g_notepad_win.w > (int32_t)g_bi->width)
    g_notepad_win.x = 8;
  if (g_notepad_win.y + g_notepad_win.h > (int32_t)g_bi->height)
    g_notepad_win.y = 48;

  g_notepad_win.x = g_viewer_win.x + 40;
  g_notepad_win.y = g_viewer_win.y + 40;
  g_notepad_win.w = 520;
  g_notepad_win.h = 320;

  // Default notepad file name
  {
    const char *def = "note.txt";
    for (uint32_t i = 0; i + 1u < (uint32_t)sizeof(g_notepad_name) && def[i]; i++) {
      g_notepad_name[i] = def[i];
      g_notepad_name[i + 1u] = 0;
    }
  }

  if (!g_bi)
    return;

  if (g_back && g_bg) {
    // Render background once into g_bg and reuse it every frame.
    uint32_t saved = g_rand_state;
    uint32_t *saved_back = g_back;
    g_back = g_bg;
    g_rand_state = 0xC001D00Du;
    draw_background();
    g_rand_state = saved;
    g_back = saved_back;

    // Initialize the presented frame.
    blit_rect(g_back, g_bg, 0, 0, g_bi->width, g_bi->height);
    draw_desktop_icons();
    draw_widgets();
    if (g_show_shell) {
      draw_shell_window(&g_shell_win);
    }
    if (g_explorer_open) {
      draw_explorer_window(&g_explorer_win);
    }
    if (g_viewer_open) {
      draw_viewer_window(&g_viewer_win);
    }
    if (g_app_open[APP_NOTEPAD]) {
      draw_notepad_window(&g_notepad_win);
    }
    draw_taskbar();
    draw_start_menu();
    {
      input_pointer_state_t p = input_pointer_get();
      uint32_t fg = 0xFFFFFF;
      uint32_t outline = 0x000000;
      if (p.buttons & 1u) {
        fg = 0xFFCC00;
      } else if (p.buttons & 2u) {
        fg = 0x00CCFF;
      }
      draw_cursor(p.x, p.y, fg, outline);
    }
    blit_rect(g_bi->framebuffer, g_back, 0, 0, g_bi->width, g_bi->height);
  } else {
    draw_background();
  }
}

void desktop_tick(void) {
  if (!g_bi)
    return;

  static uint32_t prev_buttons = 0;
  input_pointer_state_t p0 = input_pointer_get();
  g_last_ptr = p0;
  uint32_t buttons = p0.buttons;
  uint32_t pressed = (buttons) & ~prev_buttons;
  prev_buttons = buttons;

  uint8_t ui_changed = 0;

  uint32_t kbd_budget = 32;
  while (kbd_budget--) {
    char c = 0;
    if (!input_kbd_pop_char(&c)) {
      break;
    }

    // Route keyboard input to focused app.
    if (g_app_focus == APP_NOTEPAD && g_app_open[APP_NOTEPAD]) {
      if (c == '\b') {
        if (g_notepad_text_len) {
          g_notepad_text_len--;
          g_notepad_text[g_notepad_text_len] = 0;
          ui_changed = 1;
        }
      } else if (c == '\n') {
        if (g_notepad_text_len + 1u < (uint32_t)sizeof(g_notepad_text)) {
          g_notepad_text[g_notepad_text_len++] = '\n';
          g_notepad_text[g_notepad_text_len] = 0;
          ui_changed = 1;
        }
      } else if (c >= 32 && c <= 126) {
        if (g_notepad_text_len + 1u < (uint32_t)sizeof(g_notepad_text)) {
          g_notepad_text[g_notepad_text_len++] = c;
          g_notepad_text[g_notepad_text_len] = 0;
          ui_changed = 1;
        }
      }
    } else {
      if (c == '\b') {
        if (g_shell_line_len) {
          g_shell_line_len--;
          g_shell_line[g_shell_line_len] = 0;
          ui_changed = 1;
        }
      } else {
        if (c == '\n') {
          shell_run_line();
          ui_changed = 1;
        } else if (c >= 32 && c <= 126) {
          if (g_shell_line_len + 1u < (uint32_t)sizeof(g_shell_line)) {
            g_shell_line[g_shell_line_len++] = c;
            g_shell_line[g_shell_line_len] = 0;
            ui_changed = 1;
          }
        }
      }
    }
  }

  // Click handling
  if (pressed & 1u) {
    uint32_t bar_h = 32;
    if (bar_h > g_bi->height)
      bar_h = g_bi->height;
    int32_t tb_y = (int32_t)g_bi->height - (int32_t)bar_h;

    // Start button hit
    if (hit_test(p0.x, p0.y, 8, tb_y + 4, 72, (int32_t)bar_h - 8)) {
      g_start_open = (uint8_t)(g_start_open ? 0u : 1u);
      ui_changed = 1;
      g_shell_dragging = 0;
    } else if (g_start_open) {
      int32_t mx = 8;
      int32_t mw = 160;
      int32_t mh = 144;
      int32_t my = tb_y - mh;
      if (hit_test(p0.x, p0.y, mx, my, mw, mh)) {
        int32_t ry = p0.y - my;
        if (ry >= 8 && ry < 24) {
          g_show_shell = 1;
          g_app_open[APP_SHELL] = 1;
          g_app_focus = APP_SHELL;
          g_start_open = 0;
          ui_changed = 1;
        } else if (ry >= 32 && ry < 48) {
          // Explorer
          if (simplefs_init()) {
            g_explorer_open = 1;
            g_app_open[APP_EXPLORER] = 1;
            g_app_focus = APP_EXPLORER;
          } else {
            shell_append("fs init failed\n> ");
          }
          g_start_open = 0;
          ui_changed = 1;
        } else if (ry >= 56 && ry < 72) {
          // Notepad
          g_app_open[APP_NOTEPAD] = 1;
          g_app_focus = APP_NOTEPAD;
          g_start_open = 0;
          ui_changed = 1;
        } else if (ry >= 80 && ry < 96) {
          shell_append("\nhelp clear echo mem\n> ");
          g_start_open = 0;
          ui_changed = 1;
        } else if (ry >= 104 && ry < 120) {
          g_shell_out_len = 0;
          g_shell_line_len = 0;
          g_shell_line[0] = 0;
          shell_append("> ");
          g_start_open = 0;
          ui_changed = 1;
        }
      } else {
        g_start_open = 0;
        ui_changed = 1;
      }
    }

    // Taskbar app buttons: click to focus/open
    {
      uint32_t bx = g_bi->width > 420 ? (g_bi->width - 420) : 88;
      uint32_t by = (uint32_t)tb_y + 6u;
      uint32_t bw = 78;
      uint32_t bh = 20;
      uint32_t gap = 6;
      for (uint32_t i = 0; i < APP_COUNT; i++) {
        if (hit_test(p0.x, p0.y, (int32_t)bx, (int32_t)by, (int32_t)bw, (int32_t)bh)) {
          g_app_open[i] = 1;
          g_app_focus = (app_id_t)i;
          if (i == APP_SHELL)
            g_show_shell = 1;
          if (i == APP_EXPLORER)
            g_explorer_open = 1;
          if (i == APP_VIEWER)
            g_viewer_open = 1;
          ui_changed = 1;
          break;
        }
        bx += bw + gap;
        if (bx + bw + 8 > g_bi->width)
          break;
      }
    }

    // Explorer window interactions
    if (g_explorer_open) {
      int32_t wx = g_explorer_win.x;
      int32_t wy = g_explorer_win.y;
      int32_t ww = g_explorer_win.w;
      int32_t wh = g_explorer_win.h;

      // Close button
      if (hit_test(p0.x, p0.y, wx + ww - 20, wy + 2, 16, 12)) {
        g_explorer_open = 0;
        g_explorer_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 40, wy + 2, 8, 12)) {
        // scroll up
        if (g_explorer_scroll)
          g_explorer_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 30, wy + 2, 8, 12)) {
        // scroll down
        g_explorer_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 16)) {
        g_explorer_dragging = 1;
        g_explorer_drag_off_x = p0.x - wx;
        g_explorer_drag_off_y = p0.y - wy;
        g_app_open[APP_EXPLORER] = 1;
        g_app_focus = APP_EXPLORER;
        ui_changed = 1;
      } else {
        // Click in list area -> open viewer
        int32_t list_x = wx + 8;
        int32_t list_y = wy + 24;
        int32_t list_w = ww - 16;
        int32_t list_h = wh - 32;
        if (hit_test(p0.x, p0.y, list_x, list_y, list_w, list_h)) {
          uint32_t row_h = 10u;
          uint32_t idx = g_explorer_scroll +
                         (uint32_t)((p0.y - list_y) / (int32_t)row_h);
          char name[64];
          name[0] = 0;
          if (simplefs_file_name_at(idx, name, (uint32_t)sizeof(name))) {
            // single click selects
            g_explorer_selected = idx;
            ui_changed = 1;

            // double click opens
            uint32_t dt = g_tick - g_explorer_last_click_tick;
            if (g_explorer_last_click_index == idx && dt < 25u) {
              uint32_t out_len = 0;
              if (simplefs_read_file(name, g_viewer_text,
                                     (uint32_t)sizeof(g_viewer_text) - 1u,
                                     &out_len)) {
                if (out_len >= (uint32_t)sizeof(g_viewer_text))
                  out_len = (uint32_t)sizeof(g_viewer_text) - 1u;
                g_viewer_text[out_len] = 0;
                g_viewer_text_len = out_len;
                g_viewer_scroll = 0;
                // copy name
                for (uint32_t k = 0; k < (uint32_t)sizeof(g_viewer_name); k++) {
                  g_viewer_name[k] = 0;
                }
                for (uint32_t k = 0;
                     k + 1u < (uint32_t)sizeof(g_viewer_name) && name[k]; k++) {
                  g_viewer_name[k] = name[k];
                  g_viewer_name[k + 1u] = 0;
                }
                g_viewer_open = 1;
                g_app_open[APP_VIEWER] = 1;
                g_app_focus = APP_VIEWER;
                ui_changed = 1;
              }
            }

            g_explorer_last_click_tick = g_tick;
            g_explorer_last_click_index = idx;
          }
        }
      }
    }

    if (g_viewer_open) {
      int32_t wx = g_viewer_win.x;
      int32_t wy = g_viewer_win.y;
      int32_t ww = g_viewer_win.w;
      // Close
      if (hit_test(p0.x, p0.y, wx + ww - 20, wy + 2, 16, 12)) {
        g_viewer_open = 0;
        g_viewer_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 40, wy + 2, 8, 12)) {
        if (g_viewer_scroll)
          g_viewer_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 30, wy + 2, 8, 12)) {
        g_viewer_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 16)) {
        g_viewer_dragging = 1;
        g_viewer_drag_off_x = p0.x - wx;
        g_viewer_drag_off_y = p0.y - wy;
        g_app_open[APP_VIEWER] = 1;
        g_app_focus = APP_VIEWER;
        ui_changed = 1;
      }
    }

    // Notepad interactions
    if (g_app_open[APP_NOTEPAD]) {
      int32_t wx = g_notepad_win.x;
      int32_t wy = g_notepad_win.y;
      int32_t ww = g_notepad_win.w;
      int32_t wh = g_notepad_win.h;

      if (hit_test(p0.x, p0.y, wx + ww - 20, wy + 2, 16, 12)) {
        g_app_open[APP_NOTEPAD] = 0;
        g_notepad_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 62, wy + 2, 18, 12)) {
        // Save
        if (simplefs_init()) {
          if (simplefs_write_file(g_notepad_name, g_notepad_text, g_notepad_text_len)) {
            shell_append("notepad: saved\n> ");
          } else {
            shell_append("notepad: save failed\n> ");
          }
        } else {
          shell_append("notepad: fs init failed\n> ");
        }
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 40, wy + 2, 8, 12)) {
        if (g_notepad_scroll)
          g_notepad_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 30, wy + 2, 8, 12)) {
        g_notepad_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 16)) {
        g_notepad_dragging = 1;
        g_notepad_drag_off_x = p0.x - wx;
        g_notepad_drag_off_y = p0.y - wy;
        g_app_focus = APP_NOTEPAD;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, wh)) {
        // Click in body focuses notepad
        g_app_focus = APP_NOTEPAD;
        ui_changed = 1;
      }
    }
  }

  // Window manager interactions (shell window)
  if (pressed & 1u) {
    if (g_show_shell) {
      int32_t wx = g_shell_win.x;
      int32_t wy = g_shell_win.y;
      int32_t ww = g_shell_win.w;
      int32_t wh = g_shell_win.h;

      // Close button
      if (hit_test(p0.x, p0.y, wx + ww - 20, wy + 2, 16, 12)) {
        g_show_shell = 0;
        g_shell_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 16)) {
        g_shell_dragging = 1;
        g_shell_drag_off_x = p0.x - wx;
        g_shell_drag_off_y = p0.y - wy;
        ui_changed = 1;
      }
    }
  }

  if (!(buttons & 1u)) {
    g_shell_dragging = 0;
    g_explorer_dragging = 0;
    g_viewer_dragging = 0;
    g_notepad_dragging = 0;
  }

  if (g_shell_dragging && (p0.dx || p0.dy)) {
    g_shell_win.x = p0.x - g_shell_drag_off_x;
    g_shell_win.y = p0.y - g_shell_drag_off_y;
    ui_changed = 1;
  }

  if (g_explorer_dragging && (p0.dx || p0.dy)) {
    g_explorer_win.x = p0.x - g_explorer_drag_off_x;
    g_explorer_win.y = p0.y - g_explorer_drag_off_y;
    ui_changed = 1;
  }

  if (g_viewer_dragging && (p0.dx || p0.dy)) {
    g_viewer_win.x = p0.x - g_viewer_drag_off_x;
    g_viewer_win.y = p0.y - g_viewer_drag_off_y;
    ui_changed = 1;
  }

  if (g_notepad_dragging && (p0.dx || p0.dy)) {
    g_notepad_win.x = p0.x - g_notepad_drag_off_x;
    g_notepad_win.y = p0.y - g_notepad_drag_off_y;
    ui_changed = 1;
  }

  if (!g_back || !g_bg) {
    // Fallback: old full redraw path if buffers aren't available.
    draw_background();
    draw_desktop_icons();
    draw_widgets();
    if (g_show_shell) {
      draw_shell_window(&g_shell_win);
    }
    if (g_explorer_open) {
      draw_explorer_window(&g_explorer_win);
    }
    if (g_viewer_open) {
      draw_viewer_window(&g_viewer_win);
    }
    if (g_app_open[APP_NOTEPAD]) {
      draw_notepad_window(&g_notepad_win);
    }
    draw_taskbar();
    draw_start_menu();
    // Draw cursor directly to front buffer.
    input_pointer_state_t p = input_pointer_get();

    uint32_t fg = 0xFFFFFF;
    uint32_t outline = 0x000000;
    if (p.buttons & 1u) {
      fg = 0xFFCC00;
    } else if (p.buttons & 2u) {
      fg = 0x00CCFF;
    }
    draw_cursor(p.x, p.y, fg, outline);

    if ((p.buttons & 1u) && (p.dx || p.dy)) {
      int32_t ix = p.x + 18;
      int32_t iy = p.y + 18;
      if (ix < 0)
        ix = 0;
      if (iy < 0)
        iy = 0;
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 5, 0xFFCC00);
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 1, 0x000000);
      draw_rect((uint32_t)ix, (uint32_t)iy + 4, 5, 1, 0x000000);
    }

    g_last_cursor_x = p.x;
    g_last_cursor_y = p.y;
    g_last_cursor_buttons = p.buttons;
  } else {
    (void)ui_changed;

    // Full redraw into backbuffer and present.
    // Background is cached; just copy it in.
    blit_rect(g_back, g_bg, 0, 0, g_bi->width, g_bi->height);
    draw_desktop_icons();
    draw_widgets();
    if (g_show_shell) {
      draw_shell_window(&g_shell_win);
    }
    if (g_explorer_open) {
      draw_explorer_window(&g_explorer_win);
    }
    if (g_viewer_open) {
      draw_viewer_window(&g_viewer_win);
    }
    draw_taskbar();
    draw_start_menu();

    input_pointer_state_t p = input_pointer_get();

    uint32_t fg = 0xFFFFFF;
    uint32_t outline = 0x000000;
    if (p.buttons & 1u) {
      fg = 0xFFCC00;
    } else if (p.buttons & 2u) {
      fg = 0x00CCFF;
    }

    draw_cursor(p.x, p.y, fg, outline);
    if ((p.buttons & 1u) && (p.dx || p.dy)) {
      int32_t ix = p.x + 18;
      int32_t iy = p.y + 18;
      if (ix < 0)
        ix = 0;
      if (iy < 0)
        iy = 0;
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 5, 0xFFCC00);
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 1, 0x000000);
      draw_rect((uint32_t)ix, (uint32_t)iy + 4, 5, 1, 0x000000);
    }

    blit_rect(g_bi->framebuffer, g_back, 0, 0, g_bi->width, g_bi->height);

    g_last_cursor_x = p.x;
    g_last_cursor_y = p.y;
    g_last_cursor_buttons = p.buttons;
  }
  g_tick++;
}
