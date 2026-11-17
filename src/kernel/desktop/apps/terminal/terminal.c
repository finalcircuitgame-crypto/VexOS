#include "terminal.h"
#include "../../graphics/graphics.h"

#define TERMINAL_OUTPUT_CAPACITY 4096u
#define TERMINAL_LINE_CAPACITY 256u

static desktop_window_t g_window;
static char g_output[TERMINAL_OUTPUT_CAPACITY];
static uint32_t g_output_length;
static char g_line[TERMINAL_LINE_CAPACITY];
static uint32_t g_line_length;
static uint8_t g_dragging;
static int32_t g_drag_offset_x;
static int32_t g_drag_offset_y;

static uint8_t terminal_hit(int32_t px, int32_t py, int32_t x, int32_t y,
                            int32_t width, int32_t height) {
  return (uint8_t)(px >= x && py >= y && px < x + width && py < y + height);
}

void terminal_init(uint32_t screen_width, uint32_t screen_height) {
  g_output_length = 0;
  g_output[0] = 0;
  g_line_length = 0;
  g_line[0] = 0;
  g_dragging = 0;
  g_drag_offset_x = 0;
  g_drag_offset_y = 0;

  g_window.x = 16;
  g_window.y = 48;
  g_window.w = (int32_t)((screen_width * 2u) / 3u);
  if (g_window.w < 220)
    g_window.w = 220;
  g_window.h = (int32_t)(screen_height / 2u);
  if (g_window.h < 140)
    g_window.h = 140;
}

desktop_window_t *terminal_window(void) { return &g_window; }

void terminal_append(const char *text) {
  while (text && *text) {
    if (g_output_length + 1u >= (uint32_t)sizeof(g_output)) {
      for (uint32_t i = 0; i < (uint32_t)sizeof(g_output) / 2u; i++)
        g_output[i] = g_output[i + (uint32_t)sizeof(g_output) / 2u];
      g_output_length = (uint32_t)sizeof(g_output) / 2u;
    }
    g_output[g_output_length++] = *text++;
  }
  g_output[g_output_length] = 0;
}

const char *terminal_line(void) { return g_line; }
uint32_t terminal_line_length(void) { return g_line_length; }

void terminal_clear_line(void) {
  g_line_length = 0;
  g_line[0] = 0;
}

void terminal_clear_output(void) {
  g_output_length = 0;
  g_output[0] = 0;
}

uint8_t terminal_handle_char(char value) {
  if (value == '\b') {
    if (!g_line_length)
      return 0;
    g_line[--g_line_length] = 0;
    return 1;
  }

  if (value == '\n')
    return 2;

  if (value < 32 || value > 126 ||
      g_line_length + 1u >= (uint32_t)sizeof(g_line))
    return 0;

  g_line[g_line_length++] = value;
  g_line[g_line_length] = 0;
  return 1;
}

terminal_click_result_t terminal_handle_click(int32_t x, int32_t y) {
  int32_t wx = g_window.x;
  int32_t wy = g_window.y;
  int32_t ww = g_window.w;
  int32_t wh = g_window.h;

  if (terminal_hit(x, y, wx + ww - 24, wy + 5, 20, 20)) {
    g_dragging = 0;
    return TERMINAL_CLICK_CLOSE;
  }

  if (terminal_hit(x, y, wx, wy, ww, 30)) {
    g_dragging = 1;
    g_drag_offset_x = x - wx;
    g_drag_offset_y = y - wy;
    return TERMINAL_CLICK_FOCUS;
  }

  if (terminal_hit(x, y, wx, wy, ww, wh))
    return TERMINAL_CLICK_FOCUS;

  return TERMINAL_CLICK_NONE;
}

void terminal_pointer_move(int32_t x, int32_t y) {
  if (!g_dragging)
    return;
  g_window.x = x - g_drag_offset_x;
  g_window.y = y - g_drag_offset_y;
}

void terminal_pointer_release(void) { g_dragging = 0; }

void terminal_draw(const terminal_render_context_t *context) {
  if (!context || !context->window || !context->draw_chrome)
    return;

  const desktop_window_t *win = context->window;
  if (win->w < 40 || win->h < 60)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  context->draw_chrome(win, "Terminal", "VexOS Shell", context->focused, 1, 0);
  gfx_draw_string((uint32_t)(x + 14), (uint32_t)(y + 40),
                  "VEXOS TERMINAL  /  LOCAL SESSION", 0x7FA4C7u);

  uint32_t px = (uint32_t)x + 10u;
  uint32_t py = (uint32_t)y + 56u;
  uint32_t pw = w > 20 ? (uint32_t)w - 20u : 1u;
  uint32_t ph = h > 94 ? (uint32_t)h - 94u : 1u;
  gfx_blend_round_rect(px, py, pw, ph, 5u, 0x091321u, 255);
  gfx_draw_rect(px + 1u, py + 1u, pw - 2u, 1u, 0x1C344Du);

  uint32_t tx = px + 12u;
  uint32_t ty = py + 12u;
  gfx_font_family_t saved_family = gfx_get_font_family();
  uint8_t saved_scale = gfx_get_font_scale();
  uint8_t saved_bold = gfx_get_font_bold();
  uint16_t saved_size = gfx_get_font_size();

  gfx_set_font(GFX_FONT_DEJAVU_MONO, saved_scale, 0u);
  gfx_set_font_size(14u);
  uint32_t cell_width = gfx_measure_string("M");
  uint32_t cell_height = gfx_get_font_line_height();
  if (!cell_width) cell_width = 9u;
  if (!cell_height) cell_height = 16u;
  uint32_t cols = pw > 24u ? (pw - 24u) / cell_width : 0u;
  uint32_t rows = ph > 20u ? (ph - 20u) / cell_height : 0u;

  if (cols && rows) {
    uint32_t capacity = cols * rows;
    uint32_t start = g_output_length > capacity
                         ? g_output_length - capacity : 0u;
    uint32_t cx = 0;
    uint32_t cy = 0;

    for (uint32_t i = start; i < g_output_length && cy < rows; i++) {
      char value = g_output[i];
      if (value == '\n') {
        cx = 0;
        cy++;
        continue;
      }

      if (cx < cols && value >= 32 && value <= 126)
        gfx_draw_glyph(tx + cx * cell_width, ty + cy * cell_height,
                       (uint8_t)value, 0xD7E7F7u);

      cx++;
      if (cx >= cols) {
        cx = 0;
        cy++;
      }
    }
  }

  gfx_set_font(saved_family, saved_scale, saved_bold);
  gfx_set_font_size(14u);

  uint32_t input_y = (uint32_t)(y + h - 30);
  gfx_blend_round_rect((uint32_t)(x + 10), input_y, (uint32_t)(w - 20),
                       24u, 5u, 0x1A2C40u, 255);
  gfx_draw_rect((uint32_t)(x + 18), input_y + 1u,
                (uint32_t)(w - 36), 1u, 0x35516Du);
  uint32_t input_x = (uint32_t)(x + 22);
  uint32_t input_text_y = input_y + 4u;
  gfx_draw_string(input_x, input_text_y, ">", 0x79E0B0u);
  uint32_t line_x = input_x + gfx_measure_string("> ");
  gfx_draw_string(line_x, input_text_y, g_line, 0xF1F6FCu);

  if (context->focused && ((context->tick / 30u) % 2u == 0u)) {
    uint32_t cursor_x = line_x + gfx_measure_string(g_line);
    uint32_t cursor_height = gfx_get_font_line_height();
    if (cursor_height > 2u)
      cursor_height -= 2u;
    gfx_draw_rect(cursor_x, input_y + 5u, 2u, cursor_height, 0x79E0B0u);
  }

  gfx_set_font(saved_family, saved_scale, saved_bold);
  gfx_set_font_size(saved_size);
}
