#include "../../include/desktop.h"
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
#include "../desktop/window_manager.h"
#include "../desktop/window_chrome.h"
#include "../desktop/apps/files/file_manager.h"

static BootInfo *g_bi;
static uint32_t g_tick;
static uint32_t *g_back;
static uint32_t *g_bg;

extern uint8_t _binary_CGA_F08_start[];
extern uint8_t _binary_CGA_F08_end[];
extern const uint8_t _binary_desktop_icons_bin_start[];

static char g_text[1024];
static uint32_t g_text_len;

static uint8_t g_start_open;
static uint8_t g_show_shell;


static input_pointer_state_t g_last_ptr;
static uint32_t g_rand_state;

static uint32_t shell_next_rand(void) {
  g_rand_state = (g_rand_state * 1664525u) + 1013904223u;
  return g_rand_state;
}

static desktop_window_t g_viewer_win;
static uint8_t g_viewer_open;
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

static uint32_t g_viewer_scroll;

#include "../desktop/window_manager.h"
#include "../desktop/app_registry.h"
#include "../desktop/graphics/graphics.h"
#include "../desktop/compositor/compositor.h"
#include "../desktop/compositor/window_compositor.h"
#include "../desktop/apps/notepad/notepad.h"
#include "../desktop/apps/terminal/terminal.h"
#include "../desktop/apps/terminal/shell_commands.h"
#include "../desktop/apps/doom/doom_window.h"

static uint8_t g_app_open[APP_COUNT];

/* A maximized Doom window occupies the complete display, including the dock
   region. The taskbar must stay behind it until the game leaves maximized mode. */
static int desktop_doom_fullscreen_active(void) {
  return g_app_open[APP_DOOM] && doom_window_is_maximized();
}

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
  g_app_open[APP_VIEWER] = 1;
  wm_raise(APP_VIEWER);
  return 1;
}

// Setup Wizard state
static desktop_window_t g_setup_win;
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
extern void serial_print(const char *str);
#ifdef VEXOS_CI_INPUT_TEST
static uint8_t g_ci_keyboard_input_seen;
static uint8_t g_ci_mouse_move_seen;
static uint8_t g_ci_start_click_seen;
#endif

static uint8_t g_ring3_task_started;
static volatile uint8_t g_doom_window_dirty;
static uint8_t g_doom_click_handled;
static uint8_t g_doom_mouse_captured;
static uint8_t g_doom_tick_reported;
static uint8_t g_doom_draw_reported;
static uint8_t g_doom_frame_reported;
static uint8_t g_doom_render_trace_done;
static uint8_t g_doom_close_pointer_reported;
static void *g_ring3_task_stack;
static void *g_deferred_ring3_task_stack;
static const char *g_ring3_elf_name = "doom.elf";

int desktop_doom_app_ready(void) {
  if (!doom_window_open()) {
    serial_print("[DOOMWIN] Window buffer allocation failed\n");
    return 0;
  }
  /* Shell commands are also recorded in the raw HID queue. Do not replay
     those keystrokes into Doom's title/menu screen when Ring 3 starts. */
  uint32_t stale_keys = input_kbd_clear_events();
  if (stale_keys) {
    serial_print("[DOOMWIN] Cleared stale keyboard events before launch\\n");
  }
  int32_t discard_x = 0, discard_y = 0;
  input_pointer_take_motion(&discard_x, &discard_y);
  g_doom_mouse_captured = 0;
  g_doom_tick_reported = 0;
  g_doom_draw_reported = 0;
  g_doom_frame_reported = 0;
  g_doom_render_trace_done = 0;
  g_doom_close_pointer_reported = 0;
  g_app_open[APP_DOOM] = 1;
  g_start_open = 0;
  wm_raise(APP_DOOM);
  g_doom_window_dirty = 1;
#ifdef VEXOS_CI_INPUT_TEST
  if (g_ring3_task_started)
    serial_print("[DOOMWIN] Ring 3 Doom window opened and focused\n");
  else
    serial_print("[CI] Preflight Doom surface opened (not a game launch)\n");
#else
  serial_print("[DOOMWIN] Window opened and focused\n");
#endif
  return 1;
}

#ifdef VEXOS_CI_INPUT_TEST
/* The syscall self-test paints a known 1x1 pixel to verify the user buffer.
   Tear down that temporary surface before the real Doom app is launched. */
void desktop_doom_ci_test_cleanup(void) {
  doom_window_app_exited();
  g_app_open[APP_DOOM] = 0;
  g_doom_mouse_captured = 0;
  g_doom_tick_reported = 0;
  g_doom_draw_reported = 0;
  g_doom_frame_reported = 0;
  g_doom_render_trace_done = 0;
  g_doom_window_dirty = 1;
  g_doom_click_handled = 0;
}
#endif

int desktop_doom_input_active(void) { return g_app_open[APP_DOOM] && wm_get_focus()==APP_DOOM; }
int desktop_doom_mouse_input_active(void) {
  if(!desktop_doom_input_active()){g_doom_mouse_captured=0;return 0;}
  input_pointer_state_t p=input_pointer_get();
  uint32_t bar_h = desktop_doom_fullscreen_active() ? 0u : 56u;
  if(g_bi&&bar_h>g_bi->height)bar_h=g_bi->height;
  if(g_bi&&bar_h&&p.y>=(int32_t)(g_bi->height-bar_h))return 0;
  if(g_doom_mouse_captured)return 1;
  return doom_window_pointer_in_content(p.x,p.y);
}
int desktop_doom_mouse_buttons_active(void) {
  if(!desktop_doom_input_active())return 0;
  input_pointer_state_t p=input_pointer_get();
  uint32_t bar_h = desktop_doom_fullscreen_active() ? 0u : 56u;
  if(g_bi&&bar_h>g_bi->height)bar_h=g_bi->height;
  if(g_bi&&bar_h&&p.y>=(int32_t)(g_bi->height-bar_h))return 0;
  return doom_window_pointer_in_content(p.x,p.y);
}
int desktop_doom_close_requested(void) { return doom_window_close_requested(); }

/* Called from the kernel after a ring-3 process exits or faults. */
void desktop_user_app_exited(int faulted) {
  g_ring3_task_started = 0;
  g_app_open[APP_DOOM]=0;g_doom_mouse_captured=0;
  g_doom_tick_reported=0;g_doom_draw_reported=0;g_doom_frame_reported=0;
  g_doom_render_trace_done=0;
  doom_window_app_exited();
  g_doom_window_dirty=1;g_doom_click_handled=0;g_show_shell=1;g_app_open[APP_SHELL]=1;wm_raise(APP_SHELL);
  if (g_ring3_task_stack) {
    g_deferred_ring3_task_stack = g_ring3_task_stack;
    g_ring3_task_stack = NULL;
  }
  if (faulted)
    shell_append("ring3: application crashed; returned to desktop\n> ");
  else
    shell_append("ring3: application exited\n> ");
}

static uint32_t shell_u32_to_dec(char out[12], uint32_t v);
static int streq_n(const char *a, const char *b, uint32_t n);

#define DESKTOP_ICON_WIDTH 32u
#define DESKTOP_ICON_HEIGHT 32u
#define DESKTOP_ICON_BYTES (DESKTOP_ICON_WIDTH * DESKTOP_ICON_HEIGHT * 4u)

static const uint8_t *desktop_icon_pixels(app_id_t app) {
  uint32_t icon_index;
  switch (app) {
    case APP_EXPLORER: icon_index = 0u; break;
    case APP_SHELL:    icon_index = 1u; break;
    case APP_SETUP:    icon_index = 2u; break;
    case APP_NOTEPAD:  icon_index = 3u; break;
    case APP_VIEWER:   icon_index = 4u; break;
    case APP_DOOM:     icon_index = 5u; break;
    default: return NULL;
  }

  return _binary_desktop_icons_bin_start +
         (uint64_t)icon_index * (uint64_t)DESKTOP_ICON_BYTES;
}

static void draw_app_icon(uint32_t x, uint32_t y, uint32_t size,
                          app_id_t app) {
  const uint8_t *pixels = desktop_icon_pixels(app);
  if (!pixels || !size)
    return;

  gfx_blit_rgba_scaled(pixels, DESKTOP_ICON_WIDTH, DESKTOP_ICON_HEIGHT,
                       DESKTOP_ICON_WIDTH, x, y, size, size);
}

static void ring3_task_entry(void) {
  usermode_launch(g_ring3_elf_name);

  // On success usermode_launch transfers to ring 3 and never returns. A
  // return means the loader rejected the image or failed before the switch.
  shell_append("ring3: loader returned; see serial log\n");
  for (;;) {
    __asm__ volatile("sti; hlt");
  }
}

static void launch_ring3_task(const char *filename) {
  if (g_ring3_task_started)
    return;
  if (!filename || !filename[0])
    filename = "doom.elf";
  g_ring3_elf_name = filename;
  g_ring3_task_started = 1;

  if (!simplefs_init()) {
    shell_append("ring3: storage mount failed\n");
    g_ring3_task_started = 0;
    return;
  }

  uint32_t elf_size = 0;
  if (!simplefs_file_size(filename, &elf_size) || elf_size == 0) {
    shell_append("ring3: executable not found: ");
    shell_append(filename);
    shell_append("\n");
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
  g_ring3_task_stack = stack;
  Task_Create(ring3_task_entry, stack_top);
}

static void draw_window_chrome(const desktop_window_t *win,
                               const char *title, const char *sub_title,
                               uint8_t focused, uint8_t has_scroll,
                               uint8_t has_save) {
  window_chrome_draw(win, title, sub_title, focused, has_scroll, has_save,
                     draw_app_icon);
}

static void draw_notepad_window(const desktop_window_t *win) {
  notepad_render_context_t context = {
    .window = win,
    .filename = notepad_filename(),
    .text = notepad_text(),
    .text_length = notepad_text_length(),
    .scroll = notepad_scroll(),
    .focused = (uint8_t)(wm_get_focus() == APP_NOTEPAD),
    .draw_chrome = draw_window_chrome
  };
  notepad_draw(&context);
}

static void draw_setup_window(const desktop_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;

  uint8_t is_focused = (wm_get_focus() == APP_SETUP);
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
  gfx_draw_rect((uint32_t)x + 16u, bar_y, bar_w, 4, 0x2A3140);
  for (uint32_t i = 0; i < 6; i++) {
    uint32_t cx = (uint32_t)x + 16u + i * step_w;
    uint32_t color = (i <= g_setup_step) ? 0x22C55E : 0x404040;
    gfx_draw_rect(cx, bar_y, step_w - 2, 4, color);
    // Step number
    char num[2] = {(char)('1' + i), 0};
    gfx_draw_string(cx + (step_w/2) - 4, bar_y + 8, num, color);
  }

  // Content area
  uint32_t content_y = (uint32_t)y + 72u;
  uint32_t content_h = (uint32_t)h - 108u;
  gfx_draw_rect((uint32_t)x + 16u, content_y, (uint32_t)w - 32u, content_h, 0x14141A);

  // Step title
  gfx_draw_string((uint32_t)x + 24u, content_y + 12u, step_titles[g_setup_step], 0xE0E0E0);
  gfx_draw_rect((uint32_t)x + 24u, content_y + 28u, (uint32_t)w - 48u, 1, 0x2A3140);

  // Step content
  uint32_t text_y = content_y + 44u;
  switch (g_setup_step) {
    case 0: // Welcome
      gfx_draw_string((uint32_t)x + 24u, text_y, "Welcome to VexOS Setup!", 0xFFFFFF);
      gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "This wizard will help you:", 0xD0D6E0);
      gfx_draw_string((uint32_t)x + 24u, text_y + 36u, "- Detect your disk", 0xD0D6E0);
      gfx_draw_string((uint32_t)x + 24u, text_y + 52u, "- Create partitions", 0xD0D6E0);
      gfx_draw_string((uint32_t)x + 24u, text_y + 68u, "- Format with FAT16", 0xD0D6E0);
      break;
    case 1: // Detect
      gfx_draw_string((uint32_t)x + 24u, text_y, "Detecting disk...", 0xFFFFFF);
      if (g_setup_disk_ready) {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Disk ready", 0x22C55E);
        gfx_draw_string((uint32_t)x + 24u, text_y + 40u, "Type: RAM Disk (1 MiB)", 0xD0D6E0);
      } else {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Not initialized", 0xFFB000);
      }
      break;
    case 2: // Partition
      gfx_draw_string((uint32_t)x + 24u, text_y, "Creating MBR partition...", 0xFFFFFF);
      if (g_setup_partition_done) {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Partition created", 0x22C55E);
        gfx_draw_string((uint32_t)x + 24u, text_y + 40u, "Type: FAT16 LBA", 0xD0D6E0);
      } else {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Pending", 0xD0D6E0);
      }
      break;
    case 3: // Format
      gfx_draw_string((uint32_t)x + 24u, text_y, "Formatting partition...", 0xFFFFFF);
      if (g_setup_format_done) {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Formatted", 0x22C55E);
        gfx_draw_string((uint32_t)x + 24u, text_y + 40u, "FS: FAT16", 0xD0D6E0);
      } else {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "Status: Pending", 0xD0D6E0);
      }
      break;
    case 4: // Verify
      gfx_draw_string((uint32_t)x + 24u, text_y, "Verifying installation...", 0xFFFFFF);
      if (g_setup_format_done) {
        gfx_draw_string((uint32_t)x + 24u, text_y + 20u, "MBR: Valid", 0x22C55E);
        gfx_draw_string((uint32_t)x + 24u, text_y + 36u, "BPB: Valid", 0x22C55E);
        gfx_draw_string((uint32_t)x + 24u, text_y + 52u, "FAT: Initialized", 0x22C55E);
      }
      break;
    case 5: // Complete
      gfx_draw_string((uint32_t)x + 24u, text_y, "Setup Complete!", 0x22C55E);
      gfx_draw_string((uint32_t)x + 24u, text_y + 24u, "Your disk is ready to use.", 0xFFFFFF);
      gfx_draw_string((uint32_t)x + 24u, text_y + 48u, "You can now use the", 0xD0D6E0);
      gfx_draw_string((uint32_t)x + 24u, text_y + 64u, "Explorer and Notepad apps.", 0xD0D6E0);
      break;
  }

  // Navigation buttons at bottom
  uint32_t btn_y = (uint32_t)y + (uint32_t)h - 28u;

  // Back button (disabled on step 0)
  if (g_setup_step > 0) {
    gfx_draw_rect((uint32_t)x + 16u, btn_y, 50, 18, 0x2A3140);
    gfx_draw_string((uint32_t)x + 26u, btn_y + 5u, "Back", 0xFFFFFF);
  } else {
    gfx_draw_rect((uint32_t)x + 16u, btn_y, 50, 18, 0x1A1A20);
    gfx_draw_string((uint32_t)x + 26u, btn_y + 5u, "Back", 0x606060);
  }

  // Next/Finish button
  uint32_t btn_x = (uint32_t)x + (uint32_t)w - 76u;
  const char *btn_label = (g_setup_step == 5) ? "Finish" : "Next";
  gfx_draw_rect(btn_x, btn_y, 60, 18, 0x2244AA);
  gfx_draw_string(btn_x + 14u, btn_y + 5u, btn_label, 0xFFFFFF);
}

static void draw_viewer_window(const desktop_window_t *win) {
  if (!g_bi || !win)
    return;

  int32_t x = win->x;
  int32_t y = win->y;
  int32_t w = win->w;
  int32_t h = win->h;
  draw_window_chrome(win, "Viewer", g_viewer_name[0] ? g_viewer_name : "Preview",
                     (uint8_t)(wm_get_focus() == APP_VIEWER), 1, 0);

  gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 31),
            (uint32_t)(w - 2), 30u, 0x17263A);
  const char *toolbar_label =
      g_viewer_kind == VIEWER_CONTENT_PNG ? "IMAGE PREVIEW / PNG" :
      g_viewer_kind == VIEWER_CONTENT_JPEG ? "IMAGE PREVIEW / JPEG" :
      g_viewer_kind == VIEWER_CONTENT_BINARY ? "PREVIEW UNAVAILABLE" :
      "PREVIEW / TEXT CONTENT";
  gfx_draw_string((uint32_t)(x + 14), (uint32_t)(y + 42),
                  toolbar_label, 0xAFC2D6u);

  uint32_t px = (uint32_t)x + 12u;
  uint32_t py = (uint32_t)y + 68u;
  uint32_t pw = w > 24 ? (uint32_t)w - 24u : 1u;
  uint32_t ph = h > 80 ? (uint32_t)h - 80u : 1u;

  if ((g_viewer_kind == VIEWER_CONTENT_PNG ||
       g_viewer_kind == VIEWER_CONTENT_JPEG) &&
      g_viewer_pixels && g_viewer_image_width && g_viewer_image_height) {
    gfx_draw_rect(px, py, pw, ph, 0x182230);

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
        gfx_put_pixel(image_x + ix, image_y + iy,
               (out_r << 16) | (out_g << 8) | out_b);
      }
    }
    return;
  }

  gfx_blend_round_rect(px, py, pw, ph, 5u, 0xF4F7FBu, 255);
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
      gfx_draw_string(tx + cx * 8u, ty + cy * 16u, t, 0x26384Du);
    }
    cx++;
  }
}
static int32_t g_last_cursor_x;
static int32_t g_last_cursor_y;
static uint32_t g_last_cursor_buttons;

/* A tiny save-under buffer lets the no-backbuffer path update only the cursor
   instead of repainting the entire desktop on every idle frame. */
#define DESKTOP_CURSOR_SAVE_SIZE 26u
static uint32_t g_fallback_cursor_save[DESKTOP_CURSOR_SAVE_SIZE *
                                       DESKTOP_CURSOR_SAVE_SIZE];
static int32_t g_fallback_cursor_x;
static int32_t g_fallback_cursor_y;
static uint32_t g_fallback_cursor_width;
static uint32_t g_fallback_cursor_height;
static uint8_t g_fallback_cursor_valid;
static uint8_t g_fallback_scene_dirty = 1;

static void draw_background(void);
static void draw_text_panel(void);
static void draw_taskbar(void);
static void draw_start_menu(void);
static void draw_shell_window(const desktop_window_t *win);
static void draw_viewer_window(const desktop_window_t *win);
static void draw_notepad_window(const desktop_window_t *win);
static void draw_setup_window(const desktop_window_t *win);
static void draw_window_chrome(const desktop_window_t *win, const char *title, const char *sub_title, uint8_t is_focused, uint8_t has_scroll, uint8_t has_save);
static void draw_desktop_icons(void);



int desktop_draw_frame(const uint32_t *src, uint32_t w, uint32_t h) {
  int accepted = doom_window_draw_frame(src, w, h);
  if (accepted) {
    g_doom_window_dirty = 1;
    if (!g_doom_frame_reported) {
      g_doom_frame_reported = 1;
#ifdef VEXOS_CI_INPUT_TEST
      if (g_ring3_task_started)
        serial_print("[DOOMWIN] First frame rendered by Ring 3 Doom\n");
      else
        serial_print("[CI] Preflight framebuffer draw passed\n");
#else
      serial_print("[DOOMWIN] First frame received from Doom\n");
#endif
    }
  }
  return accepted;
}

static void draw_doom_window_if_open(void) {
  if (!g_doom_draw_reported && g_app_open[APP_DOOM]) {
    g_doom_draw_reported = 1;
    serial_print("[DOOMWIN] Compositor is drawing the Doom window\n");
  }
  doom_window_draw();
}

/* A maximized, focused Doom window covers the desktop except for the taskbar.
   Redraw only its visible content rectangle while new game frames arrive. */
static void draw_doom_window_visible_content(uint32_t visible_height) {
  if (!g_bi || g_bi->width <= 2u || !visible_height)
    return;
  gfx_reset_clip();
  if (gfx_push_clip(1u, 31u, g_bi->width - 2u, visible_height)) {
    draw_doom_window_if_open();
    gfx_pop_clip();
  }
  gfx_reset_clip();
}

static void draw_shell_window(const desktop_window_t *win){terminal_render_context_t ctx={.window=win,.tick=g_tick,.focused=(uint8_t)(wm_get_focus()==APP_SHELL),.draw_chrome=draw_window_chrome};terminal_draw(&ctx);}

static uint8_t desktop_rtc_read(uint8_t reg) {
  __asm__ volatile("outb %0, %1" : : "a"((uint8_t)(0x80u | reg)), "Nd"((uint16_t)0x70));
  uint8_t v;__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)0x71));
  __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x70));return v;
}
static uint8_t desktop_bcd(uint8_t v){return (uint8_t)(((v>>4)*10u)+(v&15u));}
static uint8_t desktop_clock(uint8_t*h,uint8_t*m,uint8_t*sec,uint8_t*d,uint8_t*mo,uint16_t*y){
  uint8_t s1=desktop_rtc_read(0),m1=desktop_rtc_read(2),h1=desktop_rtc_read(4),d1=desktop_rtc_read(7),o1=desktop_rtc_read(8),y1=desktop_rtc_read(9),c1=desktop_rtc_read(0x32);
  uint8_t s2=desktop_rtc_read(0),m2=desktop_rtc_read(2),h2=desktop_rtc_read(4),d2=desktop_rtc_read(7),o2=desktop_rtc_read(8),y2=desktop_rtc_read(9),c2=desktop_rtc_read(0x32);
  if((desktop_rtc_read(0x0A)&128u)||s1!=s2||m1!=m2||h1!=h2||d1!=d2||o1!=o2||y1!=y2||c1!=c2)return 0;
  uint8_t st=desktop_rtc_read(0x0B),pm=h2&128u;h2&=127u;
  if(!(st&4u)){s2=desktop_bcd(s2);m2=desktop_bcd(m2);h2=desktop_bcd(h2);d2=desktop_bcd(d2);o2=desktop_bcd(o2);y2=desktop_bcd(y2);c2=desktop_bcd(c2);}
  if(!(st&2u)){h2%=12u;if(pm)h2+=12u;}
  *h=h2;*m=m2;*sec=s2;*d=d2;*mo=o2;*y=(c2>=19u&&c2<=99u)?(uint16_t)(c2*100u+y2):(uint16_t)(2000u+y2);
  return (uint8_t)(h2<24u&&m2<60u&&d2>0u&&d2<=31u&&o2>0u&&o2<=12u);
}
static void draw_taskbar(void) {
  if (!g_bi)
    return;

  uint32_t bar_h = 56u;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;
  uint32_t y = g_bi->height - bar_h;
  uint32_t inner_y = y + 6u;
  uint32_t inner_h = bar_h > 12u ? bar_h - 12u : bar_h;

  gfx_blend_rect(0u, y, g_bi->width, bar_h, 0x090D18u, 248u);
  gfx_draw_rect(0u, y, g_bi->width, 1u, 0x3B4262u);
  gfx_draw_rect(0u, y + 1u, g_bi->width, 1u, 0x171D30u);

  /* Launcher button */
  gfx_blend_round_rect(8u, inner_y, 48u, inner_h, 10u,
                       g_start_open ? 0x342E5Du : 0x171D30u, 255u);
  gfx_draw_rect(8u, inner_y + 7u, 3u,
                inner_h > 14u ? inner_h - 14u : 1u, 0x927FFFu);
  gfx_draw_string(27u, inner_y + 17u, "V", 0xF4F2FFu);

  /* Centered application dock */
  uint32_t dock_w = 320u;
  if (dock_w + 32u > g_bi->width)
    dock_w = g_bi->width > 32u ? g_bi->width - 32u : 1u;
  uint32_t dock_x = (g_bi->width - dock_w) / 2u;
  gfx_blend_round_rect(dock_x, inner_y, dock_w, inner_h, 12u,
                       0x13192Au, 250u);
  gfx_draw_rect(dock_x + 2u, inner_y, dock_w - 4u, 1u, 0x39415Fu);
  gfx_draw_rect(dock_x + 14u, inner_y + 2u, dock_w - 28u, 1u, 0x202A40u);

  const app_id_t apps[5] = {
    APP_SHELL, APP_EXPLORER, APP_VIEWER, APP_NOTEPAD, APP_DOOM
  };
  uint32_t icon_x = dock_x + 16u;
  uint32_t icon_y = inner_y + (inner_h > 40u ? (inner_h - 40u) / 2u : 0u);
  for (uint32_t i = 0; i < 5u; ++i) {
    if (i < 4u || g_ring3_task_started) {
      draw_app_icon(icon_x, icon_y, 40u, apps[i]);
      if (g_app_open[apps[i]]) {
        uint32_t active = wm_get_focus() == apps[i] ? 0x9A8BFFu : 0x52647Fu;
        gfx_blend_round_rect(icon_x + 13u, icon_y + 40u, 14u, 3u,
                             2u, active, 255u);
      }
    }
    icon_x += 50u;
  }

  /* System clock and date */
  uint32_t clock_x = g_bi->width > 120u ? g_bi->width - 120u : 0u;
  gfx_blend_round_rect(clock_x, inner_y, 112u, inner_h, 9u,
                       0x13192Au, 250u);
  gfx_blend_round_rect(clock_x + 10u, inner_y + 10u, 5u, 5u,
                       2u, 0x67DDB5u, 255u);

  uint8_t hour, minute, second, day, month;
  uint16_t year;
  if (desktop_clock(&hour, &minute, &second, &day, &month, &year)) {
    char time_text[6] = {
      (char)('0' + hour / 10u), (char)('0' + hour % 10u), ':',
      (char)('0' + minute / 10u), (char)('0' + minute % 10u), 0
    };
    char date_text[11] = {
      (char)('0' + month / 10u), (char)('0' + month % 10u), '/',
      (char)('0' + day / 10u), (char)('0' + day % 10u), '/',
      (char)('0' + year / 1000u), (char)('0' + (year / 100u) % 10u),
      (char)('0' + (year / 10u) % 10u), (char)('0' + year % 10u), 0
    };
    gfx_draw_string(clock_x + 23u, inner_y + 8u, time_text, 0xF1F2FCu);
    gfx_draw_string(clock_x + 23u, inner_y + 29u, date_text, 0x8994AFu);
  } else {
    gfx_draw_string(clock_x + 23u, inner_y + 8u, "--:--", 0xF1F2FCu);
    gfx_draw_string(clock_x + 23u, inner_y + 29u, "--/--/----", 0x8994AFu);
  }
}
static void draw_start_menu(void) {
  if (!g_bi || !g_start_open)
    return;

  uint32_t bar_h = 56u;
  if (bar_h > g_bi->height)
    bar_h = g_bi->height;
  uint32_t bar_y = g_bi->height - bar_h;
  uint32_t x = 10u, width = 280u, height = 168u;
  uint32_t y = bar_y > height + 8u ? bar_y - height - 8u : 8u;

  gfx_blend_round_rect(x + 4u, y + 5u, width, height, 12u, 0x03050Cu, 170u);
  gfx_blend_round_rect(x, y, width, height, 12u, 0x11182Au, 252u);
  gfx_draw_rect(x + 2u, y + 1u, width - 4u, 2u, 0x8D7BFFu);
  gfx_draw_rect(x + 1u, y + height - 1u, width - 2u, 1u, 0x35405Du);
  gfx_draw_string(x + 16u, y + 9u, "VEXOS", 0xF4F2FFu);
  gfx_draw_string(x + 93u, y + 9u, "LAUNCHER", 0x9BA6C1u);
  gfx_draw_string(x + 214u, y + 9u, "2.0", 0xA99BFFu);
  gfx_draw_rect(x + 14u, y + 22u, width - 28u, 1u, 0x2C3651u);

  /* Identity rail */
  gfx_blend_round_rect(x + 10u, y + 29u, 54u, 128u, 8u, 0x222044u, 255u);
  gfx_draw_rect(x + 11u, y + 30u, 2u, 126u, 0x8D7BFFu);
  gfx_blend_round_rect(x + 23u, y + 48u, 28u, 28u, 8u, 0x51458Du, 255u);
  gfx_draw_string(x + 33u, y + 58u, "V", 0xFFFFFFu);
  gfx_draw_string(x + 20u, y + 91u, "VEX", 0xF1F0FFu);
  gfx_draw_string(x + 20u, y + 108u, "OS 2.0", 0xA99BFFu);
  gfx_draw_string(x + 20u, y + 137u, "LOCAL", 0x7F8BA8u);

  const app_id_t apps[6] = {
    APP_SHELL, APP_EXPLORER, APP_NOTEPAD, APP_VIEWER, APP_SETUP, APP_DOOM
  };
  for (uint32_t i = 0; i < 6u; ++i) {
    uint32_t row_y = y + 28u + i * 22u;
    uint32_t row_x = x + 72u;
    if ((i & 1u) == 0u)
      gfx_blend_round_rect(row_x, row_y, 198u, 21u, 5u, 0x182239u, 160u);
    if (apps[i] == wm_get_focus())
      gfx_blend_round_rect(row_x, row_y, 198u, 21u, 5u, 0x39345Fu, 235u);
    draw_app_icon(row_x + 8u, row_y + 1u, 19u, apps[i]);
    gfx_draw_string(row_x + 36u, row_y + 7u,
                    app_registry_name(apps[i]), 0xE7E9F8u);
    if (apps[i] == wm_get_focus())
      gfx_draw_rect(row_x + 190u, row_y + 5u, 2u, 11u, 0xA99BFFu);
  }
}

static void draw_desktop_icons(void) {
  if (!g_bi)
    return;

  const app_id_t apps[4] = {
    APP_EXPLORER, APP_SHELL, APP_NOTEPAD, APP_VIEWER
  };
  const uint32_t x = 22u;
  const uint32_t first_y = 132u;
  const uint32_t step = 84u;

  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t y = first_y + i * step;
    uint32_t tile_x = x - 8u;
    uint32_t tile_y = y - 8u;
    gfx_blend_round_rect(tile_x + 2u, tile_y + 3u, 64u, 72u,
                         9u, 0x050914u, 90u);
    gfx_blend_round_rect(tile_x, tile_y, 64u, 72u,
                         9u, 0x151D31u, 215u);
    gfx_draw_rect(tile_x + 8u, tile_y + 70u, 48u, 1u, 0x39415Fu);
    draw_app_icon(x + 3u, y, 40u, apps[i]);

    const char *label = app_registry_name(apps[i]);
    uint32_t length = 0;
    while (label[length])
      ++length;
    uint32_t label_x = tile_x + 2u;
    if (length * 8u < 60u)
      label_x = tile_x + (64u - length * 8u) / 2u;
    gfx_draw_string(label_x, y + 52u, label, 0xE7E9F8u);
  }
}

static void draw_shell_window_if_open(void) {
  if (g_show_shell) draw_shell_window(terminal_window());
}
static void draw_explorer_window_if_open(void) {
  if (!file_manager_is_open())
    return;
  file_manager_render_context_t context = {
    .focused = (uint8_t)(wm_get_focus() == APP_EXPLORER),
    .draw_chrome = draw_window_chrome
  };
  file_manager_draw(&context);
}
static void draw_viewer_window_if_open(void) {
  if (g_viewer_open) draw_viewer_window(&g_viewer_win);
}
static void draw_notepad_window_if_open(void) {
  draw_notepad_window(notepad_window());
}
static void draw_setup_window_if_open(void) {
  if (g_setup_open) draw_setup_window(&g_setup_win);
}
static void draw_open_windows(void) {
  window_compositor_draw();
}

static int hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w,
                    int32_t h) {
  if (px < x || py < y)
    return 0;
  if (px >= (x + w) || py >= (y + h))
    return 0;
  return 1;
}

static int focused_window_contains(int32_t x,int32_t y) {
 const desktop_window_t *w=NULL;
 switch(wm_get_focus()) {
  case APP_SHELL: if(g_show_shell) w=terminal_window(); break;
  case APP_EXPLORER: if(file_manager_is_open()) w=file_manager_window(); break;
  case APP_VIEWER: if(g_viewer_open) w=&g_viewer_win; break;
  case APP_NOTEPAD: if(g_app_open[APP_NOTEPAD]) w=notepad_window(); break;
  case APP_SETUP: if(g_setup_open) w=&g_setup_win; break;
  case APP_DOOM: return doom_window_contains(x,y);
  default: break;
 }
 return w ? hit_test(x,y,w->x,w->y,w->w,w->h) : 0;
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

static void shell_append(const char *s) { terminal_append(s); }

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
  const char *g_shell_line = terminal_line();
  uint32_t g_shell_line_len = terminal_line_length();
  for (uint32_t i = 0; i < g_shell_line_len; i++) {
    char t[2] = {g_shell_line[i], 0};
    shell_append(t);
  }
  shell_append("\n");
  if (g_shell_line_len == 0) {
    shell_append("> ");
    return;
  }

  if (shell_commands_execute(g_shell_line, g_shell_line_len, g_tick,
                             g_last_ptr.x, g_last_ptr.y, &g_rand_state)) {
    /* Built-in command handled by the shell command module. */
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "specs", 5)) {
    shell_run_specs();
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Shell specs command executed\n");
#endif
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "ring3", 5)) {
#ifdef VEXOS_CI_INPUT_TEST
    shell_append("launching ci-ring3.elf...\n");
    launch_ring3_task("ci-ring3.elf");
#else
    shell_append("launching doom.elf...\n");
    launch_ring3_task("doom.elf");
#endif
  } else if (g_shell_line_len == 4 && streq_n(g_shell_line, "doom", 4)) {
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] DOOM command accepted by terminal shell\n");
#endif
    shell_append("launching doom.elf...\n");
    launch_ring3_task("doom.elf");
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
      file_manager_open();
      g_app_open[APP_EXPLORER] = 1;
      wm_raise(APP_EXPLORER);
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
    g_app_open[APP_NOTEPAD] = 1;
    wm_raise(APP_NOTEPAD);
#ifdef VEXOS_CI_INPUT_TEST
    serial_print("[CI] Notepad opened by shell command\n");
#endif
  } else if (g_shell_line_len == 5 && streq_n(g_shell_line, "setup", 5)) {
    shell_append("opening setup wizard...\n");
    g_setup_open = 1;
    g_app_open[APP_SETUP] = 1;
    wm_raise(APP_SETUP);
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

  terminal_clear_line();
  shell_append("> ");
}

static void draw_background(void) {
  if (!g_bi || !g_bi->width || !g_bi->height)
    return;

  uint32_t *dst = g_back ? g_back : g_bi->framebuffer;
  uint32_t w = g_bi->width;
  uint32_t h = g_bi->height;
  uint32_t horizon = (h * 68u) / 100u;
  if (horizon >= h)
    horizon = h - 1u;

  /* VexOS 2.0: ink-to-indigo gradient, with a restrained violet edge light. */
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t t = (y * 255u) / (h ? h : 1u);
    uint32_t r = 7u + (t * 10u) / 255u;
    uint32_t g = 10u + (t * 8u) / 255u;
    uint32_t b = 19u + (t * 24u) / 255u;
    uint32_t *row = &dst[y * g_bi->pitch];
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t glow = (x * 255u) / (w ? w : 1u);
      uint32_t rr = r + (glow * 4u) / 255u;
      uint32_t gg = g + (glow * 2u) / 255u;
      uint32_t bb = b + (glow * 13u) / 255u;
      row[x] = (rr << 16) | (gg << 8) | bb;
    }
  }

  /* Fine grid and circuit traces live on the right, away from desktop shortcuts. */
  uint32_t grid_start = (w * 48u) / 100u;
  for (uint32_t x = grid_start; x < w; x += 48u)
    gfx_draw_rect(x, 34u, 1u, h > 100u ? h - 100u : h, 0x111B30u);
  for (uint32_t y = 42u; y + 64u < h; y += 48u)
    gfx_draw_rect(grid_start, y, w - grid_start, 1u, 0x111B30u);

  uint32_t vx0 = (w * 57u) / 100u;
  uint32_t vx1 = (w * 72u) / 100u;
  uint32_t vx2 = (w * 87u) / 100u;
  uint32_t vy0 = (h * 23u) / 100u;
  uint32_t vy1 = (h * 70u) / 100u;
  if (vx0 < w && vx1 < w && vx2 < w && vy1 < h) {
    gfx_draw_line((int32_t)vx0, (int32_t)vy0,
                  (int32_t)vx1, (int32_t)vy1, 0x1B2A46u);
    gfx_draw_line((int32_t)vx1, (int32_t)vy1,
                  (int32_t)vx2, (int32_t)vy0, 0x1B2A46u);
    gfx_draw_line((int32_t)(vx0 + 20u < w ? vx0 + 20u : vx0),
                  (int32_t)(vy0 + 10u < h ? vy0 + 10u : vy0),
                  (int32_t)vx1, (int32_t)(vy1 - 18u),
                  0x17243Cu);
    gfx_draw_line((int32_t)vx1, (int32_t)(vy1 - 18u),
                  (int32_t)(vx2 > 20u ? vx2 - 20u : vx2),
                  (int32_t)(vy0 + 10u < h ? vy0 + 10u : vy0),
                  0x17243Cu);
  }

  /* Sparse circuit nodes make the wallpaper feel built, not noisy. */
  for (uint32_t i = 0; i < 5u; ++i) {
    uint32_t nx = grid_start + ((w - grid_start) * (i + 1u)) / 6u;
    uint32_t ny = (h * (18u + i * 11u)) / 100u;
    if (nx + 5u < w && ny + 5u < h) {
      gfx_blend_round_rect(nx, ny, 5u, 5u, 2u, 0x6F65D8u, 210u);
      gfx_draw_rect(nx + 1u, ny + 1u, 3u, 3u, 0xA99BFFu);
    }
  }

  /* Brand block: simple type, generous spacing, no bitmap lettering. */
  gfx_draw_rect(24u, 40u, 3u, 42u, 0x8C7BFFu);
  gfx_draw_string(40u, 42u, "VEXOS", 0xF2F3FFu);
  gfx_draw_string(40u, 61u, "SYSTEM ENVIRONMENT", 0x8390ADu);
  gfx_draw_rect(40u, 82u, 178u, 1u, 0x29324Au);
  gfx_draw_string(40u, 93u, "DESKTOP 2.0  /  READY", 0xA79DFFu);
}static void draw_text_panel(void) {
  if (!g_bi)
    return;

  uint32_t panel_h = 96;
  if (panel_h > g_bi->height)
    panel_h = g_bi->height;

  gfx_draw_rect(8, 8, g_bi->width - 16, panel_h, 0x101010);
  gfx_draw_rect(8, 8, g_bi->width - 16, 1, 0x303030);
  gfx_draw_rect(8, 8 + panel_h - 1, g_bi->width - 16, 1, 0x303030);

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
      gfx_draw_glyph(16 + cx * 8u, 16 + cy * 8u, (uint8_t)c, 0xE0E0E0);
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
            gfx_put_pixel((uint32_t)(x + (int32_t)cx + xx),
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
        gfx_put_pixel((uint32_t)(x + (int32_t)cx), (uint32_t)(y + (int32_t)cy), fg);
      }
    }
  }
}

/* Restore the pixels captured before the previous fallback cursor draw. */
static void desktop_fallback_cursor_restore(void) {
  if (!g_fallback_cursor_valid || !g_bi || !g_bi->framebuffer)
    return;

  for (uint32_t row = 0; row < g_fallback_cursor_height; ++row) {
    uint32_t *dst = &g_bi->framebuffer[
        (uint32_t)(g_fallback_cursor_y + (int32_t)row) * g_bi->pitch +
        (uint32_t)g_fallback_cursor_x];
    const uint32_t *src =
        &g_fallback_cursor_save[row * DESKTOP_CURSOR_SAVE_SIZE];
    for (uint32_t col = 0; col < g_fallback_cursor_width; ++col)
      dst[col] = src[col];
  }
  g_fallback_cursor_valid = 0;
}

/* Capture only the small rectangle touched by the 16px cursor and drag marker. */
static void desktop_fallback_cursor_capture(int32_t cursor_x,
                                            int32_t cursor_y) {
  if (!g_bi || !g_bi->framebuffer || cursor_x < 0 || cursor_y < 0)
    return;

  int32_t left = cursor_x > 0 ? cursor_x - 1 : 0;
  int32_t top = cursor_y > 0 ? cursor_y - 1 : 0;
  if ((uint32_t)left >= g_bi->width || (uint32_t)top >= g_bi->height)
    return;

  uint32_t width = g_bi->width - (uint32_t)left;
  uint32_t height = g_bi->height - (uint32_t)top;
  if (width > DESKTOP_CURSOR_SAVE_SIZE)
    width = DESKTOP_CURSOR_SAVE_SIZE;
  if (height > DESKTOP_CURSOR_SAVE_SIZE)
    height = DESKTOP_CURSOR_SAVE_SIZE;

  g_fallback_cursor_x = left;
  g_fallback_cursor_y = top;
  g_fallback_cursor_width = width;
  g_fallback_cursor_height = height;

  for (uint32_t row = 0; row < height; ++row) {
    const uint32_t *src = &g_bi->framebuffer[
        (uint32_t)(top + (int32_t)row) * g_bi->pitch + (uint32_t)left];
    uint32_t *dst =
        &g_fallback_cursor_save[row * DESKTOP_CURSOR_SAVE_SIZE];
    for (uint32_t col = 0; col < width; ++col)
      dst[col] = src[col];
  }
  g_fallback_cursor_valid = 1;
}

static void desktop_draw_pointer_cursor(const input_pointer_state_t *pointer) {
  if (!pointer)
    return;

  uint32_t foreground = 0xFFFFFFu;
  uint32_t outline = 0x000000u;
  if (pointer->buttons & 1u)
    foreground = 0xFFCC00u;
  else if (pointer->buttons & 2u)
    foreground = 0x00CCFFu;

  draw_cursor(pointer->x, pointer->y, foreground, outline);
  if ((pointer->buttons & 1u) &&
      (pointer->dx || pointer->dy)) {
    int32_t marker_x = pointer->x + 18;
    int32_t marker_y = pointer->y + 18;
    if (marker_x < 0) marker_x = 0;
    if (marker_y < 0) marker_y = 0;
    gfx_draw_rect((uint32_t)marker_x, (uint32_t)marker_y,
                  5u, 5u, 0xFFCC00u);
    gfx_draw_rect((uint32_t)marker_x, (uint32_t)marker_y,
                  5u, 1u, 0x000000u);
    gfx_draw_rect((uint32_t)marker_x, (uint32_t)marker_y + 4u,
                  5u, 1u, 0x000000u);
  }
}

void desktop_init(BootInfo *bootInfo) {
  g_bi = bootInfo;
  compositor_init(bootInfo);
  gfx_init(bootInfo, NULL, _binary_CGA_F08_start);
  g_tick = 0;
  g_back = 0;
  g_bg = 0;
  g_text_len = 0;
  g_start_open = 0;
  g_show_shell = 0;
  g_rand_state = 0xC001D00Du;
  g_last_ptr.x = 0;
  g_last_ptr.y = 0;
  g_last_ptr.dx = 0;
  g_last_ptr.dy = 0;
  g_last_ptr.buttons = 0;
  g_last_ptr.is_absolute = 0;
  g_viewer_open = 0;
  g_viewer_dragging = 0;
  g_viewer_name[0] = 0;
  g_viewer_text[0] = 0;
  g_viewer_text_len = 0;
  g_viewer_kind = VIEWER_CONTENT_TEXT;
  g_viewer_pixels = NULL;
  g_viewer_image_width = 0;
  g_viewer_image_height = 0;
  g_viewer_scroll = 0;

  for (uint32_t i = 0; i < APP_COUNT; i++)
    g_app_open[i] = 0;
  wm_init();
  app_registry_init();
  window_compositor_init();
  window_compositor_register(APP_SHELL, &g_app_open[APP_SHELL], draw_shell_window_if_open);
  window_compositor_register(APP_EXPLORER, &g_app_open[APP_EXPLORER], draw_explorer_window_if_open);
  window_compositor_register(APP_VIEWER, &g_app_open[APP_VIEWER], draw_viewer_window_if_open);
  window_compositor_register(APP_NOTEPAD, &g_app_open[APP_NOTEPAD], draw_notepad_window_if_open);
  window_compositor_register(APP_SETUP, &g_app_open[APP_SETUP], draw_setup_window_if_open);
  window_compositor_register(APP_DOOM, &g_app_open[APP_DOOM], draw_doom_window_if_open);
  doom_window_init(bootInfo->width,bootInfo->height);
  wm_raise(APP_SHELL);

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

  /* If either half of the double buffer failed, return the partial
     allocation so fallback rendering has a consistent framebuffer target. */
  if (!g_back || !g_bg) {
    if (g_back) kfree(g_back);
    if (g_bg) kfree(g_bg);
    g_back = NULL;
    g_bg = NULL;
  }

  gfx_set_backbuffer(g_back);
  if (g_back && g_bg)
    compositor_enable_pageflip();

  g_last_cursor_x = -1000;
  g_last_cursor_y = -1000;
  g_last_cursor_buttons = 0;
  g_fallback_cursor_valid = 0;
  g_fallback_scene_dirty = 1;

  input_init(bootInfo->width, bootInfo->height);
  terminal_init(g_bi->width, g_bi->height);
  shell_append("> ");

  g_viewer_win.w = (int32_t)(g_bi->width > 680u ? 620u : (g_bi->width > 48u ? g_bi->width - 48u : g_bi->width));
  g_viewer_win.h = (int32_t)(g_bi->height > 480u ? 420u : (g_bi->height > 80u ? g_bi->height - 80u : g_bi->height));
  g_viewer_win.x = ((int32_t)g_bi->width - g_viewer_win.w) / 2 + 30;
  g_viewer_win.y = ((int32_t)g_bi->height - g_viewer_win.h) / 2 + 26;

  file_manager_init(g_bi->width, g_bi->height);
  notepad_init(g_bi->width, g_bi->height);

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
    gfx_set_backbuffer(g_back);
    g_rand_state = 0xC001D00Du;
    draw_background();
    g_rand_state = saved;
    g_back = saved_back;
    gfx_set_backbuffer(g_back);

    // Initialize the presented frame.
    compositor_blit(g_back, g_bg, 0, 0, g_bi->width, g_bi->height);
    draw_desktop_icons();
    
    draw_open_windows();
    if (!desktop_doom_fullscreen_active())
      draw_taskbar();
    draw_start_menu();
    input_pointer_state_t p = input_pointer_get();
    compositor_present(g_back, g_bi->width, g_bi->height);
    gfx_set_backbuffer(compositor_frontbuffer());
    uint32_t fg = 0xFFFFFF;
    uint32_t outline = 0x000000;
    if (p.buttons & 1u) {
      fg = 0xFFCC00;
    } else if (p.buttons & 2u) {
      fg = 0x00CCFF;
    }
    draw_cursor(p.x, p.y, fg, outline);
    gfx_set_backbuffer(g_back);
  } else {
    draw_background();
  }
}

void desktop_tick(void) {
  /* Reap the previous process's kernel transition stack only after the
     exception/syscall handler has switched back to this kernel task. */
  usermode_reap_deferred();
  if (g_deferred_ring3_task_stack) {
    kfree(g_deferred_ring3_task_stack);
    g_deferred_ring3_task_stack = NULL;
  }

  if (!g_bi)
    return;

  static uint32_t prev_buttons = 0;
#ifdef VEXOS_CI_INPUT_TEST
  static uint32_t ci_doom_buttons = UINT32_MAX;
#endif
  input_pointer_state_t p0 = input_pointer_get();
  g_last_ptr = p0;
#ifdef VEXOS_CI_INPUT_TEST
  if (g_app_open[APP_DOOM] && p0.buttons != ci_doom_buttons) {
    ci_doom_buttons = p0.buttons;
    serial_print((p0.buttons & 1u)
        ? "[CI] Doom left mouse button down reached desktop_tick\n"
        : "[CI] Doom left mouse button up reached desktop_tick\n");
  }
#endif
#ifdef VEXOS_CI_INPUT_TEST
  if (g_app_open[APP_DOOM] && !g_doom_close_pointer_reported &&
      p0.x >= (int32_t)g_bi->width - 32 && p0.y >= 0 && p0.y <= 32) {
    g_doom_close_pointer_reported = 1;
    serial_print("[CI] USB tablet pointer reached Doom close-button area\\n");
  }
#endif
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
  static uint8_t last_clock_second = 255u;
  uint8_t clock_h, clock_m, clock_second, clock_day, clock_month;
  uint16_t clock_year;
  uint8_t clock_changed = 0;
  if (desktop_clock(&clock_h, &clock_m, &clock_second, &clock_day,
                    &clock_month, &clock_year) &&
      clock_second != last_clock_second) {
    last_clock_second = clock_second;
    clock_changed = 1;
    ui_changed = 1;
  }
#ifdef VEXOS_CI_DOOM_DEMO
  /*
   * The DOOM capture runs in a headless CI guest, where synthetic host mouse
   * clicks are backend-dependent. Stage the demo on real RTC second changes:
   * open the normal terminal window, type through terminal_handle_char(), and
   * submit through shell_run_line(). This is compiled only into CI builds.
   */
  static uint8_t ci_demo_stage;
  static uint8_t ci_demo_char_index;
  static uint8_t ci_demo_clock_primed;
  static const char ci_demo_command[] = "doom";
  if (clock_changed) {
    if (ci_demo_stage == 0u) {
      if (!ci_demo_clock_primed) {
        /* Let the opening desktop appear in the recording first. */
        ci_demo_clock_primed = 1u;
      } else {
        g_show_shell = 1;
        g_app_open[APP_SHELL] = 1;
        wm_raise(APP_SHELL);
        ci_demo_stage = 1u;
        serial_print("[CI] Terminal opened by automated desktop playtest\n");
        ui_changed = 1;
      }
    } else if (ci_demo_stage == 1u && ci_demo_char_index < 4u) {
      if (terminal_handle_char(ci_demo_command[ci_demo_char_index])) {
        ci_demo_char_index++;
        ui_changed = 1;
      }
      if (ci_demo_char_index == 4u)
        ci_demo_stage = 2u;
    } else if (ci_demo_stage == 2u) {
      shell_run_line();
      ci_demo_stage = 3u;
      ui_changed = 1;
    }
  }
#endif
  if (g_app_open[APP_DOOM] && !g_doom_tick_reported) {
    g_doom_tick_reported = 1;
    serial_print("[DOOMWIN] Desktop task sees the open Doom window\n");
  }
  uint8_t doom_frame_changed = g_doom_window_dirty;
  if (doom_frame_changed) g_doom_window_dirty = 0;
  g_doom_click_handled=0;

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
    if (wm_get_focus() == APP_EXPLORER && file_manager_is_open() &&
        file_manager_handle_char(c)) {
      ui_changed = 1;
    } else if (wm_get_focus() == APP_NOTEPAD && g_app_open[APP_NOTEPAD]) {
      if (notepad_handle_char(c)) ui_changed = 1;
#ifdef VEXOS_CI_INPUT_TEST
      static uint8_t ci_notepad_text_seen;
      const char *np_text = notepad_text();
      uint32_t np_len = notepad_text_length();
      if (!ci_notepad_text_seen && np_len >= 3u &&
          np_text[np_len - 3u] == 'v' && np_text[np_len - 2u] == 'e' &&
          np_text[np_len - 1u] == 'x') {
        ci_notepad_text_seen = 1;
        serial_print("[CI] Notepad accepted the full typed text vex\n");
      }
#endif
    } else if (wm_get_focus() == APP_SHELL && g_show_shell && g_app_open[APP_SHELL]) {
      uint8_t input_result = terminal_handle_char(c);
      if (input_result == 2u) {
#ifdef VEXOS_CI_INPUT_TEST
        serial_print("[CI] Shell command submitted through keyboard\n");
#endif
        shell_run_line(); ui_changed = 1;
      } else if (input_result == 1u) {
#ifdef VEXOS_CI_INPUT_TEST
        static uint8_t ci_shell_char_seen;
        if (!ci_shell_char_seen) { ci_shell_char_seen = 1; serial_print("[CI] Shell received printable keyboard input\n"); }
#endif
        ui_changed = 1;
      }
    }
  }

  // Click handling
  if (pressed & 1u) {
    if (g_app_open[APP_DOOM]) {
      char click_x[12], click_y[12];
      shell_u32_to_dec(click_x, (uint32_t)p0.x);
      shell_u32_to_dec(click_y, (uint32_t)p0.y);
      serial_print("[DOOMWIN] Desktop click x=");
      serial_print(click_x);
      serial_print(" y=");
      serial_print(click_y);
      serial_print("\n");
    }
    uint32_t bar_h = 56;
    if (bar_h > g_bi->height)
      bar_h = g_bi->height;
    int32_t tb_y = (int32_t)g_bi->height - (int32_t)bar_h;

    // The taskbar is painted above maximized windows, so it remains interactive.
    if (p0.y >= tb_y) g_doom_click_handled = 1;

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
    } else if (g_start_open) {
      g_doom_click_handled = 1;
      int32_t mx=10,mw=280,mh=168,my=tb_y-176;
      if(hit_test(p0.x,p0.y,mx,my,mw,mh)){int32_t ry=p0.y-my;
        if(ry>=28&&ry<50){g_show_shell=1;g_app_open[APP_SHELL]=1;wm_raise(APP_SHELL);
#ifdef VEXOS_CI_INPUT_TEST
          serial_print("[CI] Shell opened from Start menu mouse click\n");
#endif
          g_start_open=0;ui_changed=1;
        }else if(ry>=50&&ry<72){if(simplefs_init()){file_manager_open();g_app_open[APP_EXPLORER]=1;wm_raise(APP_EXPLORER);}else shell_append("fs init failed\n> ");g_start_open=0;ui_changed=1;
        }else if(ry>=72&&ry<94){g_app_open[APP_NOTEPAD]=1;wm_raise(APP_NOTEPAD);g_start_open=0;ui_changed=1;
        }else if(ry>=94&&ry<116){g_viewer_open=1;g_app_open[APP_VIEWER]=1;wm_raise(APP_VIEWER);g_start_open=0;ui_changed=1;
        }else if(ry>=116&&ry<138){g_setup_open=1;g_app_open[APP_SETUP]=1;g_setup_step=0;wm_raise(APP_SETUP);g_start_open=0;ui_changed=1;
        }else if(ry>=138&&ry<162){g_start_open=0;if(g_ring3_task_started)wm_raise(APP_DOOM);else launch_ring3_task("doom.elf");ui_changed=1;}
      }else{g_start_open=0;ui_changed=1;}
    }

    // Desktop shortcuts launch their matching applications.
    if(!doom_window_is_maximized()&&!g_start_open){for(uint32_t i=0;i<4u;i++){int32_t iy=132+(int32_t)i*84;if(hit_test(p0.x,p0.y,15,iy-7,58,70)){
      if(i==0u){if(simplefs_init()){file_manager_open();g_app_open[APP_EXPLORER]=1;wm_raise(APP_EXPLORER);}else shell_append("fs init failed\n> ");}
      else if(i==1u){
        g_show_shell=1;g_app_open[APP_SHELL]=1;wm_raise(APP_SHELL);
#ifdef VEXOS_CI_INPUT_TEST
        serial_print("[CI] Terminal opened from desktop shortcut\n");
#endif
      }else if(i==2u){g_app_open[APP_NOTEPAD]=1;wm_raise(APP_NOTEPAD);}else{g_viewer_open=1;g_app_open[APP_VIEWER]=1;wm_raise(APP_VIEWER);}ui_changed=1;break;}}}

    // Taskbar dock icons: click to focus/open

    // Taskbar dock icons: click to focus/open
    {
      uint32_t sy = (uint32_t)tb_y + 6u;
      uint32_t sh = bar_h - 12u;

      uint32_t dock_w = 320;
      if (dock_w + 32 > g_bi->width)
        dock_w = g_bi->width > 32 ? (g_bi->width - 32) : 1;
      uint32_t dock_x = (g_bi->width - dock_w) / 2u;

      uint32_t icon = 40;
      uint32_t gap = 10;
      uint32_t ix = dock_x + 16;
      uint32_t iy = sy + ((sh > icon) ? ((sh - icon) / 2u) : 0u);

      for (uint32_t i = 0; i < 5; i++) {
        if (hit_test(p0.x, p0.y, (int32_t)ix, (int32_t)iy, (int32_t)icon,
                     (int32_t)icon)) {
          if (i == 0) {
            g_show_shell = 1;
            g_app_open[APP_SHELL] = 1;
            wm_raise(APP_SHELL);
          } else if (i == 1) {
            if (simplefs_init()) {
              file_manager_open();
              g_app_open[APP_EXPLORER] = 1;
              wm_raise(APP_EXPLORER);
#ifdef VEXOS_CI_INPUT_TEST
              serial_print("[CI] Files opened from taskbar dock click\n");
#endif
            } else {
              shell_append("fs init failed\n> ");
            }
          } else if (i == 2) {
            g_viewer_open = 1;
            g_app_open[APP_VIEWER] = 1;
            wm_raise(APP_VIEWER);
          } else if (i == 3) {
            g_app_open[APP_NOTEPAD] = 1;
            wm_raise(APP_NOTEPAD);
          } else if (i == 4 && g_ring3_task_started) {
            wm_raise(APP_DOOM);
          }
          ui_changed = 1;
          break;
        }
        ix += icon + gap;
        if (ix + icon + 16 > dock_x + dock_w)
          break;
      }
    }

    if (g_app_open[APP_DOOM] && !g_doom_click_handled &&
        (wm_get_focus()==APP_DOOM || !focused_window_contains(p0.x,p0.y)) &&
        doom_window_handle_click(p0.x,p0.y)) {
      g_doom_click_handled=1;
      g_doom_mouse_captured=doom_window_pointer_in_content(p0.x,p0.y)?1u:0u;
      ui_changed=1;
    }

    // File Manager owns selection, search, sorting, and pointer drag state.
    if (file_manager_is_open() && !g_doom_click_handled) {
      file_manager_event_t event = file_manager_handle_click(
          p0.x, p0.y, g_tick, viewer_load_file);
      if (event == FILE_MANAGER_EVENT_CLOSED) {
        g_app_open[APP_EXPLORER] = 0;
        ui_changed = 1;
      } else if (event == FILE_MANAGER_EVENT_OPEN_FAILED) {
        shell_append("files: could not open selected file\\n> ");
        ui_changed = 1;
      } else if (event == FILE_MANAGER_EVENT_CHANGED) {
        g_app_open[APP_EXPLORER] = 1;
        ui_changed = 1;
      }
    }

    if (g_viewer_open && !g_doom_click_handled) {
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
        g_app_open[APP_VIEWER] = 1;
        wm_raise(APP_VIEWER);
        ui_changed = 1;
      } else if (hit_test(p0.x, p0.y, wx, wy, ww, g_viewer_win.h)) {
        wm_raise(APP_VIEWER);
        ui_changed = 1;
      }
    }

    // Notepad owns click handling, save operations, and window dragging.
    if (g_app_open[APP_NOTEPAD] && !g_doom_click_handled) {
      notepad_click_result_t result = notepad_handle_click(p0.x, p0.y);
      if (result == NOTEPAD_CLICK_CLOSE) { g_app_open[APP_NOTEPAD] = 0; ui_changed = 1; }
      else if (result == NOTEPAD_CLICK_SAVE_OK) { shell_append("notepad: saved\n> "); ui_changed = 1; }
      else if (result == NOTEPAD_CLICK_SAVE_FAILED) { shell_append("notepad: save failed\n> "); ui_changed = 1; }
      else if (result == NOTEPAD_CLICK_FS_FAILED) { shell_append("notepad: fs init failed\n> "); ui_changed = 1; }
      else if (result == NOTEPAD_CLICK_SCROLL) ui_changed = 1;
      else if (result == NOTEPAD_CLICK_FOCUS) { wm_raise(APP_NOTEPAD); ui_changed = 1; }
    }

    // Setup Wizard interactions
    if (g_setup_open && !g_doom_click_handled) {
      int32_t wx = g_setup_win.x;
      int32_t wy = g_setup_win.y;
      int32_t ww = g_setup_win.w;
      int32_t wh = g_setup_win.h;

      if (hit_test(p0.x, p0.y, wx, wy, ww, wh)) {
        wm_raise(APP_SETUP);
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

  if ((pressed & 1u) && g_show_shell && !g_doom_click_handled) {terminal_click_result_t r=terminal_handle_click(p0.x,p0.y);if(r==TERMINAL_CLICK_CLOSE){g_show_shell=0;ui_changed=1;}else if(r==TERMINAL_CLICK_FOCUS){wm_raise(APP_SHELL);ui_changed=1;}}

  if (!(buttons & 1u)) {
    file_manager_pointer_release();
    g_viewer_dragging = 0;
    notepad_pointer_release();
    terminal_pointer_release();
    g_setup_dragging = 0;
  }

  if (p0.dx || p0.dy) {int32_t ox=terminal_window()->x,oy=terminal_window()->y;terminal_pointer_move(p0.x,p0.y);if(ox!=terminal_window()->x||oy!=terminal_window()->y)ui_changed=1;}

  if (p0.dx || p0.dy) {
    desktop_window_t *files_window = file_manager_window();
    int32_t old_x = files_window->x;
    int32_t old_y = files_window->y;
    file_manager_pointer_move(p0.x, p0.y);
    if (old_x != files_window->x || old_y != files_window->y)
      ui_changed = 1;
  }

  if (g_viewer_dragging && (p0.dx || p0.dy)) {
    g_viewer_win.x = p0.x - g_viewer_drag_off_x;
    g_viewer_win.y = p0.y - g_viewer_drag_off_y;
    ui_changed = 1;
  }

  if (p0.dx || p0.dy) {
    int32_t old_x = notepad_window()->x, old_y = notepad_window()->y;
    notepad_pointer_move(p0.x, p0.y);
    if (old_x != notepad_window()->x || old_y != notepad_window()->y) ui_changed = 1;
  }

  if (g_setup_dragging && (p0.dx || p0.dy)) {
    g_setup_win.x = p0.x - g_setup_drag_off_x;
    g_setup_win.y = p0.y - g_setup_drag_off_y;
    ui_changed = 1;
  }
  if(doom_window_pointer_update(p0.x,p0.y,buttons,p0.dx,p0.dy))ui_changed=1;

  input_pointer_state_t pointer = input_pointer_get();
  uint8_t cursor_changed =
      (uint8_t)(pointer.x != g_last_cursor_x ||
                pointer.y != g_last_cursor_y ||
                pointer.buttons != g_last_cursor_buttons);

  uint32_t doom_visible_bottom = g_bi->height;
  if (!desktop_doom_fullscreen_active())
    doom_visible_bottom = (g_bi->height > 56u) ? (g_bi->height - 56u) : 0u;
  uint32_t doom_visible_height =
      (doom_visible_bottom > 31u) ? (doom_visible_bottom - 31u) : 0u;
  uint8_t doom_partial_update = (uint8_t)(
      doom_frame_changed && g_app_open[APP_DOOM] &&
      wm_get_focus() == APP_DOOM && doom_window_is_maximized() &&
      !g_start_open && !ui_changed && doom_visible_height > 0u &&
      g_bi->width > 2u);

  /* Any non-Doom reason to redraw keeps the normal full-scene path. */
  if (doom_frame_changed && !doom_partial_update)
    ui_changed = 1;

  if (!g_back || !g_bg) {
    /* In fallback mode the framebuffer is the scene. Redraw it only when UI
       state changes; cursor movement uses a 26x26 save-under rectangle. */
    uint8_t scene_changed =
        (uint8_t)(g_fallback_scene_dirty || ui_changed || g_tick < 2u);

    if (doom_partial_update && !scene_changed) {
      desktop_fallback_cursor_restore();
      draw_doom_window_visible_content(doom_visible_height);
      desktop_fallback_cursor_capture(pointer.x, pointer.y);
      desktop_draw_pointer_cursor(&pointer);
    } else if (scene_changed) {
      g_fallback_cursor_valid = 0;
      if (g_app_open[APP_DOOM] && !g_doom_render_trace_done)
        serial_print("[DOOMWIN] Desktop redraw using optimized direct framebuffer\n");

      draw_background();
      draw_desktop_icons();
      draw_open_windows();
      if (!desktop_doom_fullscreen_active())
      draw_taskbar();
      draw_start_menu();

      desktop_fallback_cursor_capture(pointer.x, pointer.y);
      desktop_draw_pointer_cursor(&pointer);
      g_fallback_scene_dirty = 0;
    } else if (cursor_changed) {
      desktop_fallback_cursor_restore();
      desktop_fallback_cursor_capture(pointer.x, pointer.y);
      desktop_draw_pointer_cursor(&pointer);
    }

    g_last_cursor_x = pointer.x;
    g_last_cursor_y = pointer.y;
    g_last_cursor_buttons = pointer.buttons;
  } else {
    static uint8_t g_desktop_dirty = 1;
    uint8_t scene_changed = (uint8_t)(g_desktop_dirty || ui_changed || g_tick < 2u);

    if (scene_changed) {
      if (g_app_open[APP_DOOM] && !g_doom_render_trace_done)
        serial_print("[DOOMWIN] Desktop scene redraw started\n");
      compositor_blit(g_back, g_bg, 0, 0, g_bi->width, g_bi->height);
      draw_desktop_icons();
      draw_open_windows();
      if (g_app_open[APP_DOOM] && !g_doom_render_trace_done) {
        serial_print("[DOOMWIN] Finished window draw stage\n");
        g_doom_render_trace_done = 1;
      }
      if (!desktop_doom_fullscreen_active())
      draw_taskbar();
      draw_start_menu();
      g_desktop_dirty = 0;
    }

    uint8_t doom_partial_presented = 0;
    if (doom_partial_update && !scene_changed) {
      gfx_set_backbuffer(g_back);
      draw_doom_window_visible_content(doom_visible_height);
      compositor_present_rect(g_back, 1u, 31u,
                              g_bi->width - 2u, doom_visible_height);
      doom_partial_presented = 1;
    }

    if (scene_changed)
      compositor_present(g_back, g_bi->width, g_bi->height);

    if (scene_changed || cursor_changed || doom_partial_presented || g_tick < 2u) {
      if (!scene_changed) {
        int32_t restore_x = g_last_cursor_x > 0 ? g_last_cursor_x - 1 : 0;
        int32_t restore_y = g_last_cursor_y > 0 ? g_last_cursor_y - 1 : 0;
        uint32_t *restore_target = compositor_frontbuffer();

        /* After a page flip, the old cursor is left on the now-hidden page.
           Erase it there so the next flip cannot resurrect a cursor ghost. */
        if (doom_partial_presented && compositor_pageflip_active()) {
          uint32_t *hidden_page = compositor_backbuffer();
          if (hidden_page)
            compositor_blit(hidden_page, g_back, (uint32_t)restore_x,
                            (uint32_t)restore_y, 26u, 26u);
        } else {
          compositor_wait_vsync();
          compositor_blit(restore_target, g_back, (uint32_t)restore_x,
                          (uint32_t)restore_y, 26u, 26u);
        }
      }

      /* The cursor is an overlay: draw it on the currently visible page,
         never into the clean software scene or the hidden scanout page. */
      gfx_set_backbuffer(compositor_frontbuffer());
      desktop_draw_pointer_cursor(&pointer);
      gfx_set_backbuffer(g_back);

      g_last_cursor_x = pointer.x;
      g_last_cursor_y = pointer.y;
      g_last_cursor_buttons = pointer.buttons;
    }
  }
  g_tick++;
}

// ============================================================================
// First-boot welcome guide. This is intentionally non-destructive: VexOS does
// not yet have a real disk partitioner or installer, so this guide never
// claims to partition, format, or install to a drive.
// ============================================================================

static uint8_t g_fullscreen_setup_step = 0;
static uint8_t g_setup_complete = 0;

static void draw_fullscreen_setup_step(uint32_t screen_w, uint32_t screen_h) {
  static const char *step_titles[] = {
    "Welcome to VexOS",
    "Storage Support",
    "Keyboard and Mouse",
    "Installation Status",
    "Ready"
  };

  gfx_draw_rect(0, 0, screen_w, screen_h, 0x0A0D14);
  gfx_draw_string(32, 32, step_titles[g_fullscreen_setup_step], 0xFFFFFF);
  gfx_draw_rect(32, 52, screen_w - 64, 2, 0x2A3140);

  uint32_t bar_y = 80;
  uint32_t bar_w = screen_w - 64;
  uint32_t step_w = bar_w / 5u;
  gfx_draw_rect(32, bar_y, bar_w, 8, 0x1A1F2A);
  for (uint32_t i = 0; i < 5u; ++i) {
    uint32_t cx = 32u + i * step_w;
    uint32_t color = (i <= g_fullscreen_setup_step) ? 0x22C55E : 0x303040;
    gfx_draw_rect(cx + 2u, bar_y + 2u, step_w - 4u, 4u, color);
  }

  uint32_t content_y = 120u;
  uint32_t content_h = screen_h > 240u ? screen_h - 240u : 1u;
  gfx_blend_rect(32, content_y, screen_w - 64u, content_h, 0x0F141C, 220);
  gfx_draw_rect(32, content_y, screen_w - 64u, 1u, 0x2A3140);
  gfx_draw_rect(32, content_y + content_h - 1u, screen_w - 64u, 1u, 0x0B0E14);

  uint32_t text_x = 56u;
  uint32_t text_y = content_y + 32u;
  switch (g_fullscreen_setup_step) {
    case 0:
      gfx_draw_string(text_x, text_y, "Welcome to VexOS.", 0xFFFFFF);
      gfx_draw_string(text_x, text_y + 32u, "This is an early x86_64 operating-system build.", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 56u, "It currently boots from a UEFI disk image.", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 96u, "This guide will not modify any drive.", 0x22C55E);
      break;
    case 1:
      gfx_draw_string(text_x, text_y, "VexOS currently uses SimpleFS for data files.", 0xFFFFFF);
      gfx_draw_string(text_x, text_y + 32u, "AHCI support is being developed for SATA drives.", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 56u, "NVMe support is not implemented yet.", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 96u, "Storage support depends on the controller.", 0xFFB000);
      break;
    case 2:
      gfx_draw_string(text_x, text_y, "USB input is handled by the xHCI driver.", 0xFFFFFF);
      gfx_draw_string(text_x, text_y + 32u, "Keyboard and pointer compatibility varies", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 56u, "between firmware and physical hardware.", 0xD0D6E0);
      break;
    case 3:
      gfx_draw_string(text_x, text_y, "A real disk installer is not implemented yet.", 0xFFFFFF);
      gfx_draw_string(text_x, text_y + 32u, "This build cannot safely partition or format", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 56u, "an internal drive or install alongside Windows.", 0xD0D6E0);
      gfx_draw_string(text_x, text_y + 104u, "No disks will be changed by this setup guide.", 0x22C55E);
      break;
    case 4:
      gfx_draw_string(text_x, text_y, "The VexOS desktop is ready to start.", 0x22C55E);
      gfx_draw_string(text_x, text_y + 40u, "Click Finish to continue to the desktop.", 0xFFFFFF);
      break;
  }

  uint32_t btn_y = screen_h > 72u ? screen_h - 72u : 0u;
  if (g_fullscreen_setup_step > 0u) {
    gfx_draw_rect(32, btn_y, 100u, 32u, 0x2A3140);
    gfx_draw_string(64, btn_y + 12u, "Back", 0xFFFFFF);
  } else {
    gfx_draw_rect(32, btn_y, 100u, 32u, 0x1A1F2A);
    gfx_draw_string(64, btn_y + 12u, "Back", 0x505050);
  }

  const char *button_text = (g_fullscreen_setup_step == 4u) ? "Finish" : "Next";
  uint32_t button_color = (g_fullscreen_setup_step == 4u) ? 0x22C55E : 0x2244AA;
  uint32_t button_x = screen_w - 132u;
  gfx_draw_rect(button_x, btn_y, 100u, 32u, button_color);
  gfx_draw_string(button_x + 28u, btn_y + 12u, button_text, 0xFFFFFF);
}

void desktop_run_fullscreen_wizard(BootInfo *bootInfo) {
  if (!bootInfo || !bootInfo->framebuffer || !bootInfo->width || !bootInfo->height)
    return;

  g_bi = bootInfo;
  compositor_init(bootInfo);
  gfx_init(bootInfo, NULL, _binary_CGA_F08_start);
  g_setup_complete = 0;
  g_fullscreen_setup_step = 0;
  input_init(bootInfo->width, bootInfo->height);

  uint32_t screen_w = bootInfo->width;
  uint32_t screen_h = bootInfo->height;
  uint32_t *fb = bootInfo->framebuffer;
  uint32_t *wizard_back = NULL;
  if (bootInfo->pitch && bootInfo->height) {
    uint64_t bytes = (uint64_t)bootInfo->pitch * (uint64_t)bootInfo->height * 4u;
    if (bytes <= 0xFFFFFFFFu)
      wizard_back = (uint32_t *)kmalloc((uint32_t)bytes);
  }

  uint8_t running = 1;
  uint8_t btn_pressed = 0;
  uint8_t needs_redraw = 1;
  input_pointer_state_t last_p = {0};

  while (running) {
    xhci_poll_events();
    input_pointer_state_t p = input_pointer_get();

    if (needs_redraw || p.x != last_p.x || p.y != last_p.y || p.buttons != last_p.buttons) {
      g_back = wizard_back ? wizard_back : fb;
      gfx_set_backbuffer(g_back);
      draw_fullscreen_setup_step(screen_w, screen_h);

      uint32_t cursor_color = (p.buttons & 1u) ? 0xFFCC00u :
                             ((p.buttons & 2u) ? 0x00CCFFu : 0xFFFFFFu);
      for (int32_t i = -8; i <= 8; ++i) {
        if (p.y + i >= 0 && p.y + i < (int32_t)screen_h)
          gfx_put_pixel((uint32_t)p.x, (uint32_t)(p.y + i), cursor_color);
        if (p.x + i >= 0 && p.x + i < (int32_t)screen_w)
          gfx_put_pixel((uint32_t)(p.x + i), (uint32_t)p.y, cursor_color);
      }

      if (wizard_back)
        compositor_blit(fb, wizard_back, 0, 0, screen_w, screen_h);
      needs_redraw = 0;
      last_p = p;
    }

    for (volatile uint32_t i = 0; i < 100000u; ++i) {
      __asm__ volatile("" ::: "memory");
    }

    if ((p.buttons & 1u) && !btn_pressed) {
      btn_pressed = 1;
      uint32_t button_y = screen_h > 72u ? screen_h - 72u : 0u;
      uint32_t button_x = screen_w - 132u;
      if (p.x >= (int32_t)button_x && p.x < (int32_t)(button_x + 100u) &&
          p.y >= (int32_t)button_y && p.y < (int32_t)(button_y + 32u)) {
        if (g_fullscreen_setup_step == 4u) {
          running = 0;
          g_setup_complete = 1;
        } else {
          g_fullscreen_setup_step++;
          needs_redraw = 1;
        }
      } else if (g_fullscreen_setup_step > 0u &&
                 p.x >= 32 && p.x < 132 &&
                 p.y >= (int32_t)button_y && p.y < (int32_t)(button_y + 32u)) {
        g_fullscreen_setup_step--;
        needs_redraw = 1;
      }
    }

    if (!(p.buttons & 1u))
      btn_pressed = 0;
  }

  if (wizard_back)
    kfree(wizard_back);
  g_back = NULL;
  gfx_set_backbuffer(NULL);
}

int desktop_is_setup_complete(void) {
  return g_setup_complete;
}
