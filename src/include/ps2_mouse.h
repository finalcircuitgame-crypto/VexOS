#ifndef VEXOS_PS2_MOUSE_H
#define VEXOS_PS2_MOUSE_H

#include <stdint.h>

/* Initialize the i8042 auxiliary port for polling-mode PS/2 mouse input. */
int ps2_mouse_init(void);

/* Drain pending auxiliary bytes and publish completed mouse packets. */
void ps2_mouse_poll(void);

#endif
