#include "window_chrome.h"
#include "graphics/graphics.h"

void window_chrome_draw(const desktop_window_t *win, const char *title,
                        const char *subtitle, uint8_t focused,
                        uint8_t has_scroll, uint8_t has_save,
                        window_chrome_icon_fn draw_icon) {
  if (!win || win->w < 40 || win->h < 40)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;
  app_id_t app = APP_SHELL;

  if (title && (title[0] == 'F' || title[0] == 'E'))
    app = APP_EXPLORER;
  else if (title && title[0] == 'V')
    app = APP_VIEWER;
  else if (title && title[0] == 'N')
    app = APP_NOTEPAD;
  else if (title && title[0] == 'S')
    app = APP_SETUP;

  gfx_font_family_t saved_family = gfx_get_font_family();
  uint8_t saved_scale = gfx_get_font_scale();
  uint8_t saved_bold = gfx_get_font_bold();
  uint16_t saved_size = gfx_get_font_size();

  /* Deep frame, clear focused edge, and a separate title surface. */
  gfx_blend_round_rect((uint32_t)(x + 4), (uint32_t)(y + 7),
                       (uint32_t)w, (uint32_t)h, 11u, 0x03050Cu, 175u);
  gfx_blend_round_rect((uint32_t)x, (uint32_t)y, (uint32_t)w,
                       (uint32_t)h, 9u, 0x101626u, 255u);
  gfx_draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 1u,
                focused ? 0x9A88FFu : 0x34405Cu);
  gfx_draw_rect((uint32_t)x, (uint32_t)(y + h - 1), (uint32_t)w, 1u,
                0x2A344B);
  gfx_draw_rect((uint32_t)x, (uint32_t)y, 1u, (uint32_t)h, 0x303B57);
  gfx_draw_rect((uint32_t)(x + w - 1), (uint32_t)y, 1u,
                (uint32_t)h, 0x252F46);

  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 1),
                (uint32_t)(w - 2), 29u,
                focused ? 0x20213Au : 0x191F31u);
  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 29),
                (uint32_t)(w - 2), 1u,
                focused ? 0x554B86u : 0x303A52u);
  if (focused)
    gfx_draw_rect((uint32_t)(x + 12), (uint32_t)(y + 1), 44u, 2u,
                  0xAA9BFFu);

  if (draw_icon)
    draw_icon((uint32_t)(x + 8), (uint32_t)(y + 6), 20u, app);

  /* Use a stronger title and quieter, genuinely measured subtitle. */
  gfx_set_font(saved_family, saved_scale, 1u);
  gfx_set_font_size(18u);
  uint32_t title_width = title ? gfx_measure_string(title) : 0u;
  if (title)
    gfx_draw_string((uint32_t)(x + 36), (uint32_t)(y + 7), title,
                    focused ? 0xF4F3FFu : 0xC6CBE0u);

  gfx_set_font(saved_family, saved_scale, 0u);
  gfx_set_font_size(12u);
  if (subtitle && subtitle[0]) {
    uint32_t subtitle_width = gfx_measure_string(subtitle);
    uint32_t subtitle_x = (uint32_t)(x + 36) + title_width + 12u;
    uint32_t right_limit = (uint32_t)(x + w - 100);
    if (subtitle_x < right_limit &&
        subtitle_width <= right_limit - subtitle_x) {
      gfx_draw_rect(subtitle_x - 6u, (uint32_t)(y + 9), 1u, 12u,
                    0x59617F);
      gfx_draw_string(subtitle_x, (uint32_t)(y + 9), subtitle,
                      0x929CB9u);
    }
  }

  /* Keep the existing hit boxes; control glyphs stay compact. */
  gfx_set_font_size(12u);
  gfx_blend_round_rect((uint32_t)(x + w - 23), (uint32_t)(y + 6),
                       17u, 17u, 6u, 0x873B59u, 255u);
  gfx_draw_string((uint32_t)(x + w - 18), (uint32_t)(y + 10), "x",
                  0xFFF5FAu);

  if (has_scroll) {
    gfx_blend_round_rect((uint32_t)(x + w - 45), (uint32_t)(y + 6),
                         17u, 17u, 6u, 0x303955u, 255u);
    gfx_blend_round_rect((uint32_t)(x + w - 65), (uint32_t)(y + 6),
                         17u, 17u, 6u, 0x303955u, 255u);
    gfx_draw_string((uint32_t)(x + w - 41), (uint32_t)(y + 10), "^",
                    0xE4E6F5u);
    gfx_draw_string((uint32_t)(x + w - 61), (uint32_t)(y + 10), "v",
                    0xE4E6F5u);
  }

  if (has_save) {
    gfx_blend_round_rect((uint32_t)(x + w - 85), (uint32_t)(y + 6),
                         17u, 17u, 6u, 0x245D57u, 255u);
    gfx_draw_string((uint32_t)(x + w - 80), (uint32_t)(y + 8), "S",
                    0xE9FFF9u);
  }

  gfx_set_font(saved_family, saved_scale, saved_bold);
  gfx_set_font_size(saved_size);
}
