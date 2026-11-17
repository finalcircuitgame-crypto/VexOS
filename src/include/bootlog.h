#ifndef VEXOS_BOOTLOG_H
#define VEXOS_BOOTLOG_H

#include <stdint.h>

/* Keep the most recent serial output in RAM until persistent storage is ready. */
#define BOOTLOG_MAX_BYTES (32u * 1024u)

/* Called by serial_print(); safe before heap, VFS, or storage initialization. */
void bootlog_append(const char *text);

/* Install the UEFI Runtime Services SetVariable entry point from BootInfo. */
void bootlog_set_runtime(uint64_t set_variable_address);

/* Save a bounded checkpoint to nonvolatile UEFI variables after ExitBootServices. */
int bootlog_checkpoint(void);

/* Probe for a mounted writable VexOS volume. Never formats or partitions media. */
int bootlog_prepare(void);

/* Replace /boot.log with the current log snapshot when writable SFS storage exists. */
int bootlog_flush(void);

#endif
