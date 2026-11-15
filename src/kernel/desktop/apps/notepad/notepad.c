#include "notepad.h"
#include "../../graphics/graphics.h"
#include "../../../../include/simplefs.h"

#define NOTEPAD_TEXT_CAPACITY 4096u
#define NOTEPAD_LINE_GUTTER 42u
#define NOTEPAD_LINE_HEIGHT 16u

static desktop_window_t g_window;
static char g_filename[64];
static char g_text[NOTEPAD_TEXT_CAPACITY];
static uint32_t g_text_length;
static uint32_t g_scroll;
static uint8_t g_dirty;
static uint8_t g_dragging;
static int32_t g_drag_offset_x;
static int32_t g_drag_offset_y;
static uint32_t g_screen_width;
static uint32_t g_screen_height;

static uint32_t notepad_line_count(void) {
  uint32_t lines = 1;
  for (uint32_t i = 0; i < g_text_length; ++i)
    if (g_text[i] == '\n')
      ++lines;
  return lines;
}

/* Count rendered rows, including soft-wrapped lines and tab expansion. */
static uint32_t notepad_visual_row_count(uint32_t columns) {
  if (!columns)
    return 1;

  uint32_t rows = 1;
  uint32_t column = 0;
  for (uint32_t i = 0; i < g_text_length; ++i) {
    char value = g_text[i];
    if (value == '\n') {
      ++rows;
      column = 0;
      continue;
    }

    uint32_t advance = value == '\t' ? 4u - (column & 3u) : 1u;
    while (advance--) {
      if (column >= columns) {
        ++rows;
        column = 0;
      }
      ++column;
    }
  }
  return rows;
}

static uint32_t notepad_visible_rows(void) {
  uint32_t height = g_window.h > 120 ? (uint32_t)g_window.h - 120u : 0u;
  return height / NOTEPAD_LINE_HEIGHT;
}

static uint32_t notepad_max_scroll(void) {
  uint32_t columns = g_window.w > 72 ?
      ((uint32_t)g_window.w - 72u) / 8u : 1u;
  uint32_t rows = notepad_visual_row_count(columns);
  uint32_t visible = notepad_visible_rows();
  return rows > visible ? rows - visible : 0u;
}

static void notepad_keep_caret_visible(void) {
  g_scroll = notepad_max_scroll();
}

static void notepad_load_saved_file(void) {
  uint32_t file_size = 0;
  uint32_t bytes_read = 0;

  g_text[0] = 0;
  g_text_length = 0;
  g_dirty = 0;

  if (!simplefs_init() ||
      !simplefs_file_size(g_filename, &file_size) ||
      file_size >= NOTEPAD_TEXT_CAPACITY)
    return;

  if (file_size &&
      (!simplefs_read_file(g_filename, g_text, file_size, &bytes_read) ||
       bytes_read != file_size)) {
    g_text[0] = 0;
    return;
  }

  g_text_length = bytes_read;
  g_text[g_text_length] = 0;
}

void notepad_init(uint32_t screen_width, uint32_t screen_height) {
  g_screen_width = screen_width;
  g_screen_height = screen_height;
  g_filename[0] = 0;
  g_text[0] = 0;
  g_text_length = 0;
  g_scroll = 0;
  g_dirty = 0;
  g_dragging = 0;
  g_drag_offset_x = 0;
  g_drag_offset_y = 0;

  const char *name = "note.txt";
  uint32_t i = 0;
  while (i + 1u < sizeof(g_filename) && name[i]) {
    g_filename[i] = name[i];
    ++i;
  }
  g_filename[i] = 0;

  g_window.w = (int32_t)(screen_width > 720u ? 660u :
      (screen_width > 48u ? screen_width - 48u : screen_width));
  g_window.h = (int32_t)(screen_height > 560u ? 480u :
      (screen_height > 80u ? screen_height - 80u : screen_height));
  g_window.x = ((int32_t)screen_width - g_window.w) / 2;
  g_window.y = ((int32_t)screen_height - g_window.h) / 2;

  notepad_load_saved_file();
  notepad_keep_caret_visible();
}

desktop_window_t *notepad_window(void) { return &g_window; }
const char *notepad_filename(void) { return g_filename; }
const char *notepad_text(void) { return g_text; }
uint32_t notepad_text_length(void) { return g_text_length; }
uint32_t notepad_scroll(void) { return g_scroll; }
uint8_t notepad_is_dirty(void) { return g_dirty; }

uint8_t notepad_handle_char(char value) {
  if (value == '\b' || value == 127) {
    if (!g_text_length)
      return 0;
    g_text[--g_text_length] = 0;
    g_dirty = 1;
    notepad_keep_caret_visible();
    return 1;
  }

  if (value == '\r')
    value = '\n';
  if (value != '\n' && value != '\t' && (value < 32 || value > 126))
    return 0;
  if (g_text_length + 1u >= sizeof(g_text))
    return 0;

  g_text[g_text_length++] = value;
  g_text[g_text_length] = 0;
  g_dirty = 1;
  notepad_keep_caret_visible();
  return 1;
}

void notepad_scroll_up(void) {
  if (g_scroll)
    --g_scroll;
}

void notepad_scroll_down(void) {
  uint32_t maximum = notepad_max_scroll();
  if (g_scroll < maximum)
    ++g_scroll;
}

static uint8_t notepad_hit(int32_t px, int32_t py, int32_t x, int32_t y,
                           int32_t w, int32_t h) {
  return (uint8_t)(px >= x && py >= y && px < x + w && py < y + h);
}

notepad_click_result_t notepad_handle_click(int32_t x, int32_t y) {
  int32_t wx = g_window.x;
  int32_t wy = g_window.y;
  int32_t ww = g_window.w;

  if (notepad_hit(x, y, wx + ww - 24, wy + 5, 20, 20)) {
    g_dragging = 0;
    return NOTEPAD_CLICK_CLOSE;
  }

  if (notepad_hit(x, y, wx + ww - 86, wy + 5, 20, 20)) {
    if (!simplefs_init())
      return NOTEPAD_CLICK_FS_FAILED;
    if (!simplefs_write_file(g_filename, g_text, g_text_length))
      return NOTEPAD_CLICK_SAVE_FAILED;
    g_dirty = 0;
    return NOTEPAD_CLICK_SAVE_OK;
  }

  /* Compact editor toolbar scroll buttons. */
  if (notepad_hit(x, y, wx + ww - 66, wy + 34, 24, 22)) {
    notepad_scroll_up();
    return NOTEPAD_CLICK_SCROLL;
  }
  if (notepad_hit(x, y, wx + ww - 40, wy + 34, 24, 22)) {
    notepad_scroll_down();
    return NOTEPAD_CLICK_SCROLL;
  }

  if (notepad_hit(x, y, wx, wy, ww, 30)) {
    g_dragging = 1;
    g_drag_offset_x = x - wx;
    g_drag_offset_y = y - wy;
    return NOTEPAD_CLICK_FOCUS;
  }

  if (notepad_hit(x, y, wx, wy, g_window.w, g_window.h))
    return NOTEPAD_CLICK_FOCUS;

  return NOTEPAD_CLICK_NONE;
}

void notepad_pointer_move(int32_t x, int32_t y) {
  if (!g_dragging)
    return;

  int32_t max_x = g_screen_width > (uint32_t)g_window.w ?
      (int32_t)(g_screen_width - (uint32_t)g_window.w) : 0;
  int32_t max_y = g_screen_height > (uint32_t)g_window.h ?
      (int32_t)(g_screen_height - (uint32_t)g_window.h) : 0;
  int32_t next_x = x - g_drag_offset_x;
  int32_t next_y = y - g_drag_offset_y;

  if (next_x < 0) next_x = 0;
  if (next_y < 0) next_y = 0;
  if (next_x > max_x) next_x = max_x;
  if (next_y > max_y) next_y = max_y;
  g_window.x = next_x;
  g_window.y = next_y;
}

void notepad_pointer_release(void) {
  g_dragging = 0;
}

static void draw_line_number(uint32_t x, uint32_t y, uint32_t number) {
  char digits[11];
  uint32_t count = 0;
  do {
    digits[count++] = (char)('0' + (number % 10u));
    number /= 10u;
  } while (number && count < sizeof(digits));

  while (count) {
    char value[2] = { digits[--count], 0 };
    gfx_draw_string(x, y, value, 0x7888A2u);
    x += 8u;
  }
}

static void draw_editor_rows(uint32_t text_x, uint32_t text_y,
                             uint32_t columns, uint32_t rows) {
  if (!columns || !rows)
    return;

  uint32_t visual_row = 0;
  uint32_t column = 0;
  uint32_t logical_line = 1;
  uint32_t row_end = g_scroll + rows;

  draw_line_number(text_x - NOTEPAD_LINE_GUTTER + 6u, text_y,
                   logical_line);

  for (uint32_t i = 0; i < g_text_length && visual_row < row_end; ++i) {
    char value = g_text[i];
    if (value == '\n') {
      ++visual_row;
      ++logical_line;
      column = 0;
      if (visual_row >= g_scroll && visual_row < row_end) {
        uint32_t row_y = text_y +
            (visual_row - g_scroll) * NOTEPAD_LINE_HEIGHT;
        draw_line_number(text_x - NOTEPAD_LINE_GUTTER + 6u, row_y,
                         logical_line);
      }
      continue;
    }

    uint32_t advance = value == '\t' ? 4u - (column & 3u) : 1u;
    while (advance--) {
      if (column >= columns) {
        ++visual_row;
        column = 0;
        if (visual_row >= g_scroll && visual_row < row_end) {
          uint32_t row_y = text_y +
              (visual_row - g_scroll) * NOTEPAD_LINE_HEIGHT;
          gfx_draw_string(text_x - 12u, row_y, ">", 0x536781u);
        }
      }

      if (visual_row >= g_scroll && visual_row < row_end) {
        uint32_t row_y = text_y +
            (visual_row - g_scroll) * NOTEPAD_LINE_HEIGHT;
        char glyph[2] = { value == '\t' ? ' ' : value, 0 };
        gfx_draw_string(text_x + column * 8u, row_y, glyph, 0xD5DEECu);
      }
      ++column;
    }
  }
}

void notepad_draw(const notepad_render_context_t *context) {
  if (!context || !context->window || !context->draw_chrome)
    return;

  const desktop_window_t *win = context->window;
  if (win->w < 40 || win->h < 40)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  char subtitle[72];
  uint32_t n = 0;
  const char *filename = context->filename && context->filename[0] ?
      context->filename : "note.txt";
  while (n + 2u < sizeof(subtitle) && filename[n]) {
    subtitle[n] = filename[n];
    ++n;
  }
  if (g_dirty) {
    subtitle[n++] = ' ';
    subtitle[n++] = '*';
  }
  subtitle[n] = 0;

  context->draw_chrome(win, "Notepad", subtitle, context->focused, 0, 1);

  /* Minimal toolbar: format on the left, bounded scroll controls on the right. */
  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 31),
                (uint32_t)(w - 2), 28u, 0x151F30u);
  gfx_draw_string((uint32_t)(x + 14), (uint32_t)(y + 41),
                  "PLAIN TEXT", 0xAAB9CFu);
  gfx_draw_string((uint32_t)(x + 112), (uint32_t)(y + 41),
                  "UTF-8 SAFE ASCII", 0x62758F);
  gfx_blend_round_rect((uint32_t)(x + w - 66), (uint32_t)(y + 34),
                       24u, 22u, 5u, 0x27364B, 255u);
  gfx_blend_round_rect((uint32_t)(x + w - 40), (uint32_t)(y + 34),
                       24u, 22u, 5u, 0x27364B, 255u);
  gfx_draw_string((uint32_t)(x + w - 58), (uint32_t)(y + 41), "^", 0xE1E8F4u);
  gfx_draw_string((uint32_t)(x + w - 32), (uint32_t)(y + 41), "v", 0xE1E8F4u);

  uint32_t editor_x = (uint32_t)x + 12u;
  uint32_t editor_y = (uint32_t)y + 64u;
  uint32_t editor_w = w > 24 ? (uint32_t)w - 24u : 1u;
  uint32_t editor_h = h > 100 ? (uint32_t)h - 100u : 1u;
  gfx_draw_rect(editor_x, editor_y, editor_w, editor_h, 0x0D1420u);
  gfx_draw_rect(editor_x, editor_y, 1u, editor_h, 0x34445B);
  gfx_draw_rect(editor_x + NOTEPAD_LINE_GUTTER, editor_y + 1u, 1u,
                editor_h > 2u ? editor_h - 2u : 1u, 0x26364A);

  uint32_t text_x = editor_x + NOTEPAD_LINE_GUTTER + 8u;
  uint32_t text_y = editor_y + 10u;
  uint32_t columns = editor_w > NOTEPAD_LINE_GUTTER + 16u ?
      (editor_w - NOTEPAD_LINE_GUTTER - 16u) / 8u : 0u;
  uint32_t rows = editor_h > 20u ?
      (editor_h - 20u) / NOTEPAD_LINE_HEIGHT : 0u;
  draw_editor_rows(text_x, text_y, columns, rows);

  uint32_t status_y = (uint32_t)(y + h - 27);
  gfx_draw_rect((uint32_t)(x + 1), status_y,
                (uint32_t)(w - 2), 26u, 0x151F30u);
  gfx_draw_rect((uint32_t)(x + 1), status_y, (uint32_t)(w - 2), 1u,
                0x2E3B51);

  char count_text[16];
  uint32_t count = g_text_length;
  uint32_t digits = 0;
  char reverse[12];
  do {
    reverse[digits++] = (char)('0' + count % 10u);
    count /= 10u;
  } while (count && digits < sizeof(reverse));
  for (uint32_t i = 0; i < digits; ++i)
    count_text[i] = reverse[digits - i - 1u];
  count_text[digits] = 0;

  gfx_draw_string((uint32_t)(x + 14), status_y + 9u, count_text, 0xAAB9CFu);
  gfx_draw_string((uint32_t)(x + 14 + digits * 8u), status_y + 9u,
                  " chars", 0x71829C);
  const char *status = g_dirty ? "UNSAVED CHANGES" : "ALL CHANGES SAVED";
  uint32_t status_len = 0;
  while (status[status_len])
    ++status_len;
  uint32_t status_x = (uint32_t)(x + w) > status_len * 8u + 18u ?
      (uint32_t)(x + w) - status_len * 8u - 18u : (uint32_t)(x + 14);
  gfx_draw_string(status_x, status_y + 9u, status,
                  g_dirty ? 0xE8B96Fu : 0x73C9A8u);
}
