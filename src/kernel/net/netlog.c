#include "netlog.h"

#include "netdev.h"
#include "arp.h"
#include "ipv4.h"
#include "udp.h"
#include "interrupts.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);

/*
 * QEMU user-mode networking defaults:
 *   guest = 10.0.2.15/24, gateway/host = 10.0.2.2
 * Override these constants for a physical LAN build. DHCP is not implemented.
 */
#ifndef NETLOG_LOCAL_IP
#define NETLOG_LOCAL_IP      ARP_IPV4(10, 0, 2, 15)
#endif
#ifndef NETLOG_NETMASK
#define NETLOG_NETMASK       ARP_IPV4(255, 255, 255, 0)
#endif
#ifndef NETLOG_GATEWAY
#define NETLOG_GATEWAY       ARP_IPV4(10, 0, 2, 2)
#endif
#ifndef NETLOG_DESTINATION_IP
#define NETLOG_DESTINATION_IP ARP_IPV4(10, 0, 2, 2)
#endif
#ifndef NETLOG_DESTINATION_PORT
#define NETLOG_DESTINATION_PORT NETLOG_UDP_PORT
#endif

#define NETLOG_SOURCE_PORT 5141u
#define NETLOG_SEND_INTERVAL_TICKS 100u

static char g_queue[NETLOG_QUEUE_BYTES];
static uint32_t g_head;
static uint32_t g_tail;
static uint32_t g_used;
static uint64_t g_dropped;
static uint64_t g_last_send;
static net_device_t *g_device;
static uint8_t g_ready;
static uint8_t g_drop_notice_pending;

static uint64_t netlog_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void netlog_irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

void netlog_append(const char *text) {
    if (!text)
        return;

    /* This function is called inside serial_print's existing IRQ-safe section. */
    while (*text) {
        if (g_used == NETLOG_QUEUE_BYTES) {
            ++g_dropped;
            g_drop_notice_pending = 1;
            ++text;
            continue;
        }
        g_queue[g_head] = *text++;
        g_head = (g_head + 1u) % NETLOG_QUEUE_BYTES;
        ++g_used;
    }
}

int netlog_init(void) {
    if (g_ready)
        return 1;

    if (netdev_count() == 0u) {
        serial_print("[NETLOG] No network interface; remote logging remains queued\n");
        return 0;
    }

    g_device = netdev_get(0u);
    if (!g_device || !ipv4_configure(g_device, NETLOG_LOCAL_IP,
                                     NETLOG_NETMASK, NETLOG_GATEWAY)) {
        serial_print("[NETLOG] Static IPv4 configuration failed; remote logging disabled\n");
        g_device = NULL;
        return 0;
    }

    g_ready = 1;
    serial_print("[NETLOG] UDP transport ready; destination 10.0.2.2:5140 (QEMU user networking defaults)\n");
    return 1;
}

void netlog_poll(void) {
    if (!g_ready || !g_device || !g_device->link_up)
        return;

    const uint64_t now = Timer_Ticks();
    if (now - g_last_send < NETLOG_SEND_INTERVAL_TICKS)
        return;

    uint8_t packet[NETLOG_MAX_PAYLOAD];
    size_t length = 0;
    uint8_t send_drop_notice = 0;
    uint64_t flags = netlog_irq_save();

    if (g_drop_notice_pending) {
        static const char notice[] = "[NETLOG] queue overflow: older log bytes were dropped\n";
        while (length < sizeof(notice) - 1u) {
            packet[length] = (uint8_t)notice[length];
            ++length;
        }
        send_drop_notice = 1;
    } else {
        while (length < NETLOG_MAX_PAYLOAD && length < g_used) {
            packet[length] = (uint8_t)g_queue[(g_tail + (uint32_t)length) %
                                               NETLOG_QUEUE_BYTES];
            ++length;
        }
    }
    netlog_irq_restore(flags);

    if (length == 0u)
        return;

    if (!udp_send(g_device, NETLOG_DESTINATION_IP, NETLOG_SOURCE_PORT,
                  NETLOG_DESTINATION_PORT, packet, length))
        return;

    flags = netlog_irq_save();
    if (send_drop_notice) {
        g_drop_notice_pending = 0;
    } else {
        g_tail = (g_tail + (uint32_t)length) % NETLOG_QUEUE_BYTES;
        g_used -= (uint32_t)length;
    }
    netlog_irq_restore(flags);
    g_last_send = now;
}
