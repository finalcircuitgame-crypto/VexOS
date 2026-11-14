#ifndef VEXOS_SHELL_COMMANDS_H
#define VEXOS_SHELL_COMMANDS_H

#include <stdint.h>

/*
 * Executes terminal built-ins that do not need desktop-owned services.
 * Returns nonzero when the command was recognized and handled.
 */
uint8_t shell_commands_execute(const char *line, uint32_t length,
                              uint32_t tick, int32_t pointer_x,
                              int32_t pointer_y, uint32_t *random_state);

#endif
