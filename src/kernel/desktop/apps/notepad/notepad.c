#include "notepad.h"
#include "../../graphics/graphics.h"
#define NOTEPAD_TEXT_CAPACITY 4096u
static desktop_window_t g_window;
static char g_filename[64];
static char g_text[NOTEPAD_TEXT_CAPACITY];
static uint32_t g_text_length;
static uint32_t g_scroll;

void notepad_init(uint32_t screen_width, uint32_t screen_height) {
  g_filename[0] = 0;
  g_text[0] = 0;
  g_text_length = 0;
  g_scroll = 0;
  const char *name = "note.txt";
  uint32_t i = 0;
  while (i + 1u < sizeof(g_filename) && name[i]) {
    g_filename[i] = name[i];
    ++i;
  }
  g_filename[i] = 0;
  g_window.w = (int32_t)(screen_width > 720u ? 660u : (screen_width > 48u ? screen_width - 48u : screen_width));
  g_window.h = (int32_t)(screen_height > 560u ? 480u : (screen_height > 80u ? screen_height - 80u : screen_height));
  g_window.x = ((int32_t)screen_width - g_window.w) / 2 + 54;
  g_window.y = ((int32_t)screen_height - g_window.h) / 2 + 38;
}

desktop_window_t *notepad_window(void) { return &g_window; }
const char *notepad_filename(void) { return g_filename; }
const char *notepad_text(void) { return g_text; }
uint32_t notepad_text_length(void) { return g_text_length; }
uint32_t notepad_scroll(void) { return g_scroll; }

uint8_t notepad_handle_char(char value) {
  if (value == '\b') {
    if (!g_text_length) return 0;
    g_text[--g_text_length] = 0;
    return 1;
  }
  if (value != '\n' && (value < 32 || value > 126)) return 0;
  if (g_text_length + 1u >= sizeof(g_text)) return 0;
  g_text[g_text_length++] = value;
  g_text[g_text_length] = 0;
  return 1;
}

void notepad_scroll_up(void) { if (g_scroll) --g_scroll; }
void notepad_scroll_down(void) { ++g_scroll; }


static void draw_line_number(uint32_t x, uint32_t y, uint32_t number) {
  char digits[11];
  uint32_t count = 0;
  do {
    digits[count++] = (char)('0' + (number % 10u));
    number /= 10u;
  } while (number && count < sizeof(digits));

  while (count) {
    char value[2] = { digits[--count], 0 };
    gfx_draw_string(x, y, value, 0x8EA0B4u);
    x += 8u;
  }
}

void notepad_draw(const notepad_render_context_t *context) {
  if (!context || !context->window || !context->draw_chrome)
    return;

  const desktop_window_t *win = context->window;
  if (win->w < 40 || win->h < 40)
    return;

  int32_t x = win->x, y = win->y, w = win->w, h = win->h;
  const char *filename = context->filename && context->filename[0]
                             ? context->filename : "note.txt";
  context->draw_chrome(win, "Notepad", filename, context->focused, 1, 1);

  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 31),
                (uint32_t)(w - 2), 30u, 0x17263Au);
  gfx_draw_string((uint32_t)(x + 14), (uint32_t)(y + 42),
                  "FILE   EDIT   VIEW", 0xB9C9DAu);
  gfx_draw_string((uint32_t)(x + w - 142), (uint32_t)(y + 42),
                  "PLAIN TEXT", 0x7FA5C8u);

  uint32_t cx = (uint32_t)x + 12u, cy = (uint32_t)y + 66u;
  uint32_t cw = w > 24 ? (uint32_t)w - 24u : 1u;
  uint32_t ch = h > 78 ? (uint32_t)h - 78u : 1u;
  gfx_blend_round_rect(cx, cy, cw, ch, 5u, 0xF4F7FBu, 255);
  if (cw > 2u && ch > 2u) {
    gfx_draw_rect(cx + 1u, cy + 1u, cw - 2u, 1u, 0xFFFFFFu);
    if (ch > 2u) gfx_draw_rect(cx + 34u, cy + 1u, 1u, ch - 2u, 0xDCE5EFu);
  }

  uint32_t tx = cx + 46u, ty = cy + 12u;
  uint32_t cols = w > 72 ? (uint32_t)(w - 72) / 8u : 0u;
  uint32_t rows = h > 100 ? (uint32_t)(h - 100) / 16u : 0u;
  uint32_t start = 0, skip = context->scroll;
  const char *text = context->text ? context->text : "";
  uint32_t length = context->text_length;
  for (uint32_t i = 0; i < length && skip; ++i) {
    if (text[i] == '\n') { --skip; start = i + 1u; }
  }

  uint32_t col = 0, row = 0, line = context->scroll + 1u;
  if (rows) draw_line_number(cx + 10u, ty, line);
  for (uint32_t i = start; i < length && row < rows; ++i) {
    char ch = text[i];
    if (ch == '\n') {
      col = 0; ++row; ++line;
      if (row < rows) draw_line_number(cx + 10u, ty + row * 16u, line);
      continue;
    }
    if (ch == '\t') { col = (col + 4u) & ~3u; continue; }
    if (ch < 32 || ch > 126) ch = '.';
    if (col < cols) {
      char glyph[2] = { ch, 0 };
      gfx_draw_string(tx + col * 8u, ty + row * 16u, glyph, 0x24364Bu);
    }
    ++col;
  }
}
