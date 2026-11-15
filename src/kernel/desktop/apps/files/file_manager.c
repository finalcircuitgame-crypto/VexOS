#include "file_manager.h"

#include "../../graphics/graphics.h"
#include "../../window_manager.h"
#include "../../../../include/simplefs.h"

#define FILE_MANAGER_MAX_FILES 128u
#define FILE_MANAGER_NAME_CAP 64u
#define FILE_MANAGER_SEARCH_CAP 48u
#define FILE_MANAGER_ROW_HEIGHT 28u

static desktop_window_t g_window;
static uint8_t g_open;
static uint8_t g_dragging;
static int32_t g_drag_offset_x;
static int32_t g_drag_offset_y;
static uint32_t g_scroll;
static uint32_t g_selected;
static uint32_t g_last_click_tick;
static uint32_t g_last_click_index;
static uint8_t g_sort_descending;
static uint8_t g_search_focused;
static char g_search[FILE_MANAGER_SEARCH_CAP];
static uint32_t g_order[FILE_MANAGER_MAX_FILES];
static uint32_t g_order_count;
static uint32_t g_visible[FILE_MANAGER_MAX_FILES];
static uint32_t g_visible_count;

static uint32_t fm_min_u32(uint32_t a, uint32_t b) {
  return a < b ? a : b;
}

static int fm_hit(int32_t px, int32_t py, int32_t x, int32_t y,
                  int32_t w, int32_t h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static char fm_lower(char c) {
  if (c >= 'A' && c <= 'Z')
    return (char)(c - 'A' + 'a');
  return c;
}

static int fm_name_compare(const char *a, const char *b) {
  uint32_t i = 0;
  while (a[i] && b[i]) {
    char ca = fm_lower(a[i]);
    char cb = fm_lower(b[i]);
    if (ca < cb) return -1;
    if (ca > cb) return 1;
    i++;
  }
  if (a[i]) return 1;
  if (b[i]) return -1;
  return 0;
}

static int fm_contains(const char *text, const char *needle) {
  if (!needle || !needle[0])
    return 1;
  if (!text)
    return 0;

  for (uint32_t i = 0; text[i]; i++) {
    uint32_t j = 0;
    while (needle[j] && text[i + j] &&
           fm_lower(text[i + j]) == fm_lower(needle[j]))
      j++;
    if (!needle[j])
      return 1;
  }
  return 0;
}

static uint32_t fm_name_length(const char *name) {
  uint32_t n = 0;
  if (name)
    while (name[n])
      n++;
  return n;
}

static void fm_copy(char *dst, const char *src, uint32_t capacity) {
  if (!dst || !capacity)
    return;
  uint32_t i = 0;
  if (src) {
    while (i + 1u < capacity && src[i]) {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = 0;
}

static void fm_u32_to_text(char *out, uint32_t value) {
  char reverse[12];
  uint32_t count = 0;
  do {
    reverse[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value && count < sizeof(reverse));

  for (uint32_t i = 0; i < count; i++)
    out[i] = reverse[count - i - 1u];
  out[count] = 0;
}

static const char *fm_file_kind(const char *name) {
  uint32_t n = fm_name_length(name);
  if (n >= 4u) {
    const char *ext = name + n - 4u;
    if (fm_lower(ext[0]) == '.' && fm_lower(ext[1]) == 't' &&
        fm_lower(ext[2]) == 'x' && fm_lower(ext[3]) == 't')
      return "Text document";
    if (fm_lower(ext[0]) == '.' && fm_lower(ext[1]) == 'p' &&
        fm_lower(ext[2]) == 'n' && fm_lower(ext[3]) == 'g')
      return "Image";
    if (fm_lower(ext[0]) == '.' && fm_lower(ext[1]) == 'w' &&
        fm_lower(ext[2]) == 'a' && fm_lower(ext[3]) == 'd')
      return "Game data";
    if (fm_lower(ext[0]) == '.' && fm_lower(ext[1]) == 'e' &&
        fm_lower(ext[2]) == 'l' && fm_lower(ext[3]) == 'f')
      return "Executable";
    if (fm_lower(ext[0]) == '.' && fm_lower(ext[1]) == 'j' &&
        fm_lower(ext[2]) == 'p' && fm_lower(ext[3]) == 'g')
      return "Image";
  }
  return "File";
}

static void fm_draw_file_icon(uint32_t x, uint32_t y, const char *name) {
  uint32_t accent = 0x8E7DFFu;
  const char *kind = fm_file_kind(name);
  if (kind[0] == 'T') accent = 0x69D5B0u;
  else if (kind[0] == 'I') accent = 0x65B7F0u;
  else if (kind[0] == 'G') accent = 0xE5B36Du;
  else if (kind[0] == 'E') accent = 0xB69BFFu;

  gfx_blend_round_rect(x, y, 18u, 20u, 4u, 0x29334Bu, 255u);
  gfx_draw_rect(x + 3u, y + 3u, 12u, 2u, accent);
  gfx_draw_rect(x + 4u, y + 9u, 10u, 1u, 0x8793B0u);
  gfx_draw_rect(x + 4u, y + 13u, 10u, 1u, 0x687590u);
  gfx_draw_rect(x + 4u, y + 17u, 7u, 1u, 0x687590u);
}

static int fm_get_name(uint32_t storage_index, char *out) {
  out[0] = 0;
  return simplefs_file_name_at(storage_index, out, FILE_MANAGER_NAME_CAP);
}

static void fm_rebuild_visible(void) {
  g_visible_count = 0;
  for (uint32_t i = 0; i < g_order_count; i++) {
    char name[FILE_MANAGER_NAME_CAP];
    if (!fm_get_name(g_order[i], name))
      continue;
    if (fm_contains(name, g_search) &&
        g_visible_count < FILE_MANAGER_MAX_FILES)
      g_visible[g_visible_count++] = g_order[i];
  }

  if (g_scroll > g_visible_count)
    g_scroll = g_visible_count;
  if (g_selected != 0xFFFFFFFFu) {
    uint8_t found = 0;
    for (uint32_t i = 0; i < g_visible_count; i++)
      if (g_visible[i] == g_selected) found = 1;
    if (!found)
      g_selected = 0xFFFFFFFFu;
  }
}

static void fm_refresh(void) {
  g_order_count = simplefs_file_count();
  if (g_order_count > FILE_MANAGER_MAX_FILES)
    g_order_count = FILE_MANAGER_MAX_FILES;

  for (uint32_t i = 0; i < g_order_count; i++)
    g_order[i] = i;

  /* Small bounded insertion sort: the storage index remains the stable ID. */
  for (uint32_t i = 1; i < g_order_count; i++) {
    uint32_t key = g_order[i];
    char key_name[FILE_MANAGER_NAME_CAP];
    if (!fm_get_name(key, key_name))
      continue;
    uint32_t j = i;
    while (j > 0) {
      char previous_name[FILE_MANAGER_NAME_CAP];
      if (!fm_get_name(g_order[j - 1u], previous_name))
        break;
      int order = fm_name_compare(previous_name, key_name);
      if (g_sort_descending ? order >= 0 : order <= 0)
        break;
      g_order[j] = g_order[j - 1u];
      j--;
    }
    g_order[j] = key;
  }

  fm_rebuild_visible();
}

void file_manager_init(uint32_t screen_width, uint32_t screen_height) {
  g_open = 0;
  g_dragging = 0;
  g_drag_offset_x = 0;
  g_drag_offset_y = 0;
  g_scroll = 0;
  g_selected = 0xFFFFFFFFu;
  g_last_click_tick = 0;
  g_last_click_index = 0xFFFFFFFFu;
  g_sort_descending = 0;
  g_search_focused = 0;
  g_search[0] = 0;
  g_order_count = 0;
  g_visible_count = 0;

  g_window.w = (int32_t)(screen_width > 760u ? 720u :
      (screen_width > 48u ? screen_width - 48u : screen_width));
  g_window.h = (int32_t)(screen_height > 560u ? 500u :
      (screen_height > 80u ? screen_height - 80u : screen_height));
  g_window.x = ((int32_t)screen_width - g_window.w) / 2;
  g_window.y = ((int32_t)screen_height - g_window.h) / 2;
}

void file_manager_open(void) {
  g_open = 1;
  g_search_focused = 0;
  fm_refresh();
  wm_raise(APP_EXPLORER);
}

void file_manager_close(void) {
  g_open = 0;
  g_dragging = 0;
  g_search_focused = 0;
}

uint8_t file_manager_is_open(void) { return g_open; }
desktop_window_t *file_manager_window(void) { return &g_window; }

static void fm_draw_size(uint32_t x, uint32_t y, const char *name) {
  uint32_t bytes = 0;
  char size_text[16];
  if (!simplefs_file_size(name, &bytes)) {
    gfx_draw_string(x, y, "--", 0x8299B1u);
    return;
  }

  if (bytes < 1024u) {
    fm_u32_to_text(size_text, bytes);
    uint32_t n = fm_name_length(size_text);
    size_text[n++] = ' ';
    size_text[n++] = 'B';
    size_text[n] = 0;
  } else if (bytes < 1024u * 1024u) {
    fm_u32_to_text(size_text, bytes / 1024u);
    uint32_t n = fm_name_length(size_text);
    size_text[n++] = ' ';
    size_text[n++] = 'K';
    size_text[n++] = 'B';
    size_text[n] = 0;
  } else {
    fm_u32_to_text(size_text, bytes / (1024u * 1024u));
    uint32_t n = fm_name_length(size_text);
    size_text[n++] = ' ';
    size_text[n++] = 'M';
    size_text[n++] = 'B';
    size_text[n] = 0;
  }
  gfx_draw_string(x, y, size_text, 0x8FA8C0u);
}

void file_manager_draw(const file_manager_render_context_t *context) {
  if (!context || !context->draw_chrome || !g_open)
    return;

  int32_t x = g_window.x;
  int32_t y = g_window.y;
  int32_t w = g_window.w;
  int32_t h = g_window.h;

  char count_text[24];
  fm_u32_to_text(count_text, g_visible_count);
  uint32_t count_len = fm_name_length(count_text);
  count_text[count_len++] = ' ';
  count_text[count_len++] = 'i';
  count_text[count_len++] = 't';
  count_text[count_len++] = 'e';
  count_text[count_len++] = 'm';
  count_text[count_len++] = 's';
  count_text[count_len] = 0;

  context->draw_chrome(&g_window, "Files", count_text,
                       context->focused, 1, 0);

  /* Breadcrumb and search toolbar */
  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 31),
                (uint32_t)(w - 2), 38u, 0x191F33u);
  gfx_blend_round_rect((uint32_t)(x + 12), (uint32_t)(y + 38),
                       (uint32_t)(w - 24), 24u, 6u, 0x101626u, 255u);
  gfx_draw_string((uint32_t)(x + 22), (uint32_t)(y + 46), "<",
                  0x9C91E8u);
  gfx_draw_string((uint32_t)(x + 42), (uint32_t)(y + 46), ">",
                  0x697590u);
  gfx_draw_rect((uint32_t)(x + 62), (uint32_t)(y + 43), 1u, 14u,
                0x35405B);
  gfx_draw_string((uint32_t)(x + 76), (uint32_t)(y + 46),
                  "VexOS  /  Storage  /  Root", 0xDCE0F2u);

  uint32_t search_width = w > 600 ? 154u : 110u;
  uint32_t search_x = (uint32_t)(x + w) - search_width - 22u;
  gfx_blend_round_rect(search_x, (uint32_t)(y + 38), search_width,
                       24u, 6u, 0x101626u, 255u);
  gfx_draw_string(search_x + 9u, (uint32_t)(y + 46),
                  g_search[0] ? g_search : "Search files",
                  g_search_focused ? 0xF3F0FFu : 0x8490ADu);
  if (g_search_focused) {
    uint32_t cursor_x = search_x + 9u + fm_name_length(g_search) * 8u;
    if (cursor_x < search_x + search_width - 6u)
      gfx_draw_rect(cursor_x, (uint32_t)(y + 44), 1u, 12u, 0xA99BFFu);
  }

  uint32_t body_y = (uint32_t)y + 70u;
  uint32_t body_h = h > 84 ? (uint32_t)h - 84u : 1u;

  /* Places rail */
  gfx_blend_round_rect((uint32_t)(x + 10), body_y, 140u, body_h,
                       7u, 0x121A2Bu, 255u);
  gfx_draw_rect((uint32_t)(x + 10), body_y + 1u, 2u,
                body_h > 2u ? body_h - 2u : 1u, 0x796BDAu);
  gfx_draw_string((uint32_t)(x + 22), body_y + 14u,
                  "QUICK ACCESS", 0x8F9AB6u);
  gfx_blend_round_rect((uint32_t)(x + 16), body_y + 32u,
                       128u, 27u, 5u, 0x302B50u, 255u);
  gfx_draw_rect((uint32_t)(x + 20), body_y + 40u, 3u, 11u, 0x9A88FFu);
  gfx_draw_string((uint32_t)(x + 34), body_y + 41u,
                  "Storage", 0xF1F0FFu);
  gfx_draw_string((uint32_t)(x + 22), body_y + 78u,
                  "DEVICES", 0x8F9AB6u);
  gfx_blend_round_rect((uint32_t)(x + 18), body_y + 91u,
                       124u, 26u, 5u, 0x1A2438u, 255u);
  gfx_draw_rect((uint32_t)(x + 24), body_y + 100u, 10u, 8u, 0x67DDB5u);
  gfx_draw_string((uint32_t)(x + 42), body_y + 100u,
                  "VexOS disk", 0xCAD2E7u);
  gfx_draw_string((uint32_t)(x + 24), body_y + 126u,
                  "Root volume", 0x7E8BA8u);

  /* Main file list surface */
  uint32_t list_x = (uint32_t)x + 160u;
  uint32_t list_w = w > 176 ? (uint32_t)w - 172u : 1u;
  gfx_blend_round_rect(list_x, body_y, list_w, body_h,
                       7u, 0x111929u, 255u);
  gfx_draw_string(list_x + 14u, body_y + 12u,
                  "Storage", 0xF0F1FCu);
  gfx_draw_string(list_x + 14u, body_y + 30u,
                  "Files on this device", 0x8995B1u);
  if (list_w > 340u) {
    gfx_blend_round_rect(list_x + list_w - 112u, body_y + 10u,
                         98u, 20u, 6u, 0x202B3Eu, 255u);
    gfx_draw_string(list_x + list_w - 101u, body_y + 16u,
                    "LOCAL DISK", 0x8DDCC0u);
  }

  uint32_t header_y = body_y + 50u;
  gfx_draw_rect(list_x + 2u, header_y, list_w - 4u, 23u, 0x1B263B);
  gfx_draw_string(list_x + 14u, header_y + 8u,
                  g_sort_descending ? "NAME  v" : "NAME  ^",
                  0xC1B8FFu);
  uint32_t type_x = list_x + (list_w > 490u ? 330u : 210u);
  uint32_t size_x = list_x + (list_w > 490u ? 500u : 0u);
  gfx_draw_string(type_x, header_y + 8u, "TYPE", 0xA1ABC4u);
  if (list_w > 490u)
    gfx_draw_string(size_x, header_y + 8u, "SIZE", 0xA1ABC4u);
  gfx_draw_rect(list_x + 2u, header_y + 22u, list_w - 4u, 1u, 0x34405A);

  uint32_t rows_y = header_y + 24u;
  uint32_t available_h = h > 178 ? (uint32_t)h - 178u : 0u;
  uint32_t row_capacity = available_h / FILE_MANAGER_ROW_HEIGHT;
  uint32_t remaining = g_visible_count > g_scroll ?
      g_visible_count - g_scroll : 0u;
  uint32_t visible_rows = fm_min_u32(remaining, row_capacity);

  if (!g_visible_count) {
    uint32_t empty_x = list_x + (list_w > 280u ? (list_w - 280u) / 2u : 18u);
    gfx_blend_round_rect(empty_x + 88u, rows_y + 18u,
                         44u, 44u, 12u, 0x252344u, 255u);
    gfx_draw_string(empty_x + 104u, rows_y + 33u, "V", 0xB4A8FFu);
    gfx_draw_string(empty_x + 30u, rows_y + 78u,
                    g_search[0] ? "No matching files" : "This folder is empty",
                    0xE1E4F2u);
    gfx_draw_string(empty_x + 18u, rows_y + 98u,
                    g_search[0] ? "Try another search term." :
                    "Files saved to this volume will appear here.",
                    0x8793AEu);
  } else if (row_capacity) {
    gfx_push_clip(list_x, rows_y, list_w,
                  row_capacity * FILE_MANAGER_ROW_HEIGHT);
    for (uint32_t row = 0; row < visible_rows; ++row) {
      uint32_t visible_index = g_scroll + row;
      uint32_t storage_index = g_visible[visible_index];
      char name[FILE_MANAGER_NAME_CAP];
      if (!fm_get_name(storage_index, name))
        continue;

      uint32_t row_y = rows_y + row * FILE_MANAGER_ROW_HEIGHT;
      if (storage_index == g_selected) {
        gfx_blend_round_rect(list_x + 5u, row_y + 1u,
                             list_w - 10u, 25u, 5u, 0x302B50u, 255u);
        gfx_draw_rect(list_x + 6u, row_y + 5u, 2u, 17u, 0xA18FFFu);
      } else if (row & 1u) {
        gfx_draw_rect(list_x + 5u, row_y + 1u,
                      list_w - 10u, 25u, 0x151F30u);
      }

      fm_draw_file_icon(list_x + 14u, row_y + 3u, name);
      if (fm_name_length(name) > 34u)
        name[34] = 0;
      gfx_draw_string(list_x + 42u, row_y + 9u, name,
                      storage_index == g_selected ? 0xFFFFFFu : 0xD7DDF0u);
      gfx_draw_string(type_x, row_y + 9u, fm_file_kind(name), 0x929DB8u);
      if (list_w > 490u)
        fm_draw_size(size_x, row_y + 9u, name);
    }
    gfx_pop_clip();
  }

  gfx_draw_rect((uint32_t)(x + 160), (uint32_t)(y + h - 28),
                list_w, 1u, 0x303A53u);
  if (g_selected != 0xFFFFFFFFu) {
    char selected_name[FILE_MANAGER_NAME_CAP];
    if (fm_get_name(g_selected, selected_name))
      gfx_draw_string((uint32_t)(x + 174), (uint32_t)(y + h - 18),
                      selected_name, 0xB4A8FFu);
  } else {
    gfx_draw_string((uint32_t)(x + 174), (uint32_t)(y + h - 18),
                    "VexOS Storage", 0x8995B1u);
  }
  gfx_draw_string((uint32_t)(x + w - 150), (uint32_t)(y + h - 18),
                  "Double-click to open", 0x8995B1u);
}

file_manager_event_t file_manager_handle_click(
    int32_t px, int32_t py, uint32_t tick,
    file_manager_open_file_fn open_file) {
  if (!g_open)
    return FILE_MANAGER_EVENT_NONE;

  int32_t x = g_window.x;
  int32_t y = g_window.y;
  int32_t w = g_window.w;
  int32_t h = g_window.h;

  if (fm_hit(px, py, x + w - 24, y + 5, 20, 20)) {
    file_manager_close();
    return FILE_MANAGER_EVENT_CLOSED;
  }
  if (fm_hit(px, py, x + w - 46, y + 5, 20, 20)) {
    if (g_scroll) g_scroll--;
    g_search_focused = 0;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }
  if (fm_hit(px, py, x + w - 66, y + 5, 20, 20)) {
    if (g_scroll < g_visible_count) g_scroll++;
    g_search_focused = 0;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }

  if (fm_hit(px, py, x, y, w, 30)) {
    g_dragging = 1;
    g_drag_offset_x = px - x;
    g_drag_offset_y = py - y;
    g_search_focused = 0;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }

  uint32_t search_width = w > 600 ? 154u : 110u;
  int32_t search_x = x + w - (int32_t)search_width - 22;
  if (fm_hit(px, py, search_x, y + 38, (int32_t)search_width, 24)) {
    g_search_focused = 1;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }

  uint32_t by = (uint32_t)y + 70u;
  uint32_t header_y = by + 50u;
  uint32_t mx = (uint32_t)x + 160u;
  uint32_t mw = w > 176 ? (uint32_t)w - 172u : 1u;
  if (fm_hit(px, py, (int32_t)mx, (int32_t)header_y, (int32_t)mw, 23)) {
    if (px < (int32_t)mx + (int32_t)(mw > 490u ? 180u : 150u)) {
      g_sort_descending = (uint8_t)!g_sort_descending;
      fm_refresh();
    }
    g_search_focused = 0;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }

  int32_t list_x = x + 160;
  int32_t list_y = y + 144;
  int32_t list_w = w - 172;
  int32_t list_h = h - 174;
  if (fm_hit(px, py, list_x, list_y, list_w, list_h)) {
    wm_raise(APP_EXPLORER);
    g_search_focused = 0;
    uint32_t row = (uint32_t)(py - list_y) / FILE_MANAGER_ROW_HEIGHT;
    uint32_t visible_index = g_scroll + row;
    if (visible_index >= g_visible_count)
      return FILE_MANAGER_EVENT_CHANGED;

    uint32_t storage_index = g_visible[visible_index];
    g_selected = storage_index;
    uint32_t elapsed = tick - g_last_click_tick;
    if (g_last_click_index == storage_index && elapsed < 25u) {
      char name[FILE_MANAGER_NAME_CAP];
      if (fm_get_name(storage_index, name) && open_file && open_file(name)) {
        g_last_click_index = 0xFFFFFFFFu;
        return FILE_MANAGER_EVENT_CHANGED;
      }
      return FILE_MANAGER_EVENT_OPEN_FAILED;
    }
    g_last_click_tick = tick;
    g_last_click_index = storage_index;
    return FILE_MANAGER_EVENT_CHANGED;
  }

  if (fm_hit(px, py, x, y, w, h)) {
    g_search_focused = 0;
    wm_raise(APP_EXPLORER);
    return FILE_MANAGER_EVENT_CHANGED;
  }
  return FILE_MANAGER_EVENT_NONE;
}

uint8_t file_manager_handle_char(char value) {
  if (!g_open || !g_search_focused)
    return 0;

  uint32_t length = fm_name_length(g_search);
  if (value == '\b' || value == 127) {
    if (length) {
      g_search[length - 1u] = 0;
      fm_rebuild_visible();
    }
    return 1;
  }
  if (value == '\n' || value == '\r') {
    g_search_focused = 0;
    return 1;
  }
  if (value == 27) {
    g_search[0] = 0;
    g_search_focused = 0;
    fm_rebuild_visible();
    return 1;
  }
  if (value < 32 || value > 126 || length + 1u >= FILE_MANAGER_SEARCH_CAP)
    return 1;

  g_search[length] = value;
  g_search[length + 1u] = 0;
  g_scroll = 0;
  fm_rebuild_visible();
  return 1;
}

void file_manager_pointer_move(int32_t x, int32_t y) {
  if (g_dragging) {
    g_window.x = x - g_drag_offset_x;
    g_window.y = y - g_drag_offset_y;
  }
}

void file_manager_pointer_release(void) {
  g_dragging = 0;
}
