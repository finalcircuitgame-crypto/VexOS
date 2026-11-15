#include "app_registry.h"

static const desktop_app_info_t g_apps[APP_COUNT] = {
  { APP_SHELL, "Terminal" },
  { APP_EXPLORER, "Files" },
  { APP_VIEWER, "Viewer" },
  { APP_NOTEPAD, "Notepad" },
  { APP_SETUP, "Setup" }
};

static uint8_t g_registry_ready;

void app_registry_init(void) {
  g_registry_ready = 1;
}

const desktop_app_info_t *app_registry_get(app_id_t id) {
  if (!g_registry_ready || (uint32_t)id >= APP_COUNT)
    return 0;
  if (g_apps[id].id != id)
    return 0;
  return &g_apps[id];
}

const char *app_registry_name(app_id_t id) {
  const desktop_app_info_t *app = app_registry_get(id);
  return app ? app->name : "Unknown";
}
