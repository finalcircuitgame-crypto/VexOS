#ifndef VEXOS_BOOTLOG_H
#define VEXOS_BOOTLOG_H

#include <stdint.h>

/* Keep the most recent boot/serial output in RAM until storage is ready. */
#define BOOTLOG_MAX_BYTES (32u * 1024u)

/* Called by serial_print(); safe before heap, VFS, or storage initialization. */
void bootlog_append(const char *text);

/* Probe for a mounted writable VexOS volume. Never formats or partitions media. */
int bootlog_prepare(void);

/* Replace /boot.log with the current log snapshot when writable storage exists. */
int bootlog_flush(void);

#endif
