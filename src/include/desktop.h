#ifndef DESKTOP_H
#define DESKTOP_H

#include "bootinfo.h"

void desktop_init(BootInfo *bootInfo);
void desktop_tick(void);

// Fullscreen setup wizard (boot-to-wizard mode)
void desktop_run_fullscreen_wizard(BootInfo *bootInfo);
int desktop_is_setup_complete(void);

void desktop_draw_frame(const uint32_t* src, uint32_t w, uint32_t h);
void desktop_set_fullscreen_app_active(int active);

#endif
