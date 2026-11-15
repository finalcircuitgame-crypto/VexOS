#ifndef VEXOS_APP_REGISTRY_H
#define VEXOS_APP_REGISTRY_H

#include "window_manager.h"

/* Built-in app metadata registry. App IDs remain stable across UI surfaces. */
typedef struct {
  app_id_t id;
  const char *name;
} desktop_app_info_t;

void app_registry_init(void);
const desktop_app_info_t *app_registry_get(app_id_t id);
const char *app_registry_name(app_id_t id);

#endif
