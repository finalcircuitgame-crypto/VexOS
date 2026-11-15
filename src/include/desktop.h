#ifndef DESKTOP_H
#define DESKTOP_H

#include "bootinfo.h"

void desktop_init(BootInfo *bootInfo);
void desktop_tick(void);

// Fullscreen setup wizard (boot-to-wizard mode)
void desktop_run_fullscreen_wizard(BootInfo *bootInfo);
int desktop_is_setup_complete(void);

void desktop_draw_frame(const uint32_t* src, uint32_t w, uint32_t h);
int desktop_doom_app_ready(void);
int desktop_doom_input_active(void);
int desktop_doom_mouse_input_active(void);
int desktop_doom_close_requested(void);
void desktop_user_app_exited(int faulted);

#endif
