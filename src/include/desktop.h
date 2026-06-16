#ifndef DESKTOP_H
#define DESKTOP_H

#include "bootinfo.h"

void desktop_init(BootInfo *bootInfo);
void desktop_tick(void);

// Fullscreen setup wizard (boot-to-wizard mode)
void desktop_run_fullscreen_wizard(BootInfo *bootInfo);
int desktop_is_setup_complete(void);

#endif
