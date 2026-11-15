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

  gfx_blend_round_rect((uint32_t)(x + 4), (uint32_t)(y + 6),
                       (uint32_t)w, (uint32_t)h, 9u, 0x02060Cu, 155);
  gfx_blend_round_rect((uint32_t)x, (uint32_t)y, (uint32_t)w,
                       (uint32_t)h, 8u, 0x101A28u, 255);
  gfx_draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 1u,
                focused ? 0x5EA9F7u : 0x3B4C62u);
  gfx_draw_rect((uint32_t)x, (uint32_t)(y + h - 1), (uint32_t)w, 1u,
                0x26364Au);
  gfx_draw_rect((uint32_t)x, (uint32_t)y, 1u, (uint32_t)h, 0x35485Fu);
  gfx_draw_rect((uint32_t)(x + w - 1), (uint32_t)y, 1u,
                (uint32_t)h, 0x26364Au);
  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 1),
                (uint32_t)(w - 2), 29u,
                focused ? 0x1D3550u : 0x1A293Bu);
  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 29),
                (uint32_t)(w - 2), 1u,
                focused ? 0x42688Cu : 0x34475Du);

  if (draw_icon)
    draw_icon((uint32_t)(x + 8), (uint32_t)(y + 6), 20u, app);
  if (title)
    gfx_draw_string((uint32_t)(x + 36), (uint32_t)(y + 11), title,
                    focused ? 0xF5F8FCu : 0xC0CAD8u);

  if (subtitle && subtitle[0]) {
    uint32_t title_len = 0;
    uint32_t subtitle_len = 0;
    while (title && title[title_len])
      title_len++;
    while (subtitle[subtitle_len])
      subtitle_len++;

    uint32_t subtitle_x = (uint32_t)(x + 36) + title_len * 8u + 10u;
    if (subtitle_x + subtitle_len * 8u < (uint32_t)(x + w - 100)) {
      gfx_draw_rect(subtitle_x - 5u, (uint32_t)(y + 10), 1u, 10u,
                    0x59708Au);
      gfx_draw_string(subtitle_x, (uint32_t)(y + 11), subtitle,
                      0x9EB5CCu);
    }
  }

  gfx_blend_round_rect((uint32_t)(x + w - 23), (uint32_t)(y + 6),
                       17u, 17u, 5u, 0xB94755u, 255u);
  gfx_draw_string((uint32_t)(x + w - 18), (uint32_t)(y + 10), "x",
                  0xFFFFFFu);

  if (has_scroll) {
    gfx_blend_round_rect((uint32_t)(x + w - 45), (uint32_t)(y + 6),
                         17u, 17u, 5u, 0x334B65u, 255u);
    gfx_blend_round_rect((uint32_t)(x + w - 65), (uint32_t)(y + 6),
                         17u, 17u, 5u, 0x334B65u, 255u);
    gfx_draw_string((uint32_t)(x + w - 41), (uint32_t)(y + 10), "^",
                    0xE8F1FAu);
    gfx_draw_string((uint32_t)(x + w - 61), (uint32_t)(y + 10), "v",
                    0xE8F1FAu);
  }

  if (has_save) {
    gfx_blend_round_rect((uint32_t)(x + w - 85), (uint32_t)(y + 6),
                         17u, 17u, 5u, 0x236B59u, 255u);
    gfx_draw_string((uint32_t)(x + w - 80), (uint32_t)(y + 10), "S",
                    0xF0FFF8u);
  }
}
