#ifndef VEXOS_NETLOG_H
#define VEXOS_NETLOG_H

#include <stddef.h>
#include <stdint.h>

/* Development-only UDP log transport. Calls from serial_print are bounded. */
#define NETLOG_UDP_PORT 5140u
#define NETLOG_QUEUE_BYTES 8192u
#define NETLOG_MAX_PAYLOAD 1000u

void netlog_append(const char *text);
int netlog_init(void);
void netlog_poll(void);
void netlog_flush_now(void);
void netlog_service(void);

#endif
