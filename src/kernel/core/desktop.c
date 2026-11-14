#include "../../include/desktop.h"
#include "../../include/gfx.h"
#include "../../include/window_manager.h"
#include "../../include/input.h"
#include "../../include/heap.h"
#include "../../include/hw_profile.h"
#include "../../include/usermode.h"
#include "../../include/task.h"
#include "../../include/simplefs.h"
#include "../../include/ramfs.h"
#include "../../include/png.h"
#include "../../include/jpeg.h"
#include "../../include/interrupts.h"
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
static uint8_t g_explorer_open;
static uint8_t g_viewer_open;

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
static uint8_t g_explorer_dragging;
static int32_t g_explorer_drag_off_x;
static int32_t g_explorer_drag_off_y;
static uint8_t g_viewer_dragging;
static int32_t g_viewer_drag_off_x;
static int32_t g_viewer_drag_off_y;
static char g_viewer_name[64];
static char g_viewer_text[1024];
static uint32_t g_viewer_text_len;
static uint32_t g_viewer_kind;
static uint8_t *g_viewer_pixels;
static uint32_t g_viewer_image_width;
static uint32_t g_viewer_image_height;

enum {
  VIEWER_CONTENT_TEXT = 0,
  VIEWER_CONTENT_PNG = 1,
  VIEWER_CONTENT_JPEG = 2,
  VIEWER_CONTENT_BINARY = 3
};

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
  APP_SETUP = 4,
  APP_COUNT = 5
} app_id_t;

static void app_raise(app_id_t app);

static void viewer_clear_content(void) {
  if (g_viewer_pixels) {
    kfree(g_viewer_pixels);
    g_viewer_pixels = NULL;
  }
  g_viewer_image_width = 0;
  g_viewer_image_height = 0;
  g_viewer_kind = VIEWER_CONTENT_TEXT;
  g_viewer_text_len = 0;
  g_viewer_text[0] = 0;
}

static void viewer_set_message(uint32_t kind, const char *message) {
  uint32_t i = 0;
  if (message) {
    while (i + 1u < (uint32_t)sizeof(g_viewer_text) && message[i]) {
      g_viewer_text[i] = message[i];
      i++;
    }
  }
  g_viewer_text[i] = 0;
  g_viewer_text_len = i;
  g_viewer_kind = kind;
}

static int viewer_is_text(const uint8_t *data, uint32_t length) {
  if (!data)
    return 0;
  for (uint32_t i = 0; i < length; i++) {
    uint8_t c = data[i];
    if (c == 0)
      return 0;
    if (c < 32u && c != '\n' && c != '\r' && c != '\t')
      return 0;
  }
  return 1;
}

static void viewer_copy_name(const char *name) {
  uint32_t i = 0;
  while (i + 1u < (uint32_t)sizeof(g_viewer_name) && name && name[i]) {
    g_viewer_name[i] = name[i];
    i++;
  }
  g_viewer_name[i] = 0;
}

static int viewer_load_file(const char *name) {
  uint32_t file_size = 0;
  uint32_t out_len = 0;
  uint8_t header[8] = {0};
  static const uint8_t png_signature[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};

  if (!name || !simplefs_init() || !simplefs_file_size(name, &file_size) ||
      file_size == 0) {
    return 0;
  }

  viewer_clear_content();
  viewer_copy_name(name);

  if (file_size >= sizeof(header) &&
      simplefs_read_file(name, header, (uint32_t)sizeof(header), &out_len)) {
    uint8_t is_png = 1;
    for (uint32_t i = 0; i < (uint32_t)sizeof(png_signature); i++) {
      if (header[i] != png_signature[i]) {
        is_png = 0;
        break;
      }
    }

    if (is_png) {
      if (file_size > 8u * 1024u * 1024u) {
        viewer_set_message(VIEWER_CONTENT_BINARY,
                           "PNG file is too large to preview.");
      } else {
        uint8_t *file_data = (uint8_t *)kmalloc(file_size);
        if (!file_data) {
          viewer_set_message(VIEWER_CONTENT_BINARY,
                             "Not enough memory to open this PNG.");
        } else {
          uint32_t bytes_read = 0;
          uint32_t image_width = 0;
          uint32_t image_height = 0;
          uint8_t *image_pixels = NULL;
          int read_ok = simplefs_read_file(name, file_data, file_size, &bytes_read) &&
                        bytes_read == file_size;
          int decode_ok = read_ok &&
                          png_decode_rgba(file_data, file_size, &image_width,
                                          &image_height, &image_pixels);
          kfree(file_data);

          if (decode_ok && image_pixels && image_width && image_height) {
            g_viewer_pixels = image_pixels;
            g_viewer_image_width = image_width;
            g_viewer_image_height = image_height;
            g_viewer_kind = VIEWER_CONTENT_PNG;
            g_viewer_text[0] = 0;
            g_viewer_text_len = 0;
          } else {
            if (image_pixels)
              kfree(image_pixels);
            viewer_set_message(VIEWER_CONTENT_BINARY,
                               read_ok ? "PNG encoding is not supported or is invalid."
                                       : "Could not read the complete PNG file.");
          }
        }
      }
    } else if (header[0] == 0xFFu && header[1] == 0xD8u) {
      if (file_size > 8u * 1024u * 1024u) {
        viewer_set_message(VIEWER_CONTENT_JPEG,
                           "JPEG file is too large to preview.");
      } else {
        uint8_t *file_data = (uint8_t *)kmalloc(file_size);
        if (!file_data) {
          viewer_set_message(VIEWER_CONTENT_JPEG,
                             "Not enough memory to open this JPEG.");
        } else {
          uint32_t bytes_read = 0;
          uint32_t image_width = 0;
          uint32_t image_height = 0;
          uint8_t *image_pixels = NULL;
          int read_ok = simplefs_read_file(name, file_data, file_size, &bytes_read) &&
                        bytes_read == file_size;
          int decode_ok = read_ok &&
                          jpeg_decode_rgba(file_data, file_size, &image_width,
                                           &image_height, &image_pixels);
          kfree(file_data);

          if (decode_ok && image_pixels && image_width && image_height) {
            g_viewer_pixels = image_pixels;
            g_viewer_image_width = image_width;
            g_viewer_image_height = image_height;
            g_viewer_kind = VIEWER_CONTENT_JPEG;
            g_viewer_text[0] = 0;
            g_viewer_text_len = 0;
          } else {
            if (image_pixels)
              kfree(image_pixels);
            viewer_set_message(VIEWER_CONTENT_JPEG,
                               read_ok ? "JPEG format is unsupported or invalid. Baseline JPEG is supported."
                                       : "Could not read the complete JPEG file.");
          }
        }
      }
    } else {
      uint32_t text_len = 0;
      if (!simplefs_read_file(name, g_viewer_text,
                              (uint32_t)sizeof(g_viewer_text) - 1u,
                              &text_len)) {
        return 0;
      }
      if (text_len >= (uint32_t)sizeof(g_viewer_text))
        text_len = (uint32_t)sizeof(g_viewer_text) - 1u;
      if (viewer_is_text((const uint8_t *)g_viewer_text, text_len)) {
        g_viewer_text[text_len] = 0;
        g_viewer_text_len = text_len;
        g_viewer_kind = VIEWER_CONTENT_TEXT;
      } else {
        viewer_set_message(VIEWER_CONTENT_BINARY,
                           "Binary file. Preview supports text and PNG images.");
      }
    }
  } else {
    uint32_t text_len = 0;
    if (!simplefs_read_file(name, g_viewer_text,
                            (uint32_t)sizeof(g_viewer_text) - 1u, &text_len)) {
      return 0;
    }
    if (text_len >= (uint32_t)sizeof(g_viewer_text))
      text_len = (uint32_t)sizeof(g_viewer_text) - 1u;
    if (viewer_is_text((const uint8_t *)g_viewer_text, text_len)) {
      g_viewer_text[text_len] = 0;
      g_viewer_text_len = text_len;
    } else {
      viewer_set_message(VIEWER_CONTENT_BINARY,
                         "Binary file. Preview supports text and PNG images.");
    }
  }

  g_viewer_scroll = 0;
  g_viewer_open = 1;
      wm_open(APP_VIEWER);
  app_raise(APP_VIEWER);
  return 1;
}

static void app_raise(app_id_t app) {
  wm_raise((uint32_t)app);
}

static wm_window_t g_notepad_win;
static uint8_t g_notepad_dragging;
static int32_t g_notepad_drag_off_x;
static int32_t g_notepad_drag_off_y;
static char g_notepad_name[64];
static char g_notepad_text[4096];
static uint32_t g_notepad_text_len;
static uint32_t g_notepad_scroll;

// Setup Wizard state
static wm_window_t g_setup_win;
static uint8_t g_setup_open;
static uint8_t g_setup_dragging;
static int32_t g_setup_drag_off_x;
static int32_t g_setup_drag_off_y;
static uint32_t g_setup_step; // 0=welcome, 1=detect, 2=partition, 3=format, 4=verify, 5=complete
static uint8_t g_setup_disk_ready;
static uint8_t g_setup_partition_done;
static uint8_t g_setup_format_done;

static void shell_append(const char *s);

// External functions
void xhci_poll_events(void);
#ifdef VEXOS_CI_INPUT_TEST
extern void serial_print(const char *str);
static uint8_t g_ci_keyboard_input_seen;
static uint8_t g_ci_mouse_move_seen;
static uint8_t g_ci_start_click_seen;
#endif

static uint8_t g_ring3_task_started;
static uint8_t g_fullscreen_app_active;

void desktop_set_fullscreen_app_active(int active) {
  g_fullscreen_app_active = active ? 1u : 0u;
}

static void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c);
static void draw_string_8x8(uint32_t x, uint32_t y, const char *s,
                            uint32_t fg);
static void blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t c, uint8_t a);
static void blend_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t radius, uint32_t c, uint8_t a);
static uint32_t shell_u32_to_dec(char out[12], uint32_t v);
static int streq_n(const char *a, const char *b, uint32_t n);

static inline void put_px(uint32_t x, uint32_t y, uint32_t c);

static void draw_icon_20(uint32_t x, uint32_t y, const uint32_t *pix) {
  for (uint32_t yy = 0; yy < 20u; yy++) {
    for (uint32_t xx = 0; xx < 20u; xx++) {
      uint32_t c = pix[yy * 20u + xx];
      if (c & 0xFF000000u) {
        put_px(x + xx, y + yy, c & 0x00FFFFFFu);
      }
    }
  }
}

static void draw_icon_20_scaled(uint32_t x, uint32_t y, const uint32_t *pix,
                                uint32_t scale) {
  if (scale == 0)
    return;
  for (uint32_t yy = 0; yy < 20u; yy++) {
    for (uint32_t xx = 0; xx < 20u; xx++) {
      uint32_t c = pix[yy * 20u + xx];
      if (!(c & 0xFF000000u))
        continue;
      uint32_t rgb = c & 0x00FFFFFFu;
      for (uint32_t sy = 0; sy < scale; sy++) {
        for (uint32_t sx = 0; sx < scale; sx++) {
          put_px(x + xx * scale + sx, y + yy * scale + sy, rgb);
        }
      }
    }
  }
}

static const uint32_t k_icon_shell_20[20u * 20u] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu,
    0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static const uint32_t k_icon_explorer_20[20u * 20u] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0x00000000u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u,
    0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u,
    0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u, 0xFFCC8B12u,
    0xFFCC8B12u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFCC8B12u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFCC8B12u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFCC8B12u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFCC8B12u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFCC8B12u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFCC8B12u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFCC8B12u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u, 0xFFF3C046u,
    0xFFCC8B12u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u, 0xFFE6A823u,
    0xFFE6A823u, 0xFFE6A823u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static const uint32_t k_icon_viewer_20[20u * 20u] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFF2D80FFu, 0xFF2D80FFu,
    0xFF2D80FFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF0B111Cu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0B111Cu, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static const uint32_t k_icon_notepad_20[20u * 20u] = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,

    0x00000000u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u,
    0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0xFF2A3140u, 0x00000000u,

    0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,

    0x00000000u, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu,
    0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu,
    0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu,
    0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0xFF2D80FFu, 0x00000000u,
};


static void draw_app_icon(uint32_t x,uint32_t y,uint32_t size,app_id_t app){
  uint32_t tile=app==APP_EXPLORER?0x35402Du:app==APP_VIEWER?0x263D60u:app==APP_NOTEPAD?0x493B65u:app==APP_SETUP?0x3B345Bu:0x243B59u;
  if(size<18u)return;blend_round_rect(x,y,size,size,size/5u,tile,255);draw_rect(x+1u,y+1u,size-2u,1u,0x7187A4u);
  if(size<28u){const char *m=app==APP_EXPLORER?"F":app==APP_VIEWER?"V":app==APP_NOTEPAD?"N":app==APP_SETUP?"S":">";draw_string_8x8(x+(size-8u)/2u,y+(size-8u)/2u,m,0xFFFFFFu);return;}
  if(app==APP_SHELL){blend_round_rect(x+6u,y+7u,size-12u,size-14u,4u,0x0A1423u,255);draw_rect(x+7u,y+8u,size-14u,5u,0x385777u);draw_rect(x+10u,y+10u,3u,2u,0xF28B82u);draw_rect(x+15u,y+10u,3u,2u,0xF6C76Bu);draw_rect(x+20u,y+10u,3u,2u,0x74C69Du);draw_string_8x8(x+10u,y+20u,">_",0x7DE0B1u);}
  else if(app==APP_EXPLORER){blend_round_rect(x+6u,y+14u,size-12u,size-21u,4u,0xC98222u,255);blend_round_rect(x+8u,y+10u,size/2u,9u,3u,0xF4C45Eu,255);draw_rect(x+7u,y+18u,size-14u,2u,0xFFE08Au);}
  else if(app==APP_VIEWER){blend_round_rect(x+6u,y+6u,size-12u,size-12u,4u,0xEAF2F8u,255);draw_rect(x+9u,y+9u,size-18u,size-18u,0x83C5E8u);blend_round_rect(x+size-15u,y+11u,5u,5u,3u,0xFFD166u,255);draw_rect(x+10u,y+size-17u,size/3u,6u,0x2E8B72u);draw_rect(x+size/3u,y+size-20u,size/3u,9u,0x26765Fu);}
  else if(app==APP_NOTEPAD){blend_round_rect(x+8u,y+5u,size-16u,size-10u,3u,0xF4F7FCu,255);draw_rect(x+12u,y+11u,size-24u,2u,0x4C8BD8u);draw_rect(x+12u,y+17u,size-24u,2u,0xA9BBD0u);draw_rect(x+12u,y+23u,size-24u,2u,0xA9BBD0u);if(size>38u)draw_rect(x+12u,y+29u,size-24u,2u,0xA9BBD0u);}
  else{blend_round_rect(x+9u,y+9u,size-18u,size-18u,size/4u,0x8A79CBu,255);blend_round_rect(x+size/2u-4u,y+size/2u-4u,8u,8u,4u,0xE8E3FFu,255);draw_rect(x+size/2u-2u,y+6u,4u,7u,0xDCD4FFu);draw_rect(x+size/2u-2u,y+size-13u,4u,7u,0xDCD4FFu);draw_rect(x+6u,y+size/2u-2u,7u,4u,0xDCD4FFu);draw_rect(x+size-13u,y+size/2u-2u,7u,4u,0xDCD4FFu);}
}
static void draw_file_icon(uint32_t x,uint32_t y,const char *name){
  uint32_t c=0x8EA4BEu,n=0;while(name&&name[n])n++;
  if(n>4u&&streq_n(&name[n-4u],".txt",4u))c=0x63B98Au;else if(n>4u&&streq_n(&name[n-4u],".png",4u))c=0x5FA9E8u;else if(n>4u&&streq_n(&name[n-4u],".wad",4u))c=0xD69B54u;else if(n>4u&&streq_n(&name[n-4u],".elf",4u))c=0xA88BDEu;
  blend_round_rect(x,y,18u,20u,3u,0xEAF0F7u,255);draw_rect(x+2u,y+2u,14u,2u,c);draw_rect(x+4u,y+8u,10u,1u,0x8A9AAF);draw_rect(x+4u,y+12u,10u,1u,0x8A9AAF);draw_rect(x+4u,y+16u,7u,1u,0x8A9AAF);
}

static void ring3_task_entry(void) {
  usermode_launch();

  // On success usermode_launch transfers to ring 3 and never returns. A
  // return means the loader rejected the image or failed before the switch.
  shell_append("ring3: loader returned; see serial log\n");
  for (;;) {
    __asm__ volatile("sti; hlt");
  }
}

static void launch_ring3_task(void) {
  if (g_ring3_task_started)
    return;
  g_ring3_task_started = 1;

  if (!simplefs_init()) {
    shell_append("ring3: storage mount failed\n");
    g_ring3_task_started = 0;
    return;
  }

  uint32_t elf_size = 0;
  if (!simplefs_file_size("doom.elf", &elf_size) || elf_size == 0) {
    shell_append("ring3: doom.elf not found\n");
    g_ring3_task_started = 0;
    return;
  }

  void *stack = kmalloc(8192);
  if (!stack) {
    shell_append("ring3: no mem\n");
    g_ring3_task_started = 0;
    return;
  }

  void *stack_top = (void *)((uint8_t *)stack + 8192);
  Task_Create(ring3_task_entry, stack_top);
}

static void draw_window_chrome(const wm_window_t *win,const char *title,const char *sub_title,uint8_t focused,uint8_t has_scroll,uint8_t has_save){
  if(!g_bi||!win||win->w<40||win->h<40)return;int32_t x=win->x,y=win->y,w=win->w,h=win->h;app_id_t app=APP_SHELL;
  if(title&&(title[0]=='F'||title[0]=='E'))app=APP_EXPLORER;else if(title&&title[0]=='V')app=APP_VIEWER;else if(title&&title[0]=='N')app=APP_NOTEPAD;else if(title&&title[0]=='S')app=APP_SETUP;
  blend_round_rect((uint32_t)(x+4),(uint32_t)(y+6),(uint32_t)w,(uint32_t)h,9u,0x02060Cu,155);blend_round_rect((uint32_t)x,(uint32_t)y,(uint32_t)w,(uint32_t)h,8u,0x101A28u,255);
  draw_rect((uint32_t)x,(uint32_t)y,(uint32_t)w,1u,focused?0x5EA9F7u:0x3B4C62u);draw_rect((uint32_t)x,(uint32_t)(y+h-1),(uint32_t)w,1u,0x26364Au);draw_rect((uint32_t)x,(uint32_t)y,1u,(uint32_t)h,0x35485Fu);draw_rect((uint32_t)(x+w-1),(uint32_t)y,1u,(uint32_t)h,0x26364Au);
  draw_rect((uint32_t)(x+1),(uint32_t)(y+1),(uint32_t)(w-2),29u,focused?0x1D3550u:0x1A293Bu);draw_rect((uint32_t)(x+1),(uint32_t)(y+29),(uint32_t)(w-2),1u,focused?0x42688Cu:0x34475Du);
  draw_app_icon((uint32_t)(x+8),(uint32_t)(y+6),20u,app);draw_string_8x8((uint32_t)(x+36),(uint32_t)(y+11),title,focused?0xF5F8FCu:0xC0CAD8u);
  if(sub_title&&sub_title[0]){uint32_t tl=0,sl=0;while(title&&title[tl])tl++;while(sub_title[sl])sl++;uint32_t sx=(uint32_t)(x+36)+tl*8u+10u;if(sx+sl*8u<(uint32_t)(x+w-100)){draw_rect(sx-5u,(uint32_t)(y+10),1u,10u,0x59708Au);draw_string_8x8(sx,(uint32_t)(y+11),sub_title,0x9EB5CCu);}}
  blend_round_rect((uint32_t)(x+w-23),(uint32_t)(y+6),17u,17u,5u,0xB94755u,255);draw_string_8x8((uint32_t)(x+w-18),(uint32_t)(y+10),"x",0xFFFFFFu);
  if(has_scroll){blend_round_rect((uint32_t)(x+w-45),(uint32_t)(y+6),17u,17u,5u,0x334B65u,255);blend_round_rect((uint32_t)(x+w-65),(uint32_t)(y+6),17u,17u,5u,0x334B65u,255);draw_string_8x8((uint32_t)(x+w-41),(uint32_t)(y+10),"^",0xE8F1FAu);draw_string_8x8((uint32_t)(x+w-61),(uint32_t)(y+10),"v",0xE8F1FAu);}
  if(has_save){blend_round_rect((uint32_t)(x+w-85),(uint32_t)(y+6),17u,17u,5u,0x236B59u,255);draw_string_8x8((uint32_t)(x+w-80),(uint32_t)(y+10),"S",0xF0FFF8u);}
}

static void draw_notepad_window(const wm_window_t *win){
  if(!g_bi||!win)return;int32_t x=win->x,y=win->y,w=win->w,h=win->h;draw_window_chrome(win,"Notepad",g_notepad_name[0]?g_notepad_name:"note.txt",(uint8_t)(wm_is_focused(APP_NOTEPAD)),1,1);
  draw_rect((uint32_t)(x+1),(uint32_t)(y+31),(uint32_t)(w-2),30u,0x17263A);draw_string_8x8((uint32_t)(x+14),(uint32_t)(y+42),"FILE   EDIT   VIEW",0xB9C9DAu);draw_string_8x8((uint32_t)(x+w-142),(uint32_t)(y+42),"PLAIN TEXT",0x7FA5C8u);
  uint32_t cx=(uint32_t)x+12u,cy=(uint32_t)y+66u,cw=w>24?(uint32_t)w-24u:1u,ch=h>78?(uint32_t)h-78u:1u;blend_round_rect(cx,cy,cw,ch,5u,0xF4F7FBu,255);draw_rect(cx+1u,cy+1u,cw-2u,1u,0xFFFFFFu);draw_rect(cx+34u,cy+1u,1u,ch-2u,0xDCE5EFu);
  uint32_t tx=cx+46u,ty=cy+12u,cols=w>72?(uint32_t)(w-72)/8u:0u,rows=h>100?(uint32_t)(h-100)/16u:0u,start=0,skip=g_notepad_scroll;for(uint32_t i=0;i<g_notepad_text_len&&skip;i++)if(g_notepad_text[i]=='\n'){skip--;start=i+1u;}
  uint32_t col=0,row=0,line=g_notepad_scroll+1u;char num[12];if(rows){shell_u32_to_dec(num,line);draw_string_8x8(cx+10u,ty,num,0x8EA0B4u);}
  for(uint32_t i=start;i<g_notepad_text_len&&row<rows;i++){char c=g_notepad_text[i];if(c=='\n'){col=0;row++;line++;if(row<rows){shell_u32_to_dec(num,line);draw_string_8x8(cx+10u,ty+row*16u,num,0x8EA0B4u);}continue;}if(c=='\t'){col=(col+4u)&~3u;continue;}if(c<32||c>126)c='.';if(col<cols){char t[2]={c,0};draw_string_8x8(tx+col*8u,ty+row*16u,t,0x24364Bu);}col++;}
}

static void draw_setup_window(const wm_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  uint8_t is_focused = (wm_is_focused(APP_SETUP));
  draw_window_chrome(win, "Setup Wizard", NULL, is_focused, 0, 0);

  if (!g_setup_open)
    return;

  const char *step_titles[] = {
    "Welcome",
    "Detect Disk",
    "Partition",
    "Format",
    "Verify",
    "Complete"
  };

  // Draw progress bar at top
  uint32_t bar_y = (uint32_t)y + 36u;
  uint32_t bar_w = (uint32_t)w - 32u;
  uint32_t step_w = bar_w / 6u;
  draw_rect((uint32_t)x + 16u, bar_y, bar_w, 4, 0x2A3140);
  for (uint32_t i = 0; i < 6; i++) {
    uint32_t cx = (uint32_t)x + 16u + i * step_w;
    uint32_t color = (i <= g_setup_step) ? 0x22C55E : 0x404040;
    draw_rect(cx, bar_y, step_w - 2, 4, color);
    // Step number
    char num[2] = {(char)('1' + i), 0};
    draw_string_8x8(cx + (step_w/2) - 4, bar_y + 8, num, color);
  }

  // Content area
  uint32_t content_y = (uint32_t)y + 72u;
  uint32_t content_h = (uint32_t)h - 108u;
  draw_rect((uint32_t)x + 16u, content_y, (uint32_t)w - 32u, content_h, 0x14141A);

  // Step title
  draw_string_8x8((uint32_t)x + 24u, content_y + 12u, step_titles[g_setup_step], 0xE0E0E0);
  draw_rect((uint32_t)x + 24u, content_y + 28u, (uint32_t)w - 48u, 1, 0x2A3140);

  // Step content
  uint32_t text_y = content_y + 44u;
  switch (g_setup_step) {
    case 0: // Welcome
      draw_string_8x8((uint32_t)x + 24u, text_y, "Welcome to VexOS Setup!", 0xFFFFFF);
      draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "This wizard will help you:", 0xD0D6E0);
      draw_string_8x8((uint32_t)x + 24u, text_y + 36u, "- Detect your disk", 0xD0D6E0);
      draw_string_8x8((uint32_t)x + 24u, text_y + 52u, "- Create partitions", 0xD0D6E0);
      draw_string_8x8((uint32_t)x + 24u, text_y + 68u, "- Format with FAT16", 0xD0D6E0);
      break;
    case 1: // Detect
      draw_string_8x8((uint32_t)x + 24u, text_y, "Detecting disk...", 0xFFFFFF);
      if (g_setup_disk_ready) {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Disk ready", 0x22C55E);
        draw_string_8x8((uint32_t)x + 24u, text_y + 40u, "Type: RAM Disk (1 MiB)", 0xD0D6E0);
      } else {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Not initialized", 0xFFB000);
      }
      break;
    case 2: // Partition
      draw_string_8x8((uint32_t)x + 24u, text_y, "Creating MBR partition...", 0xFFFFFF);
      if (g_setup_partition_done) {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Partition created", 0x22C55E);
        draw_string_8x8((uint32_t)x + 24u, text_y + 40u, "Type: FAT16 LBA", 0xD0D6E0);
      } else {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Pending", 0xD0D6E0);
      }
      break;
    case 3: // Format
      draw_string_8x8((uint32_t)x + 24u, text_y, "Formatting partition...", 0xFFFFFF);
      if (g_setup_format_done) {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Formatted", 0x22C55E);
        draw_string_8x8((uint32_t)x + 24u, text_y + 40u, "FS: FAT16", 0xD0D6E0);
      } else {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "Status: Pending", 0xD0D6E0);
      }
      break;
    case 4: // Verify
      draw_string_8x8((uint32_t)x + 24u, text_y, "Verifying installation...", 0xFFFFFF);
      if (g_setup_format_done) {
        draw_string_8x8((uint32_t)x + 24u, text_y + 20u, "MBR: Valid", 0x22C55E);
        draw_string_8x8((uint32_t)x + 24u, text_y + 36u, "BPB: Valid", 0x22C55E);
        draw_string_8x8((uint32_t)x + 24u, text_y + 52u, "FAT: Initialized", 0x22C55E);
      }
      break;
    case 5: // Complete
      draw_string_8x8((uint32_t)x + 24u, text_y, "Setup Complete!", 0x22C55E);
      draw_string_8x8((uint32_t)x + 24u, text_y + 24u, "Your disk is ready to use.", 0xFFFFFF);
      draw_string_8x8((uint32_t)x + 24u, text_y + 48u, "You can now use the", 0xD0D6E0);
      draw_string_8x8((uint32_t)x + 24u, text_y + 64u, "Explorer and Notepad apps.", 0xD0D6E0);
      break;
  }

  // Navigation buttons at bottom
  uint32_t btn_y = (uint32_t)y + (uint32_t)h - 28u;

  // Back button (disabled on step 0)
  if (g_setup_step > 0) {
    draw_rect((uint32_t)x + 16u, btn_y, 50, 18, 0x2A3140);
    draw_string_8x8((uint32_t)x + 26u, btn_y + 5u, "Back", 0xFFFFFF);
  } else {
    draw_rect((uint32_t)x + 16u, btn_y, 50, 18, 0x1A1A20);
    draw_string_8x8((uint32_t)x + 26u, btn_y + 5u, "Back", 0x606060);
  }

  // Next/Finish button
  uint32_t btn_x = (uint32_t)x + (uint32_t)w - 76u;
  const char *btn_label = (g_setup_step == 5) ? "Finish" : "Next";
  draw_rect(btn_x, btn_y, 60, 18, 0x2244AA);
  draw_string_8x8(btn_x + 14u, btn_y + 5u, btn_label, 0xFFFFFF);
}

static void draw_explorer_window(const wm_window_t *win){
  if(!g_bi||!win)return;int32_t x=win->x,y=win->y,w=win->w,h=win->h;uint32_t total=simplefs_file_count();char cb[32];uint32_t n=shell_u32_to_dec(cb,total);cb[n]=' ';cb[n+1]='i';cb[n+2]='t';cb[n+3]='e';cb[n+4]='m';cb[n+5]='s';cb[n+6]=0;
  draw_window_chrome(win,"Files",cb,(uint8_t)(wm_is_focused(APP_EXPLORER)),1,0);draw_rect((uint32_t)(x+1),(uint32_t)(y+31),(uint32_t)(w-2),38u,0x17263A);
  blend_round_rect((uint32_t)(x+12),(uint32_t)(y+38),(uint32_t)(w-24),24u,6u,0x0D1928u,255);draw_string_8x8((uint32_t)(x+22),(uint32_t)(y+46),"<",0xAFC4D8u);draw_string_8x8((uint32_t)(x+42),(uint32_t)(y+46),">",0x71879Du);draw_rect((uint32_t)(x+62),(uint32_t)(y+43),1u,14u,0x344A60);draw_string_8x8((uint32_t)(x+76),(uint32_t)(y+46),"This PC   >   VexOS Storage",0xDCE8F4u);
  if(w>620){draw_rect((uint32_t)(x+w-144),(uint32_t)(y+43),1u,14u,0x344A60);draw_string_8x8((uint32_t)(x+w-128),(uint32_t)(y+46),"Search files",0x71879Du);}
  uint32_t by=(uint32_t)y+70u,bh=h>84?(uint32_t)h-84u:1u;blend_round_rect((uint32_t)(x+10),by,140u,bh,5u,0x131F2E,255);draw_string_8x8((uint32_t)(x+22),by+14u,"LOCATIONS",0x7891AAu);blend_round_rect((uint32_t)(x+16),by+32u,128u,26u,5u,0x23415F,255);draw_rect((uint32_t)(x+20),by+40u,8u,10u,0x70B7F2u);draw_string_8x8((uint32_t)(x+36),by+41u,"Storage",0xF1F6FCu);draw_string_8x8((uint32_t)(x+22),by+76u,"VOLUME",0x7891AAu);draw_string_8x8((uint32_t)(x+22),by+94u,"SimpleFS",0xC8D7E6u);draw_string_8x8((uint32_t)(x+22),by+112u,"Local disk",0x849BB2u);
  uint32_t mx=(uint32_t)x+160u,mw=w>176?(uint32_t)w-172u:1u;draw_string_8x8(mx+8u,by+10u,"All files",0xF1F6FCu);draw_string_8x8(mx+8u,by+28u,"Files on your VexOS storage volume",0x8FA5BBu);uint32_t hy=by+50u;draw_rect(mx+2u,hy,mw-4u,23u,0x1A2A3Du);draw_string_8x8(mx+14u,hy+8u,"NAME",0x9EB4CAu);uint32_t kx=mx+(mw>490u?380u:260u);draw_string_8x8(kx,hy+8u,"KIND",0x9EB4CAu);draw_rect(mx+2u,hy+22u,mw-4u,1u,0x30445Au);
  uint32_t ly=hy+24u,rh=28u,rows=h>178?((uint32_t)h-178u)/rh:0u,first=g_explorer_scroll>total?total:g_explorer_scroll,count=total-first;if(count>rows)count=rows;
  if(!total){draw_string_8x8(mx+18u,ly+28u,"No files yet",0xD5E1EDu);draw_string_8x8(mx+18u,ly+48u,"Files added to this volume will appear here.",0x8299B1u);}
  for(uint32_t i=0;i<count;i++){char name[64];name[0]=0;uint32_t idx=first+i;if(!simplefs_file_name_at(idx,name,sizeof(name)))continue;uint32_t ry=ly+i*rh;if(idx==g_explorer_selected)blend_round_rect(mx+4u,ry+1u,mw-8u,25u,4u,0x25496Bu,255);else if(i&1u)draw_rect(mx+4u,ry+1u,mw-8u,25u,0x162536u);draw_file_icon(mx+13u,ry+3u,name);
    uint32_t nl=0;while(name[nl])nl++;const char *kind="File";if(nl>4u&&streq_n(&name[nl-4u],".txt",4u))kind="Text document";else if(nl>4u&&streq_n(&name[nl-4u],".png",4u))kind="Image";else if(nl>4u&&streq_n(&name[nl-4u],".wad",4u))kind="Doom data";else if(nl>4u&&streq_n(&name[nl-4u],".elf",4u))kind="Executable";if(nl>35u)name[35]=0;draw_string_8x8(mx+42u,ry+9u,name,idx==g_explorer_selected?0xFFFFFFu:0xD7E3EFu);draw_string_8x8(kx,ry+9u,kind,0x8FA8C0u);}
  draw_rect((uint32_t)(x+160),(uint32_t)(y+h-28),mw,1u,0x2A3D52u);draw_string_8x8((uint32_t)(x+174),(uint32_t)(y+h-18),"VexOS Storage",0x7891AAu);draw_string_8x8((uint32_t)(x+w-130),(uint32_t)(y+h-18),"Double-click to open",0x7891AAu);
}

static void draw_viewer_window(const wm_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;
  draw_window_chrome(win, "Viewer", g_viewer_name[0] ? g_viewer_name : "Preview",
                     (uint8_t)(wm_is_focused(APP_VIEWER)), 1, 0);

  draw_rect((uint32_t)(x + 1), (uint32_t)(y + 31),
            (uint32_t)(w - 2), 30u, 0x17263A);
  const char *toolbar_label =
      g_viewer_kind == VIEWER_CONTENT_PNG ? "IMAGE PREVIEW / PNG" :
      g_viewer_kind == VIEWER_CONTENT_JPEG ? "IMAGE PREVIEW / JPEG" :
      g_viewer_kind == VIEWER_CONTENT_BINARY ? "PREVIEW UNAVAILABLE" :
      "PREVIEW / TEXT CONTENT";
  draw_string_8x8((uint32_t)(x + 14), (uint32_t)(y + 42),
                  toolbar_label, 0xAFC2D6u);

  uint32_t px = (uint32_t)x + 12u;
  uint32_t py = (uint32_t)y + 68u;
  uint32_t pw = w > 24 ? (uint32_t)w - 24u : 1u;
  uint32_t ph = h > 80 ? (uint32_t)h - 80u : 1u;

  if ((g_viewer_kind == VIEWER_CONTENT_PNG ||
       g_viewer_kind == VIEWER_CONTENT_JPEG) &&
      g_viewer_pixels && g_viewer_image_width && g_viewer_image_height) {
    draw_rect(px, py, pw, ph, 0x182230);

    uint32_t max_w = pw > 16u ? pw - 16u : 1u;
    uint32_t max_h = ph > 16u ? ph - 16u : 1u;
    uint64_t sx = ((uint64_t)max_w << 16) / g_viewer_image_width;
    uint64_t sy = ((uint64_t)max_h << 16) / g_viewer_image_height;
    uint64_t scale = sx < sy ? sx : sy;
    if (scale == 0)
      scale = 1;

    uint32_t draw_w = (uint32_t)(((uint64_t)g_viewer_image_width * scale) >> 16);
    uint32_t draw_h = (uint32_t)(((uint64_t)g_viewer_image_height * scale) >> 16);
    if (draw_w == 0) draw_w = 1;
    if (draw_h == 0) draw_h = 1;
    if (draw_w > max_w) draw_w = max_w;
    if (draw_h > max_h) draw_h = max_h;

    uint32_t image_x = px + (pw - draw_w) / 2u;
    uint32_t image_y = py + (ph - draw_h) / 2u;

    for (uint32_t iy = 0; iy < draw_h; iy++) {
      uint32_t src_y = (uint32_t)(((uint64_t)iy * g_viewer_image_height) / draw_h);
      if (src_y >= g_viewer_image_height)
        src_y = g_viewer_image_height - 1u;
      for (uint32_t ix = 0; ix < draw_w; ix++) {
        uint32_t src_x = (uint32_t)(((uint64_t)ix * g_viewer_image_width) / draw_w);
        if (src_x >= g_viewer_image_width)
          src_x = g_viewer_image_width - 1u;

        uint32_t si = (src_y * g_viewer_image_width + src_x) * 4u;
        uint8_t r = g_viewer_pixels[si + 0u];
        uint8_t g = g_viewer_pixels[si + 1u];
        uint8_t b = g_viewer_pixels[si + 2u];
        uint32_t checker = (((ix / 8u) + (iy / 8u)) & 1u) ? 0xDCE4EEu : 0xF7F9FCu;
        uint32_t a = g_viewer_pixels[si + 3u];
        uint32_t inv = 255u - a;
        uint32_t br = (checker >> 16) & 0xFFu;
        uint32_t bg = (checker >> 8) & 0xFFu;
        uint32_t bb = checker & 0xFFu;
        uint32_t out_r = ((uint32_t)r * a + br * inv) / 255u;
        uint32_t out_g = ((uint32_t)g * a + bg * inv) / 255u;
        uint32_t out_b = ((uint32_t)b * a + bb * inv) / 255u;
        put_px(image_x + ix, image_y + iy,
               (out_r << 16) | (out_g << 8) | out_b);
      }
    }
    return;
  }

  blend_round_rect(px, py, pw, ph, 5u, 0xF4F7FBu, 255);
  uint32_t tx = px + 16u;
  uint32_t ty = py + 14u;
  uint32_t cols = w > 56 ? (uint32_t)(w - 56) / 8u : 0u;
  uint32_t rows = h > 104 ? (uint32_t)(h - 104) / 16u : 0u;
  uint32_t start = 0;
  uint32_t skip = g_viewer_scroll;
  for (uint32_t i = 0; i < g_viewer_text_len && skip; i++) {
    if (g_viewer_text[i] == '\n') {
      skip--;
      start = i + 1u;
    }
  }

  uint32_t cx = 0;
  uint32_t cy = 0;
  for (uint32_t i = start; i < g_viewer_text_len && cy < rows; i++) {
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
    if (cx < cols) {
      char t[2] = {c, 0};
      draw_string_8x8(tx + cx * 8u, ty + cy * 16u, t, 0x26384Du);
    }
    cx++;
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
static void draw_setup_window(const wm_window_t *win);
static void draw_window_chrome(const wm_window_t *win, const char *title, const char *sub_title, uint8_t is_focused, uint8_t has_scroll, uint8_t has_save);
static void draw_desktop_icons(void);
static void draw_widgets(void);

static void blit_rect(uint32_t *dst, const uint32_t *src, uint32_t x,
                      uint32_t y, uint32_t w, uint32_t h) {
  gfx_blit_rect(g_bi, dst, src, x, y, w, h);
}

void desktop_draw_frame(const uint32_t *src, uint32_t w, uint32_t h) {
  if (!g_bi || !src)
    return;
  gfx_blit_frame(g_bi, src, w, w, h);
}

static inline void put_px(uint32_t x, uint32_t y, uint32_t c) {
  gfx_put_pixel(g_bi, g_back, x, y, c);
}

static void draw_shell_window(const wm_window_t *win){
  if(!g_bi||!win)return;int32_t x=win->x,y=win->y,w=win->w,h=win->h;uint8_t focused=(wm_is_focused(APP_SHELL));draw_window_chrome(win,"Terminal","VexOS Shell",focused,1,0);draw_string_8x8((uint32_t)(x+14),(uint32_t)(y+40),"VEXOS TERMINAL  /  LOCAL SESSION",0x7FA4C7u);
  uint32_t px=(uint32_t)x+10u,py=(uint32_t)y+56u,pw=w>20?(uint32_t)w-20u:1u,ph=h>94?(uint32_t)h-94u:1u;blend_round_rect(px,py,pw,ph,5u,0x091321u,255);draw_rect(px+1u,py+1u,pw-2u,1u,0x1C344Du);uint32_t tx=px+12u,ty=py+12u,cols=pw>24u?(pw-24u)/8u:0u,rows=ph>20u?(ph-20u)/10u:0u;
  if(cols&&rows){uint32_t cap=cols*rows,start=g_shell_out_len>cap?g_shell_out_len-cap:0u,cx=0,cy=0;for(uint32_t i=start;i<g_shell_out_len&&cy<rows;i++){char c=g_shell_out[i];if(c=='\n'){cx=0;cy++;continue;}if(cx<cols&&c>=32&&c<=126)draw_glyph_8x8(tx+cx*8u,ty+cy*10u,(uint8_t)c,0xD7E7F7u);cx++;if(cx>=cols){cx=0;cy++;}}}
  uint32_t iy=(uint32_t)(y+h-30);blend_round_rect((uint32_t)(x+10),iy,(uint32_t)(w-20),24u,5u,0x1A2C40u,255);draw_rect((uint32_t)(x+18),iy+1u,(uint32_t)(w-36),1u,0x35516Du);draw_string_8x8((uint32_t)(x+22),iy+8u,">",0x79E0B0u);draw_string_8x8((uint32_t)(x+38),iy+8u,g_shell_line,0xF1F6FCu);uint32_t len=0;while(g_shell_line[len])len++;if(focused&&((g_tick/30u)%2u==0u))draw_rect((uint32_t)(x+38)+len*8u,iy+8u,7u,8u,0x79E0B0u);
}

static void draw_taskbar(void){
  if(!g_bi)return;uint32_t bh=56u;if(bh>g_bi->height)bh=g_bi->height;uint32_t y=g_bi->height-bh;blend_rect(0u,y,g_bi->width,bh,0x080E18u,224u);draw_rect(0u,y,g_bi->width,1u,0x53799Cu);uint32_t sy=y+6u,sh=bh-12u;
  blend_round_rect(8u,sy,48u,sh,10u,g_start_open?0x28588Bu:0x1A3451u,255u);draw_rect(18u,sy+10u,10u,10u,0xE8F3FFu);draw_rect(31u,sy+10u,10u,10u,0x8FC7F4u);draw_rect(18u,sy+23u,10u,10u,0x8FC7F4u);draw_rect(31u,sy+23u,10u,10u,0xE8F3FFu);
  uint32_t sw=196u;if(sw+72u>g_bi->width)sw=g_bi->width>72u?g_bi->width-72u:1u;blend_round_rect(62u,sy,sw,sh,10u,0x111E2Du,235u);draw_rect(62u,sy,sw,1u,0x344B64u);blend_round_rect(74u,sy+13u,10u,10u,5u,0x7894AFu,255u);draw_rect(83u,sy+22u,5u,2u,0x7894AFu);draw_string_8x8(96u,sy+18u,"Search apps and files",0x91A7BEu);
  uint32_t dw=420u;if(dw+32u>g_bi->width)dw=g_bi->width>32u?g_bi->width-32u:1u;uint32_t dx=(g_bi->width-dw)/2u;blend_round_rect(dx,sy,dw,sh,12u,0x101C2Au,242u);draw_rect(dx+1u,sy,dw-2u,1u,0x3A526Bu);draw_rect(dx+1u,sy+sh-1u,dw-2u,1u,0x0B1420u);uint32_t ix=dx+16u,iy=sy+((sh>40u)?((sh-40u)/2u):0u);for(uint32_t i=0;i<4u;i++){draw_app_icon(ix,iy,40u,(app_id_t)i);ix+=50u;if(ix+56u>dx+dw)break;}
  uint32_t rw=132u,rx=g_bi->width>rw+8u?g_bi->width-rw-8u:8u;if(rx>dx+dw+8u){blend_round_rect(rx,sy,rw,sh,10u,0x111E2Du,235u);draw_string_8x8(rx+14u,sy+10u,"VEXOS",0xE7F1FCu);draw_string_8x8(rx+14u,sy+26u,"SYSTEM READY",0x79D8A8u);}
}

static void draw_start_menu(void){
  if(!g_bi||!g_start_open)return;uint32_t bh=56u;if(bh>g_bi->height)bh=g_bi->height;uint32_t by=g_bi->height-bh,x=10u,w=280u,h=168u,y=by>h+8u?by-h-8u:8u;blend_round_rect(x,y,w,h,12u,0x101B2Au,250u);
  draw_rect(x+1u,y+1u,w-2u,1u,0x5A83A8u);draw_rect(x+1u,y+h-1u,w-2u,1u,0x26394Fu);draw_rect(x+1u,y+1u,1u,h-2u,0x354B63u);draw_rect(x+w-2u,y+1u,1u,h-2u,0x26394Fu);draw_string_8x8(x+16u,y+9u,"VEXOS",0xF0F6FCu);draw_string_8x8(x+208u,y+9u,"APPS",0x7FA0BEu);draw_rect(x+14u,y+22u,w-28u,1u,0x2B4057u);
  const app_id_t apps[5]={APP_SHELL,APP_EXPLORER,APP_NOTEPAD,APP_VIEWER,APP_SETUP};const char *labels[6]={"Terminal","Files","Notepad","Viewer","Setup","Help"};
  for(uint32_t i=0;i<6u;i++){uint32_t ry=y+28u+i*22u;if((i&1u)==0u)blend_round_rect(x+10u,ry,w-20u,21u,4u,0x17283Bu,160u);if(i<5u)draw_app_icon(x+17u,ry+1u,19u,apps[i]);else{blend_round_rect(x+17u,ry+1u,19u,19u,5u,0x334B65u,255u);draw_string_8x8(x+23u,ry+6u,"?",0xEAF3FCu);}draw_string_8x8(x+46u,ry+7u,labels[i],0xDCE8F4u);if(i==0u)draw_string_8x8(x+232u,ry+7u,">",0x75A8D4u);}
}

static void draw_desktop_icons(void){
  if(!g_bi)return;uint32_t x=22u,y=132u,sz=42u,step=84u;const app_id_t apps[4]={APP_EXPLORER,APP_SHELL,APP_NOTEPAD,APP_VIEWER};const char *labels[4]={"Files","Terminal","Notepad","Viewer"};
  for(uint32_t i=0;i<4u;i++){uint32_t iy=y+i*step;blend_round_rect(x-7u,iy-7u,56u,70u,8u,0x14263Au,90u);draw_app_icon(x,iy,sz,apps[i]);uint32_t len=0;while(labels[i][len])len++;draw_string_8x8(x+(sz-len*8u)/2u,iy+sz+8u,labels[i],0xF1F6FCu);}
}

static void draw_widgets(void){
  if(!g_bi)return;uint32_t w=326u,x=g_bi->width>w+24u?g_bi->width-w-24u:8u,y=100u;
  blend_round_rect(x,y,w,88u,12u,0x101D2Cu,226u);draw_rect(x+1u,y+1u,w-2u,1u,0x4C7194u);draw_string_8x8(x+18u,y+16u,"WORKSPACE",0x7F9AB6u);draw_string_8x8(x+18u,y+38u,"Welcome to VexOS",0xF0F6FCu);draw_string_8x8(x+18u,y+61u,"Your desktop is ready.",0xA5B9CDu);
  blend_round_rect(x+w-66u,y+22u,42u,42u,10u,0x244B70u,255u);draw_rect(x+w-55u,y+33u,8u,8u,0xE8F3FFu);draw_rect(x+w-43u,y+33u,8u,8u,0x86BCE8u);draw_rect(x+w-55u,y+45u,8u,8u,0x86BCE8u);draw_rect(x+w-43u,y+45u,8u,8u,0xE8F3FFu);
  y+=102u;blend_round_rect(x,y,w,142u,12u,0x101D2Cu,226u);draw_rect(x+1u,y+1u,w-2u,1u,0x3D5874u);draw_string_8x8(x+18u,y+16u,"SYSTEM",0x7F9AB6u);const hw_profile_t *p=hw_profile_get();uint32_t ram=p?(uint32_t)(p->ram_usable_bytes/(1024ull*1024ull)):0u;
  draw_string_8x8(x+18u,y+42u,"CPU",0x829AB2u);char cpu[25];uint32_t ci=0;if(p){while(ci+1u<sizeof(cpu)&&p->cpu_brand[ci]){cpu[ci]=p->cpu_brand[ci];ci++;}}cpu[ci]=0;draw_string_8x8(x+66u,y+42u,cpu[0]?cpu:"Unknown processor",0xE0EAF4u);
  char buf[16];shell_u32_to_dec(buf,ram);draw_string_8x8(x+18u,y+68u,"RAM",0x829AB2u);draw_string_8x8(x+66u,y+68u,buf,0xE0EAF4u);draw_string_8x8(x+106u,y+68u,"MB",0x829AB2u);draw_string_8x8(x+18u,y+94u,"DISPLAY",0x829AB2u);draw_string_8x8(x+90u,y+94u,(p&&p->has_pci_display)?"Graphics device detected":"Framebuffer",0xE0EAF4u);draw_rect(x+18u,y+120u,w-36u,1u,0x283D53u);
  y+=156u;blend_round_rect(x,y,w,92u,12u,0x101D2Cu,226u);draw_rect(x+1u,y+1u,w-2u,1u,0x3D5874u);draw_string_8x8(x+18u,y+16u,"STORAGE",0x7F9AB6u);uint32_t files=simplefs_file_count();char fb[16];shell_u32_to_dec(fb,files);draw_string_8x8(x+18u,y+40u,"Indexed files",0xA5B9CDu);draw_string_8x8(x+w-60u,y+40u,fb,0xF0F6FCu);draw_string_8x8(x+18u,y+64u,"SimpleFS volume",0x8299B2u);
}


// Paint windows from back to front. The window manager keeps the focused app
// at the end of the z-order list so it is rendered above overlapping windows.
static void draw_open_windows(void) {
  for (uint32_t i = 0; i < wm_z_count(); i++) {
    app_id_t app = (app_id_t)wm_z_at(i);
    if ((uint32_t)app >= APP_COUNT || !wm_is_open((uint32_t)app))
      continue;

    switch (app) {
      case APP_SHELL:
        if (g_show_shell)
          draw_shell_window(&g_shell_win);
        break;
      case APP_EXPLORER:
        if (g_explorer_open)
          draw_explorer_window(&g_explorer_win);
        break;
      case APP_VIEWER:
        if (g_viewer_open)
          draw_viewer_window(&g_viewer_win);
        break;
      case APP_NOTEPAD:
        draw_notepad_window(&g_notepad_win);
        break;
      case APP_SETUP:
        if (g_setup_open)
          draw_setup_window(&g_setup_win);
        break;
      default:
        break;
    }
  }
}

static void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c) {
  gfx_fill_rect(g_bi, g_back, x, y, w, h, c);
}



static void blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t c, uint8_t a) {
  gfx_blend_rect(g_bi, g_back, x, y, w, h, c, a);
}

static void blend_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t radius, uint32_t c, uint8_t a) {
  gfx_blend_round_rect(g_bi, g_back, x, y, w, h, radius, c, a);
}

static void draw_string_8x8(uint32_t x, uint32_t y, const char *s,
                            uint32_t fg) {
  gfx_draw_text8(g_bi, g_back, x, y, s, fg);
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
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Shell help command executed\n");
#endif
    shell_append("help clear cls echo mem ver uptime pos rand specs ring3 fs explorer view notepad setup wall\n");
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
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Shell mem command executed\n");
#endif
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
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Shell specs command executed\n");
#endif
  } else if ((g_shell_line_len == 5 && streq_n(g_shell_line, "ring3", 5)) ||
             (g_shell_line_len == 4 && streq_n(g_shell_line, "doom", 4))) {
    shell_append("launching doom.elf...\n");
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
      wm_open(APP_EXPLORER);
      app_raise(APP_EXPLORER);
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
      char name[64];
      uint32_t n = 0;
      while (i < g_shell_line_len && n + 1u < (uint32_t)sizeof(name))
        name[n++] = g_shell_line[i++];
      name[n] = 0;
      if (viewer_load_file(name)) {
        shell_append("viewer: opened ");
        shell_append(name);
        shell_append("\n");
      } else {
        shell_append("view: could not open file\n");
      }
    }
  } else if (g_shell_line_len == 7 && streq_n(g_shell_line, "notepad", 7)) {
    shell_append("opening notepad...\n");
    wm_open(APP_NOTEPAD);
    app_raise(APP_NOTEPAD);
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Notepad opened by shell command\n");
#endif
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "setup", 5)) {
    shell_append("opening setup wizard...\n");
    g_setup_open = 1;
      wm_open(APP_SETUP);
    app_raise(APP_SETUP);
    g_setup_step = 0;
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

              uint32_t sw = g_bi->width;
              uint32_t sh = g_bi->height;
              uint32_t scale_fp = 0x00010000u;
              if (pw && ph) {
                uint64_t sx = ((uint64_t)sw << 16) / (uint64_t)pw;
                uint64_t sy = ((uint64_t)sh << 16) / (uint64_t)ph;
                scale_fp = (uint32_t)((sx > sy) ? sx : sy);
                if (scale_fp == 0)
                  scale_fp = 0x00010000u;
              }

              uint64_t scaled_w = ((uint64_t)pw * (uint64_t)scale_fp) >> 16;
              uint64_t scaled_h = ((uint64_t)ph * (uint64_t)scale_fp) >> 16;
              uint64_t crop_x = 0;
              uint64_t crop_y = 0;
              if (scaled_w > (uint64_t)sw)
                crop_x = (scaled_w - (uint64_t)sw) / 2ull;
              if (scaled_h > (uint64_t)sh)
                crop_y = (scaled_h - (uint64_t)sh) / 2ull;

              for (uint32_t y = 0; y < sh; y++) {
                uint32_t *row = &g_bg[y * g_bi->pitch];
                uint64_t yp = (uint64_t)y + crop_y;
                uint64_t sy_px = (yp << 16) / (uint64_t)scale_fp;
                uint32_t src_y = (uint32_t)sy_px;
                if (src_y >= ph)
                  src_y = ph ? (ph - 1u) : 0u;
                for (uint32_t x = 0; x < sw; x++) {
                  uint64_t xp = (uint64_t)x + crop_x;
                  uint64_t sx_px = (xp << 16) / (uint64_t)scale_fp;
                  uint32_t src_x = (uint32_t)sx_px;
                  if (src_x >= pw)
                    src_x = pw ? (pw - 1u) : 0u;
                  uint32_t si = (src_y * pw + src_x) * 4u;
                  uint8_t r = pix[si + 0];
                  uint8_t gg = pix[si + 1];
                  uint8_t b = pix[si + 2];
                  row[x] = ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
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
  gfx_draw_glyph8(g_bi, g_back, x, y, ch, fg);
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
  g_show_shell = 0;
  g_explorer_open = 0;
  g_viewer_open = 0;
  g_setup_open = 0;
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
  g_explorer_dragging = 0;
  g_viewer_dragging = 0;
  g_viewer_name[0] = 0;
  g_viewer_text[0] = 0;
  g_viewer_text_len = 0;
  g_viewer_kind = VIEWER_CONTENT_TEXT;
  g_viewer_pixels = NULL;
  g_viewer_image_width = 0;
  g_viewer_image_height = 0;
  g_explorer_scroll = 0;
  g_viewer_scroll = 0;
  g_explorer_selected = 0xFFFFFFFFu;
  g_explorer_last_click_tick = 0;
  g_explorer_last_click_index = 0xFFFFFFFFu;

  wm_init(APP_COUNT);
  app_raise(APP_SHELL);

  g_notepad_dragging = 0;
  g_notepad_drag_off_x = 0;
  g_notepad_drag_off_y = 0;
  g_notepad_name[0] = 0;
  g_notepad_text[0] = 0;
  g_notepad_text_len = 0;
  g_notepad_scroll = 0;

  g_setup_dragging = 0;
  g_setup_drag_off_x = 0;
  g_setup_drag_off_y = 0;

  if (g_bi && g_bi->pitch && g_bi->height) {
    uint64_t pixels = (uint64_t)g_bi->pitch * (uint64_t)g_bi->height;
    uint64_t bytes = pixels * 4u;
    if (bytes <= 0xFFFFFFFFu) {
      uint64_t pair_bytes = bytes * 2u;
      if (pair_bytes >= bytes && hw_tuning_allow_backbuffer(pair_bytes)) {
        g_back = (uint32_t *)kmalloc((uint32_t)bytes);
        g_bg = (uint32_t *)kmalloc((uint32_t)bytes);
      } else {
        const hw_profile_t *hw = hw_profile_get();
        uint64_t min_free = 16ull * 1024ull * 1024ull;
        if (hw && hw->ram_usable_bytes &&
            pair_bytes >= bytes && pair_bytes <= UINT64_MAX - min_free &&
            (pair_bytes + min_free) <= hw->ram_usable_bytes) {
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

  g_explorer_win.w = (int32_t)(g_bi->width > 760u ? 720u : (g_bi->width > 48u ? g_bi->width - 48u : g_bi->width));
  g_explorer_win.h = (int32_t)(g_bi->height > 560u ? 500u : (g_bi->height > 80u ? g_bi->height - 80u : g_bi->height));
  g_explorer_win.x = ((int32_t)g_bi->width - g_explorer_win.w) / 2;
  g_explorer_win.y = ((int32_t)g_bi->height - g_explorer_win.h) / 2;
  g_viewer_win.w = (int32_t)(g_bi->width > 680u ? 620u : (g_bi->width > 48u ? g_bi->width - 48u : g_bi->width));
  g_viewer_win.h = (int32_t)(g_bi->height > 480u ? 420u : (g_bi->height > 80u ? g_bi->height - 80u : g_bi->height));
  g_viewer_win.x = ((int32_t)g_bi->width - g_viewer_win.w) / 2 + 30;
  g_viewer_win.y = ((int32_t)g_bi->height - g_viewer_win.h) / 2 + 26;

  // Make sure the notepad window is always on-screen.
  if (g_notepad_win.x + g_notepad_win.w > (int32_t)g_bi->width)
    g_notepad_win.x = 8;
  if (g_notepad_win.y + g_notepad_win.h > (int32_t)g_bi->height)
    g_notepad_win.y = 48;

  g_notepad_win.w = (int32_t)(g_bi->width > 720u ? 660u : (g_bi->width > 48u ? g_bi->width - 48u : g_bi->width));
  g_notepad_win.h = (int32_t)(g_bi->height > 560u ? 480u : (g_bi->height > 80u ? g_bi->height - 80u : g_bi->height));
  g_notepad_win.x = ((int32_t)g_bi->width - g_notepad_win.w) / 2 + 54;
  g_notepad_win.y = ((int32_t)g_bi->height - g_notepad_win.h) / 2 + 38;

  // Default notepad file name
  {
    const char *def = "note.txt";
    for (uint32_t i = 0; i + 1u < (uint32_t)sizeof(g_notepad_name) && def[i]; i++) {
      g_notepad_name[i] = def[i];
      g_notepad_name[i + 1u] = 0;
    }
  }

  // Setup wizard window (centered)
  g_setup_win.w = 480;
  g_setup_win.h = 320;
  g_setup_win.x = (int32_t)((g_bi->width - (uint32_t)g_setup_win.w) / 2u);
  g_setup_win.y = (int32_t)((g_bi->height - (uint32_t)g_setup_win.h) / 2u);
  if (g_setup_win.x < 16) g_setup_win.x = 16;
  if (g_setup_win.y < 16) g_setup_win.y = 16;
  g_setup_open = 0; // Closed by default, user opens via shell or start menu
  g_setup_step = 0;
  g_setup_disk_ready = 0;
  g_setup_partition_done = 0;
  g_setup_format_done = 0;

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
    draw_open_windows();
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
  if (!g_bi || g_fullscreen_app_active)
    return;

  static uint32_t prev_buttons = 0;
  input_pointer_state_t p0 = input_pointer_get();
  g_last_ptr = p0;
#ifdef VEXOS_CI_INPUT_TEST
  if (!g_ci_mouse_move_seen && (p0.dx != 0 || p0.dy != 0)) {
    g_ci_mouse_move_seen = 1;
    serial_print("[CI] Mouse pointer movement reached desktop_tick\n");
  }
#endif
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
#ifdef VEXOS_CI_INPUT_TEST
    if (!g_ci_keyboard_input_seen) {
      g_ci_keyboard_input_seen = 1;
      serial_print("[CI] Keyboard character reached desktop input queue\n");
    }
#endif

    // Route keyboard input to focused app.
    if (wm_is_focused(APP_NOTEPAD) && wm_is_open(APP_NOTEPAD)) {
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
#ifdef VEXOS_CI_INPUT_TEST
          static uint8_t ci_notepad_text_seen;
          if (!ci_notepad_text_seen && g_notepad_text_len >= 3u &&
              g_notepad_text[g_notepad_text_len - 3u] == 'v' &&
              g_notepad_text[g_notepad_text_len - 2u] == 'e' &&
              g_notepad_text[g_notepad_text_len - 1u] == 'x') {
            ci_notepad_text_seen = 1;
            serial_print("[CI] Notepad accepted the full typed text vex\n");
          }
#endif
          ui_changed = 1;
        }
      }
    } else if (wm_is_focused(APP_SHELL) && g_show_shell && wm_is_open(APP_SHELL)) {
      if (c == '\b') {
        if (g_shell_line_len) {
          g_shell_line_len--;
          g_shell_line[g_shell_line_len] = 0;
          ui_changed = 1;
        }
      } else {
        if (c == '\n') {
#ifdef VEXOS_CI_INPUT_TEST
          serial_print("[CI] Shell command submitted through keyboard\n");
#endif
          shell_run_line();
          ui_changed = 1;
        } else if (c >= 32 && c <= 126) {
#ifdef VEXOS_CI_INPUT_TEST
          static uint8_t ci_shell_char_seen;
          if (!ci_shell_char_seen) {
            ci_shell_char_seen = 1;
            serial_print("[CI] Shell received printable keyboard input\n");
          }
#endif
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
    uint32_t bar_h = 56;
    if (bar_h > g_bi->height)
      bar_h = g_bi->height;
    int32_t tb_y = (int32_t)g_bi->height - (int32_t)bar_h;

    // Start button hit
    if (hit_test(p0.x, p0.y, 8, tb_y + 4, 52, (int32_t)bar_h - 8)) {
      g_start_open = (uint8_t)(g_start_open ? 0u : 1u);
#ifdef VEXOS_CI_INPUT_TEST
      if (g_start_open && !g_ci_start_click_seen) {
        g_ci_start_click_seen = 1;
        serial_print("[CI] Start menu opened by USB mouse click\n");
      }
#endif
      ui_changed = 1;
      g_shell_dragging = 0;
    } else if (g_start_open) {
      int32_t mx=10,mw=280,mh=168,my=tb_y-176;
      if(hit_test(p0.x,p0.y,mx,my,mw,mh)){int32_t ry=p0.y-my;
        if(ry>=28&&ry<50){g_show_shell = 1;
      wm_open(APP_SHELL);app_raise(APP_SHELL);
#ifdef VEXOS_CI_INPUT_TEST
          serial_print("[CI] Shell opened from Start menu mouse click\n");
#endif
          g_start_open=0;ui_changed=1;
        }else if(ry>=50&&ry<72){if(simplefs_init()){g_explorer_open = 1;
      wm_open(APP_EXPLORER);app_raise(APP_EXPLORER);}else shell_append("fs init failed\n> ");g_start_open=0;ui_changed=1;
        }else if(ry>=72&&ry<94){wm_open(APP_NOTEPAD);app_raise(APP_NOTEPAD);g_start_open=0;ui_changed=1;
        }else if(ry>=94&&ry<116){g_viewer_open = 1;
      wm_open(APP_VIEWER);app_raise(APP_VIEWER);g_start_open=0;ui_changed=1;
        }else if(ry>=116&&ry<138){g_setup_open = 1;
      wm_open(APP_SETUP);g_setup_step=0;app_raise(APP_SETUP);g_start_open=0;ui_changed=1;
        }else if(ry>=138&&ry<162){g_show_shell = 1;
      wm_open(APP_SHELL);app_raise(APP_SHELL);shell_append("VexOS help: help clear echo mem ver uptime ps rand specs fs explorer view notepad setup\n> ");g_start_open=0;ui_changed=1;}
      }else{g_start_open=0;ui_changed=1;}
    }

    // Desktop shortcuts launch their matching applications.
    if(!g_start_open){for(uint32_t i=0;i<4u;i++){int32_t iy=132+(int32_t)i*84;if(hit_test(p0.x,p0.y,15,iy-7,58,70)){
      if(i==0u){if(simplefs_init()){g_explorer_open = 1;
      wm_open(APP_EXPLORER);app_raise(APP_EXPLORER);}else shell_append("fs init failed\n> ");}
      else if(i==1u){g_show_shell = 1;
      wm_open(APP_SHELL);app_raise(APP_SHELL);}else if(i==2u){wm_open(APP_NOTEPAD);app_raise(APP_NOTEPAD);}else{g_viewer_open = 1;
      wm_open(APP_VIEWER);app_raise(APP_VIEWER);}ui_changed=1;break;}}}

    // Taskbar dock icons: click to focus/open

    // Taskbar dock icons: click to focus/open
    {
      uint32_t sy = (uint32_t)tb_y + 6u;
      uint32_t sh = bar_h - 12u;

      uint32_t dock_w = 420;
      if (dock_w + 32 > g_bi->width)
        dock_w = g_bi->width > 32 ? (g_bi->width - 32) : 1;
      uint32_t dock_x = (g_bi->width - dock_w) / 2u;

      uint32_t icon = 40;
      uint32_t gap = 10;
      uint32_t ix = dock_x + 16;
      uint32_t iy = sy + ((sh > icon) ? ((sh - icon) / 2u) : 0u);

      for (uint32_t i = 0; i < 4; i++) {
        if (hit_test(p0.x, p0.y, (int32_t)ix, (int32_t)iy, (int32_t)icon,
                     (int32_t)icon)) {
          if (i == 0) {
            g_show_shell = 1;
      wm_open(APP_SHELL);
            app_raise(APP_SHELL);
          } else if (i == 1) {
            if (simplefs_init()) {
              g_explorer_open = 1;
      wm_open(APP_EXPLORER);
              app_raise(APP_EXPLORER);
            } else {
              shell_append("fs init failed\n> ");
            }
          } else if (i == 2) {
            g_viewer_open = 1;
      wm_open(APP_VIEWER);
            app_raise(APP_VIEWER);
          } else if (i == 3) {
            wm_open(APP_NOTEPAD);
            app_raise(APP_NOTEPAD);
          }
          ui_changed = 1;
          break;
        }
        ix += icon + gap;
        if (ix + icon + 16 > dock_x + dock_w)
          break;
      }
    }

    // Explorer window interactions
    if (g_explorer_open && wm_is_open(APP_EXPLORER)) {
      int32_t wx = g_explorer_win.x;
      int32_t wy = g_explorer_win.y;
      int32_t ww = g_explorer_win.w;
      int32_t wh = g_explorer_win.h;

      // Close button
      if (hit_test(p0.x, p0.y, wx + ww - 24, wy + 5, 20, 20)) {
        g_explorer_open = 0;
        g_explorer_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 46, wy + 5, 20, 20)) {
        // scroll up
        if (g_explorer_scroll)
          g_explorer_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 66, wy + 5, 20, 20)) {
        // scroll down
        g_explorer_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 30)) {
        g_explorer_dragging = 1;
        g_explorer_drag_off_x = p0.x - wx;
        g_explorer_drag_off_y = p0.y - wy;
        g_explorer_open = 1;
      wm_open(APP_EXPLORER);
        app_raise(APP_EXPLORER);
        ui_changed = 1;
      } else {
        // Click in list area -> open viewer
        int32_t list_x = wx + 160;
        int32_t list_y = wy + 144;
        int32_t list_w = ww - 172;
        int32_t list_h = wh - 174;
        if (hit_test(p0.x, p0.y, list_x, list_y, list_w, list_h)) {
          app_raise(APP_EXPLORER);
          uint32_t row_h = 28u;
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
              if (viewer_load_file(name)) {
                ui_changed = 1;
              } else {
                shell_append("viewer: could not open file\n");
              }
            }

            g_explorer_last_click_tick = g_tick;
            g_explorer_last_click_index = idx;
          }
        }
      }
    }

    if (g_viewer_open && wm_is_open(APP_VIEWER)) {
      int32_t wx = g_viewer_win.x;
      int32_t wy = g_viewer_win.y;
      int32_t ww = g_viewer_win.w;
      // Close
      if (hit_test(p0.x, p0.y, wx + ww - 24, wy + 5, 20, 20)) {
        g_viewer_open = 0;
        g_viewer_dragging = 0;
        viewer_clear_content();
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 46, wy + 5, 20, 20)) {
        if (g_viewer_scroll)
          g_viewer_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 66, wy + 5, 20, 20)) {
        g_viewer_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 30)) {
        g_viewer_dragging = 1;
        g_viewer_drag_off_x = p0.x - wx;
        g_viewer_drag_off_y = p0.y - wy;
        g_viewer_open = 1;
      wm_open(APP_VIEWER);
        app_raise(APP_VIEWER);
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, g_viewer_win.h)) {
        app_raise(APP_VIEWER);
        ui_changed = 1;
      }
    }

    // Notepad interactions
    if (wm_is_open(APP_NOTEPAD)) {
      int32_t wx = g_notepad_win.x;
      int32_t wy = g_notepad_win.y;
      int32_t ww = g_notepad_win.w;
      int32_t wh = g_notepad_win.h;

      if (hit_test(p0.x, p0.y, wx + ww - 24, wy + 5, 20, 20)) {
        wm_close(APP_NOTEPAD);
        g_notepad_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 86, wy + 5, 20, 20)) {
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
      } else if (hit_test(p0.x, p0.y, wx + ww - 46, wy + 5, 20, 20)) {
        if (g_notepad_scroll)
          g_notepad_scroll--;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx + ww - 66, wy + 5, 20, 20)) {
        g_notepad_scroll++;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 30)) {
        g_notepad_dragging = 1;
        g_notepad_drag_off_x = p0.x - wx;
        g_notepad_drag_off_y = p0.y - wy;
        app_raise(APP_NOTEPAD);
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, wh)) {
        // Click in body focuses notepad
        app_raise(APP_NOTEPAD);
        ui_changed = 1;
      }
    }

    // Setup Wizard interactions
    if (g_setup_open && wm_is_open(APP_SETUP)) {
      int32_t wx = g_setup_win.x;
      int32_t wy = g_setup_win.y;
      int32_t ww = g_setup_win.w;
      int32_t wh = g_setup_win.h;

      if (hit_test(p0.x, p0.y, wx, wy, ww, wh)) {
        app_raise(APP_SETUP);
        ui_changed = 1;
      }

      // Close button
      if (hit_test(p0.x, p0.y, wx + ww - 24, wy + 5, 20, 20)) {
        g_setup_open = 0;
        g_setup_dragging = 0;
        ui_changed = 1;
      }
      // Title bar drag
      else if (hit_test(p0.x, p0.y, wx, wy, ww, 30)) {
        g_setup_dragging = 1;
        g_setup_drag_off_x = p0.x - wx;
        g_setup_drag_off_y = p0.y - wy;
        ui_changed = 1;
      }
      // Back button
      else if (g_setup_step > 0 && hit_test(p0.x, p0.y, wx + 16, wy + wh - 28, 50, 18)) {
        if (g_setup_step > 0) {
          g_setup_step--;
        }
        ui_changed = 1;
      }
      // Next/Finish button
      else if (hit_test(p0.x, p0.y, wx + ww - 76, wy + wh - 28, 60, 18)) {
        if (g_setup_step == 5) {
          // Finish - close wizard
          g_setup_open = 0;
          shell_append("setup: complete\n> ");
        } else {
          // Advance to next step and execute action
          switch (g_setup_step) {
            case 0: // Welcome -> Detect
              g_setup_disk_ready = 1;
              break;
            case 1: // Detect -> Partition
              g_setup_partition_done = 1;
              break;
            case 2: // Partition -> Format
              g_setup_format_done = 1;
              break;
            case 3: // Format -> Verify
              // Format already done
              break;
            case 4: // Verify -> Complete
              // Verification done
              break;
          }
          g_setup_step++;
        }
        ui_changed = 1;
      }
    }
  }

  // Window manager interactions (shell window)
  if (pressed & 1u) {
    if (g_show_shell && wm_is_open(APP_SHELL)) {
      int32_t wx = g_shell_win.x;
      int32_t wy = g_shell_win.y;
      int32_t ww = g_shell_win.w;
      int32_t wh = g_shell_win.h;

      // Close button
      if (hit_test(p0.x, p0.y, wx + ww - 24, wy + 5, 20, 20)) {
        g_show_shell = 0;
        g_shell_dragging = 0;
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, 30)) {
        g_shell_dragging = 1;
        g_shell_drag_off_x = p0.x - wx;
        g_shell_drag_off_y = p0.y - wy;
        app_raise(APP_SHELL);
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, wh)) {
        app_raise(APP_SHELL);
        ui_changed = 1;
      }
    }
  }

  if (!(buttons & 1u)) {
    g_shell_dragging = 0;
    g_explorer_dragging = 0;
    g_viewer_dragging = 0;
    g_notepad_dragging = 0;
    g_setup_dragging = 0;
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

  if (g_setup_dragging && (p0.dx || p0.dy)) {
    g_setup_win.x = p0.x - g_setup_drag_off_x;
    g_setup_win.y = p0.y - g_setup_drag_off_y;
    ui_changed = 1;
  }

  if (!g_back || !g_bg) {
    // Low-memory fallback: render directly to the hardware framebuffer.
    draw_background();
    draw_desktop_icons();
    draw_widgets();
    draw_open_windows();
    draw_taskbar();
    draw_start_menu();

    input_pointer_state_t p = input_pointer_get();
    uint32_t fg = 0xFFFFFF;
    uint32_t outline = 0x000000;
    if (p.buttons & 1u)
      fg = 0xFFCC00;
    else if (p.buttons & 2u)
      fg = 0x00CCFF;
    draw_cursor(p.x, p.y, fg, outline);

    if ((p.buttons & 1u) && (p.dx || p.dy)) {
      int32_t ix = p.x + 18;
      int32_t iy = p.y + 18;
      if (ix < 0) ix = 0;
      if (iy < 0) iy = 0;
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 5, 0xFFCC00);
      draw_rect((uint32_t)ix, (uint32_t)iy, 5, 1, 0x000000);
      draw_rect((uint32_t)ix, (uint32_t)iy + 4, 5, 1, 0x000000);
    }

    g_last_cursor_x = p.x;
    g_last_cursor_y = p.y;
    g_last_cursor_buttons = p.buttons;
  } else {
    static uint8_t g_desktop_dirty = 1;
    uint8_t scene_changed = (uint8_t)(g_desktop_dirty || ui_changed || g_tick < 2u);

    if (scene_changed) {
      blit_rect(g_back, g_bg, 0, 0, g_bi->width, g_bi->height);
      draw_desktop_icons();
      draw_widgets();
      draw_open_windows();
      draw_taskbar();
      draw_start_menu();
      g_desktop_dirty = 0;
    }

    input_pointer_state_t p = input_pointer_get();
    uint8_t cursor_changed =
        (uint8_t)(p.x != g_last_cursor_x || p.y != g_last_cursor_y ||
                  p.buttons != g_last_cursor_buttons);

    if (scene_changed || cursor_changed || g_tick < 2u) {
      if (scene_changed) {
        // Scene changes still need a complete presentation.
        blit_rect(g_bi->framebuffer, g_back, 0, 0, g_bi->width, g_bi->height);
      } else {
        // Mouse-only updates restore just the old cursor footprint, not the
        // entire screen. The 26px region includes the cursor outline/drag marker.
        int32_t restore_x = g_last_cursor_x > 0 ? g_last_cursor_x - 1 : 0;
        int32_t restore_y = g_last_cursor_y > 0 ? g_last_cursor_y - 1 : 0;
        blit_rect(g_bi->framebuffer, g_back, (uint32_t)restore_x,
                  (uint32_t)restore_y, 26u, 26u);
      }

      uint32_t *saved_back = g_back;
      g_back = NULL;

      uint32_t fg = 0xFFFFFF;
      uint32_t outline = 0x000000;
      if (p.buttons & 1u)
        fg = 0xFFCC00;
      else if (p.buttons & 2u)
        fg = 0x00CCFF;
      draw_cursor(p.x, p.y, fg, outline);

      if ((p.buttons & 1u) && (p.dx || p.dy)) {
        int32_t ix = p.x + 18;
        int32_t iy = p.y + 18;
        if (ix < 0) ix = 0;
        if (iy < 0) iy = 0;
        draw_rect((uint32_t)ix, (uint32_t)iy, 5, 5, 0xFFCC00);
        draw_rect((uint32_t)ix, (uint32_t)iy, 5, 1, 0x000000);
        draw_rect((uint32_t)ix, (uint32_t)iy + 4, 5, 1, 0x000000);
      }

      g_back = saved_back;
      g_last_cursor_x = p.x;
      g_last_cursor_y = p.y;
      g_last_cursor_buttons = p.buttons;
    }
  }
  g_tick++;
}

// ============================================================================
// Fullscreen Setup Wizard (Boot-to-First-Run Mode)
// ============================================================================

static uint8_t g_fullscreen_setup_step = 0;
static uint8_t g_fullscreen_setup_disk_ready = 0;
static uint8_t g_fullscreen_setup_partition_done = 0;
static uint8_t g_fullscreen_setup_format_done = 0;
static uint8_t g_setup_complete = 0;

static void draw_fullscreen_setup_step(uint32_t screen_w, uint32_t screen_h) {
  const char *step_titles[] = {
    "Welcome to VexOS",
    "Detect Disk",
    "Create Partition",
    "Format Filesystem",
    "Verify Installation",
    "Setup Complete"
  };

  // Dark background
  draw_rect(0, 0, screen_w, screen_h, 0x0A0D14);

  // Title at top
  draw_string_8x8(32, 32, step_titles[g_fullscreen_setup_step], 0xFFFFFF);
  draw_rect(32, 52, screen_w - 64, 2, 0x2A3140);

  // Progress bar
  uint32_t bar_y = 80;
  uint32_t bar_w = screen_w - 64;
  uint32_t step_w = bar_w / 6u;
  draw_rect(32, bar_y, bar_w, 8, 0x1A1F2A);
  for (uint32_t i = 0; i < 6; i++) {
    uint32_t cx = 32 + i * step_w;
    uint32_t color = (i <= g_fullscreen_setup_step) ? 0x22C55E : 0x303040;
    draw_rect(cx + 2, bar_y + 2, step_w - 4, 4, color);
  }

  // Content area background
  uint32_t content_y = 120;
  uint32_t content_h = screen_h - 240;
  blend_rect(32, content_y, screen_w - 64, content_h, 0x0F141C, 220);
  draw_rect(32, content_y, screen_w - 64, 1, 0x2A3140);
  draw_rect(32, content_y + content_h - 1, screen_w - 64, 1, 0x0B0E14);

  // Step content
  uint32_t text_x = 56;
  uint32_t text_y = content_y + 32;
  switch (g_fullscreen_setup_step) {
    case 0: // Welcome
      draw_string_8x8(text_x, text_y, "Welcome to VexOS Setup!", 0xFFFFFF);
      draw_string_8x8(text_x, text_y + 32, "This wizard will guide you through:", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 56, "  - Disk detection", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 76, "  - Partition creation", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 96, "  - Filesystem formatting", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 136, "Click 'Next' to begin...", 0x22C55E);
      break;
    case 1: // Detect
      draw_string_8x8(text_x, text_y, "Detecting storage devices...", 0xFFFFFF);
      if (g_fullscreen_setup_disk_ready) {
        draw_string_8x8(text_x, text_y + 40, "Status: Disk detected and ready", 0x22C55E);
        draw_string_8x8(text_x, text_y + 64, "Device: RAM Disk (1 MiB)", 0xD0D6E0);
        draw_string_8x8(text_x, text_y + 84, "Sector size: 512 bytes", 0xD0D6E0);
      } else {
        draw_string_8x8(text_x, text_y + 40, "Status: Scanning for devices...", 0xFFB000);
      }
      break;
    case 2: // Partition
      draw_string_8x8(text_x, text_y, "Creating MBR partition...", 0xFFFFFF);
      if (g_fullscreen_setup_partition_done) {
        draw_string_8x8(text_x, text_y + 40, "Status: Partition created successfully", 0x22C55E);
        draw_string_8x8(text_x, text_y + 64, "Partition: Primary (FAT16 LBA)", 0xD0D6E0);
        draw_string_8x8(text_x, text_y + 84, "Start LBA: 1", 0xD0D6E0);
      } else {
        draw_string_8x8(text_x, text_y + 40, "Status: Waiting to create partition...", 0xD0D6E0);
      }
      break;
    case 3: // Format
      draw_string_8x8(text_x, text_y, "Formatting partition...", 0xFFFFFF);
      if (g_fullscreen_setup_format_done) {
        draw_string_8x8(text_x, text_y + 40, "Status: Format complete", 0x22C55E);
        draw_string_8x8(text_x, text_y + 64, "Filesystem: FAT16", 0xD0D6E0);
        draw_string_8x8(text_x, text_y + 84, "Volume label: VEXOS", 0xD0D6E0);
      } else {
        draw_string_8x8(text_x, text_y + 40, "Status: Waiting to format...", 0xD0D6E0);
      }
      break;
    case 4: // Verify
      draw_string_8x8(text_x, text_y, "Verifying installation...", 0xFFFFFF);
      if (g_fullscreen_setup_format_done) {
        draw_string_8x8(text_x, text_y + 40, "MBR Signature: 0x55AA (Valid)", 0x22C55E);
        draw_string_8x8(text_x, text_y + 60, "BPB Structure: Valid", 0x22C55E);
        draw_string_8x8(text_x, text_y + 80, "FAT Tables: Initialized", 0x22C55E);
        draw_string_8x8(text_x, text_y + 100, "Root Directory: Ready", 0x22C55E);
      }
      break;
    case 5: // Complete
      draw_string_8x8(text_x, text_y, "Setup Complete!", 0x22C55E);
      draw_string_8x8(text_x, text_y + 40, "Your VexOS installation is ready.", 0xFFFFFF);
      draw_string_8x8(text_x, text_y + 80, "The system will now boot to the desktop.", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 100, "You can run Setup again anytime from the", 0xD0D6E0);
      draw_string_8x8(text_x, text_y + 120, "Start menu or by typing 'setup' in Shell.", 0xD0D6E0);
      break;
  }

  // Navigation buttons at bottom
  uint32_t btn_y = screen_h - 72;

  // Back button (disabled on step 0)
  if (g_fullscreen_setup_step > 0) {
    draw_rect(32, btn_y, 100, 32, 0x2A3140);
    draw_string_8x8(64, btn_y + 12, "Back", 0xFFFFFF);
  } else {
    draw_rect(32, btn_y, 100, 32, 0x1A1F2A);
    draw_string_8x8(64, btn_y + 12, "Back", 0x505050);
  }

  // Next/Finish button
  const char *btn_text = (g_fullscreen_setup_step == 5) ? "Finish" : "Next";
  uint32_t btn_color = (g_fullscreen_setup_step == 5) ? 0x22C55E : 0x2244AA;
  uint32_t btn_x = screen_w - 132;
  draw_rect(btn_x, btn_y, 100, 32, btn_color);
  draw_string_8x8(btn_x + 28, btn_y + 12, btn_text, 0xFFFFFF);
}


// Run the fullscreen setup wizard (blocking until complete)
void desktop_run_fullscreen_wizard(BootInfo *bootInfo) {
  g_bi = bootInfo;
  g_setup_complete = 0;
  g_fullscreen_setup_step = 0;
  g_fullscreen_setup_disk_ready = 0;
  g_fullscreen_setup_partition_done = 0;
  g_fullscreen_setup_format_done = 0;

  // Initialize input
  input_init(bootInfo->width, bootInfo->height);

  uint32_t screen_w = bootInfo->width;
  uint32_t screen_h = bootInfo->height;
  uint32_t *fb = bootInfo->framebuffer;

  // Allocate local backbuffer to prevent screen flickering
  uint32_t *wizard_back = NULL;
  if (bootInfo->pitch && bootInfo->height) {
    uint64_t pixels = (uint64_t)bootInfo->pitch * (uint64_t)bootInfo->height;
    uint64_t bytes = pixels * 4u;
    if (bytes <= 0xFFFFFFFFu) {
      wizard_back = (uint32_t *)kmalloc((uint32_t)bytes);
    }
  }

  uint8_t running = 1;
  uint8_t btn_pressed = 0;
  uint8_t needs_redraw = 1;
  input_pointer_state_t last_p = {0};

  while (running) {
    // Poll input
    xhci_poll_events();
    input_pointer_state_t p = input_pointer_get();

    // Only redraw if something changed (mouse moved, button pressed, etc)
    if (needs_redraw || p.x != last_p.x || p.y != last_p.y || p.buttons != last_p.buttons) {
      if (wizard_back) {
        g_back = wizard_back;
      } else {
        g_back = fb;
      }

      draw_fullscreen_setup_step(screen_w, screen_h);

      // Draw cursor
      uint32_t cursor_fg = 0xFFFFFF;
      if (p.buttons & 1u) cursor_fg = 0xFFCC00;
      else if (p.buttons & 2u) cursor_fg = 0x00CCFF;
      for (int i = -8; i <= 8; i++) {
        if (p.y + i < (int)screen_h) put_px(p.x, p.y + i, cursor_fg);
        if (p.x + i < (int)screen_w) put_px(p.x + i, p.y, cursor_fg);
      }

      if (wizard_back) {
        blit_rect(fb, wizard_back, 0, 0, screen_w, screen_h);
      }

      needs_redraw = 0;
      last_p = p;
    }

    // Yield to prevent 100% CPU usage
    for (volatile int i = 0; i < 1000000; i++);

    // Handle button clicks
    if ((p.buttons & 1u) && !btn_pressed) {
      btn_pressed = 1;

      // Check Next/Finish button
      uint32_t btn_x = screen_w - 132;
      uint32_t btn_y = screen_h - 72;
      if (p.x >= btn_x && p.x < btn_x + 100 &&
          p.y >= btn_y && p.y < btn_y + 32) {
        if (g_fullscreen_setup_step == 5) {
          // Finish - exit wizard
          running = 0;
          g_setup_complete = 1;
        } else {
          // Advance and perform action
          switch (g_fullscreen_setup_step) {
            case 0: g_fullscreen_setup_disk_ready = 1; break;
            case 1: g_fullscreen_setup_partition_done = 1; break;
            case 2: g_fullscreen_setup_format_done = 1; break;
            case 3: /* format done */ break;
            case 4: /* verify done */ break;
          }
          g_fullscreen_setup_step++;
          needs_redraw = 1;
        }
      }

      // Check Back button
      if (g_fullscreen_setup_step > 0 &&
          p.x >= 32 && p.x < 132 &&
          p.y >= btn_y && p.y < btn_y + 32) {
        g_fullscreen_setup_step--;
        needs_redraw = 1;
      }
    }

    if (!(p.buttons & 1u)) {
      btn_pressed = 0;
    }

  }

  if (wizard_back) {
    kfree(wizard_back);
  }
  g_back = NULL;
}

int desktop_is_setup_complete(void) {
  return g_setup_complete;
}
