#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/include/vfs.h"
#include "../src/kernel/desktop/window_manager.h"
#include "../src/kernel/desktop/apps/files/file_manager.h"
#include "../src/kernel/desktop/apps/notepad/notepad.h"

#define TEST_FILE_MAX 32u
#define TEST_DATA_MAX 4096u

typedef struct {
  char path[VFS_PATH_MAX];
  uint8_t data[TEST_DATA_MAX];
  uint32_t size;
  uint8_t used;
} test_file_t;

static test_file_t g_files[TEST_FILE_MAX];
static uint32_t g_file_slots;
static uint8_t g_volume_writable = 1;
static app_id_t g_focus = APP_SHELL;
static char g_last_opened[VFS_PATH_MAX];

#define CHECK(condition, message) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
    return 1; \
  } \
} while (0)

static uint32_t text_length(const char *text) {
  uint32_t length = 0;
  while (text && text[length]) ++length;
  return length;
}

static int text_equal(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

static int is_root(const char *path) {
  return !path || path[0] == 0 || text_equal(path, "/");
}

static int path_has_prefix(const char *path, const char *prefix) {
  uint32_t length = text_length(prefix);
  return strncmp(path, prefix, length) == 0 &&
         path[length] == '/';
}

static int find_file(const char *path) {
  for (uint32_t i = 0; i < g_file_slots; ++i)
    if (g_files[i].used && text_equal(g_files[i].path, path))
      return (int)i;
  return -1;
}

static int directory_exists(const char *path) {
  if (is_root(path)) return 1;
  for (uint32_t i = 0; i < g_file_slots; ++i)
    if (g_files[i].used && path_has_prefix(g_files[i].path, path))
      return 1;
  return 0;
}

static int add_file(const char *path, const void *data, uint32_t size) {
  if (!path || !path[0] || size > TEST_DATA_MAX ||
      g_file_slots >= TEST_FILE_MAX || find_file(path) >= 0)
    return 0;
  test_file_t *file = &g_files[g_file_slots++];
  memset(file, 0, sizeof(*file));
  strncpy(file->path, path, sizeof(file->path) - 1u);
  if (data && size) memcpy(file->data, data, size);
  file->size = size;
  file->used = 1;
  return 1;
}

static int child_seen_before(const char *directory, uint32_t limit,
                             const char *child) {
  for (uint32_t i = 0; i < limit; ++i) {
    if (!g_files[i].used) continue;
    const char *remainder = g_files[i].path;
    if (!is_root(directory)) {
      if (!path_has_prefix(remainder, directory)) continue;
      remainder += text_length(directory) + 1u;
    }
    char previous[VFS_NAME_MAX];
    uint32_t n = 0;
    while (remainder[n] && remainder[n] != '/' && n + 1u < sizeof(previous)) {
      previous[n] = remainder[n];
      ++n;
    }
    previous[n] = 0;
    if (text_equal(previous, child)) return 1;
  }
  return 0;
}

int vfs_init(void) { return 1; }
int vfs_is_writable(void) { return g_volume_writable != 0; }

int vfs_dir_open(vfs_dir_t *dir, const char *path) {
  if (!dir || dir->is_open || !directory_exists(path)) return 0;
  memset(dir, 0, sizeof(*dir));
  strncpy(dir->path, is_root(path) ? "/" : path, sizeof(dir->path) - 1u);
  dir->is_open = 1;
  return 1;
}

int vfs_dir_read(vfs_dir_t *dir, char *out_name, uint32_t out_cap,
                 vfs_stat_t *out_stat) {
  if (!dir || !dir->is_open || !out_name || !out_cap || !out_stat)
    return -1;

  while (dir->next_index < g_file_slots) {
    uint32_t index = dir->next_index++;
    if (!g_files[index].used) continue;

    const char *remainder = g_files[index].path;
    if (!is_root(dir->path)) {
      if (!path_has_prefix(remainder, dir->path)) continue;
      remainder += text_length(dir->path) + 1u;
    }

    char child[VFS_NAME_MAX];
    uint32_t n = 0;
    while (remainder[n] && remainder[n] != '/' && n + 1u < sizeof(child)) {
      child[n] = remainder[n];
      ++n;
    }
    child[n] = 0;
    if (!n || child_seen_before(dir->path, index, child)) continue;
    if (n + 1u > out_cap) return -1;

    memcpy(out_name, child, n + 1u);
    out_stat->type = remainder[n] == '/' ?
        VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out_stat->size = out_stat->type == VFS_NODE_FILE ?
        g_files[index].size : 0u;
    return 1;
  }
  return 0;
}

int vfs_dir_close(vfs_dir_t *dir) {
  if (!dir || !dir->is_open) return 0;
  dir->is_open = 0;
  return 1;
}

int vfs_create(const char *path) {
  if (!g_volume_writable || !path || find_file(path) >= 0)
    return 0;
  char parent[VFS_PATH_MAX];
  strncpy(parent, path, sizeof(parent) - 1u);
  parent[sizeof(parent) - 1u] = 0;
  uint32_t n = text_length(parent);
  while (n && parent[n - 1u] != '/') --n;
  if (n) parent[n - 1u] = 0;
  else parent[0] = 0;
  if (!directory_exists(parent)) return 0;
  return add_file(path, NULL, 0);
}

int vfs_unlink(const char *path) {
  if (!g_volume_writable) return 0;
  int index = find_file(path);
  if (index < 0) return 0;
  g_files[index].used = 0;
  return 1;
}

int vfs_stat(const char *path, vfs_stat_t *out_stat) {
  if (!path || !out_stat) return 0;
  int index = find_file(path);
  if (index >= 0) {
    out_stat->type = VFS_NODE_FILE;
    out_stat->size = g_files[index].size;
    return 1;
  }
  if (directory_exists(path)) {
    out_stat->type = VFS_NODE_DIRECTORY;
    out_stat->size = 0;
    return 1;
  }
  return 0;
}

int vfs_read(const char *path, void *out, uint32_t max_len,
             uint32_t *out_len) {
  if (out_len) *out_len = 0;
  int index = find_file(path);
  if (index < 0 || (!out && max_len)) return 0;
  uint32_t count = g_files[index].size < max_len ?
      g_files[index].size : max_len;
  if (count) memcpy(out, g_files[index].data, count);
  if (out_len) *out_len = count;
  return 1;
}

int vfs_write(const char *path, const void *data, uint32_t len) {
  if (!g_volume_writable || (!data && len) || len > TEST_DATA_MAX) return 0;
  int index = find_file(path);
  if (index < 0) {
    if (!add_file(path, data, len)) return 0;
    return 1;
  }
  if (len) memcpy(g_files[index].data, data, len);
  g_files[index].size = len;
  return 1;
}

void wm_raise(app_id_t app) { g_focus = app; }
app_id_t wm_get_focus(void) { return g_focus; }

void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint32_t radius, uint32_t color,
                          uint8_t alpha) {
  (void)x; (void)y; (void)width; (void)height; (void)radius;
  (void)color; (void)alpha;
}
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color) {
  (void)x; (void)y; (void)width; (void)height; (void)color;
}
void gfx_draw_string(uint32_t x, uint32_t y, const char *text,
                     uint32_t foreground) {
  (void)x; (void)y; (void)text; (void)foreground;
}

static int record_open(const char *path) {
  strncpy(g_last_opened, path, sizeof(g_last_opened) - 1u);
  g_last_opened[sizeof(g_last_opened) - 1u] = 0;
  return 1;
}

static int click_at(const desktop_window_t *window, int32_t x, int32_t y,
                    uint32_t tick, file_manager_open_file_fn open_file) {
  return (int)file_manager_handle_click(window->x + x, window->y + y,
                                        tick, open_file);
}

int main(void) {
  static const char alpha[] = "Alpha document";
  static const char readme[] = "Notes inside docs";
  static const char zeta[] = "Zeta document";

  if (!add_file("docs/readme.txt", readme, sizeof(readme) - 1u) ||
      !add_file("docs/nested/inside.txt", "nested", 6u) ||
      !add_file("alpha.txt", alpha, sizeof(alpha) - 1u) ||
      !add_file("empty.txt", NULL, 0u) ||
      !add_file("zeta.txt", zeta, sizeof(zeta) - 1u)) {
    fprintf(stderr, "FAIL: test fixture setup\n");
    return 1;
  }

  file_manager_init(1280u, 800u);
  file_manager_open();
  desktop_window_t *window = file_manager_window();
  CHECK(file_manager_is_open(), "Files window opens");
  CHECK(g_focus == APP_EXPLORER, "Files receives focus");

  /* Root order is alpha.txt, docs/, empty.txt, zeta.txt. Open docs from the
     explicit toolbar button, then open readme.txt by double-click. */
  click_at(window, 170, 144 + 28 + 5, 100u, record_open);
  click_at(window, 170, 144 + 28 + 5, 101u, record_open);
  CHECK(text_equal(g_last_opened, ""), "folders do not invoke file callback");
  CHECK(g_focus == APP_EXPLORER, "opening a folder keeps Files focused");

  /* Readme is the second item in docs after the nested folder. */
  click_at(window, 170, 144 + 28 + 5, 200u, record_open);
  click_at(window, 170, 144 + 28 + 5, 201u, record_open);
  CHECK(text_equal(g_last_opened, "docs/readme.txt"),
        "double-click opens the selected file");
  CHECK(g_focus == APP_EXPLORER, "file callback can change focus");

  /* Return to root using the window's back control. */
  click_at(window, window->w - 43, 10, 220u, record_open);
  CHECK(g_focus == APP_EXPLORER, "back navigation keeps Files focused");

  /* Select alpha.txt, then use the visible Open toolbar action. */
  click_at(window, 170, 144 + 5, 300u, record_open);
  click_at(window, 160 + (window->w - 172) - 266 + 3, 80 + 3,
           301u, record_open);
  CHECK(text_equal(g_last_opened, "alpha.txt"),
        "Open button opens selected file");

  /* New creates a file and immediately opens it for editing. */
  g_last_opened[0] = 0;
  uint32_t before_create = g_file_slots;
  click_at(window, 160 + (window->w - 172) - 204 + 3, 80 + 3,
           320u, record_open);
  CHECK(find_file("New File.txt") >= 0, "New creates an empty file");
  CHECK(g_file_slots == before_create + 1u, "New creates one file only");
  CHECK(text_equal(g_last_opened, "New File.txt"),
        "New opens the file in the editor callback");
  CHECK(g_focus == APP_NOTEPAD, "new document retains editor focus");

  /* Delete requires the two-step confirmation and removes the selected file. */
  click_at(window, 170, 144 + 5, 340u, record_open);
  click_at(window, 160 + (window->w - 172) - 130 + 3, 80 + 3,
           341u, record_open);
  CHECK(find_file("alpha.txt") >= 0, "first Delete click only arms confirmation");
  click_at(window, 160 + (window->w - 172) - 130 + 3, 80 + 3,
           342u, record_open);
  CHECK(find_file("alpha.txt") < 0, "confirmed Delete removes the selected file");

  /* Read-only media should explain/disable mutations rather than silently fail. */
  g_volume_writable = 0;
  file_manager_open();
  uint32_t before_read_only_create = g_file_slots;
  click_at(window, 160 + (window->w - 172) - 204 + 3, 80 + 3,
           400u, record_open);
  CHECK(g_file_slots == before_read_only_create,
        "New is disabled on read-only volumes");

  /* Notepad loads existing and empty documents, preserves edits on a failed
     load, saves the current document, and writes before switching documents. */
  g_volume_writable = 1;
  notepad_init(1280u, 800u);
  CHECK(notepad_open_file("zeta.txt"), "Notepad opens a text file");
  CHECK(notepad_text_length() == sizeof(zeta) - 1u &&
        memcmp(notepad_text(), zeta, sizeof(zeta) - 1u) == 0,
        "Notepad loads file contents");
  CHECK(notepad_handle_char('!'), "Notepad accepts typed text");
  CHECK(notepad_open_file("empty.txt"), "Notepad opens an empty file");
  int zeta_index = find_file("zeta.txt");
  CHECK(zeta_index >= 0, "edited document still exists");
  CHECK(g_files[zeta_index].size == sizeof(zeta),
        "switching documents saves previous edits");
  CHECK(g_files[zeta_index].data[g_files[zeta_index].size - 1u] == '!',
        "saved document contains typed character");
  CHECK(notepad_text_length() == 0, "empty file opens as an empty document");
  CHECK(notepad_handle_char('x'), "empty document accepts text");
  desktop_window_t *editor = notepad_window();
  CHECK(notepad_handle_click(editor->x + editor->w - 80,
                             editor->y + 10) == NOTEPAD_CLICK_SAVE_OK,
        "Notepad save action writes through VFS");
  int empty_index = find_file("empty.txt");
  CHECK(empty_index >= 0 && g_files[empty_index].size == 1u &&
        g_files[empty_index].data[0] == 'x',
        "Notepad save persists edited empty file");

  puts("PASS: Files navigation, open, create, delete, read-only state, and Notepad load/save");
  return 0;
}
