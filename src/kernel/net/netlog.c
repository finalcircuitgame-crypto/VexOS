#include "netlog.h"

#include "netdev.h"
#include "arp.h"
#include "ipv4.h"
#include "udp.h"
#include "interrupts.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);\nvoid ConsoleDebugPrint(const char *str);

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
#ifndef NETLOG_DESTINATION_IPV4
#define NETLOG_DESTINATION_IPV4 0x0A000202u
#endif
#ifndef NETLOG_DESTINATION_IP
#define NETLOG_DESTINATION_IP ((uint32_t)NETLOG_DESTINATION_IPV4)
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
static uint8_t g_reported_link_wait;
static uint8_t g_reported_udp_reject;
static uint8_t g_reported_udp_accept;

static void netlog_append_text(char *buffer, size_t capacity, size_t *used, const char *text) {
    while (*text && *used + 1u < capacity) buffer[(*used)++] = *text++;
    buffer[*used] = 0;
}

static void netlog_append_counter(char *buffer, size_t capacity, size_t *used,
                                  const char *label, uint64_t value) {
    char digits[21];
    size_t length = 0;
    do { digits[length++] = (char)('0' + (value % 10u)); value /= 10u; }
    while (value != 0u && length < sizeof(digits));
    netlog_append_text(buffer, capacity, used, label);
    while (length && *used + 1u < capacity) buffer[(*used)++] = digits[--length];
    if (*used + 1u < capacity) buffer[(*used)++] = '\n';
    buffer[*used] = 0;
}

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
            /* Preserve the newest diagnostics by evicting the oldest byte. */
            g_tail = (g_tail + 1u) % NETLOG_QUEUE_BYTES;
            --g_used;
            ++g_dropped;
            g_drop_notice_pending = 1;
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
    serial_print("[NETLOG] UDP transport ready; remote logging enabled\n");
    return 1;
}

static void netlog_flush_internal(int force) {
    if (!g_ready || !g_device)
        return;

    if (!g_device->link_up) {
        if (!g_reported_link_wait) {
            g_reported_link_wait = 1u;
            serial_print("[NETLOG] Waiting for registered interface link before sending\n");
        }
        return;
    }

    const uint64_t now = Timer_Ticks();
    if (!force && now - g_last_send < NETLOG_SEND_INTERVAL_TICKS)
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

    /*
     * udp_send() succeeds when ARP accepts/queues the IPv4 packet. The service
     * pump keeps polling the NIC until the ARP reply releases that queued frame.
     */
    if (!udp_send(g_device, NETLOG_DESTINATION_IP, NETLOG_SOURCE_PORT,
                  NETLOG_DESTINATION_PORT, packet, length)) {
        if (!g_reported_udp_reject) {
            udp_stats_t udp_stats = {0};
            ipv4_stats_t ipv4_stats = {0};
            arp_stats_t arp_stats = {0};
            udp_get_stats(&udp_stats);
            ipv4_get_stats(&ipv4_stats);
            arp_get_stats(&arp_stats);

            g_reported_udp_reject = 1u;
            char diagnostic[640] = {0};
            size_t used = 0;
            netlog_append_text(diagnostic, sizeof(diagnostic), &used,
                "[NETLOG] UDP SEND REJECTED - TX DIAGNOSTICS\n"
                "Cumulative counters at first rejection:\n");
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "udp_drop=", udp_stats.tx_dropped);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "ipv4_drop=", ipv4_stats.tx_dropped);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_requests=", arp_stats.tx_requests);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_tx_fail=", arp_stats.tx_failures);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_pending=", arp_stats.pending_queued);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_pending_drop=", arp_stats.pending_dropped);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_timeouts=", arp_stats.pending_timeouts);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_rx_replies=", arp_stats.rx_replies);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "arp_pending_sent=", arp_stats.pending_sent);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "nic_tx=", g_device->tx_packets);
            netlog_append_counter(diagnostic, sizeof(diagnostic), &used, "nic_drop=", g_device->tx_dropped);
            ConsoleDebugPrint(diagnostic);
            serial_print(diagnostic);
        }
        return;
    }

    if (!g_reported_udp_accept) {
        g_reported_udp_accept = 1u;
        serial_print("[NETLOG] First UDP payload accepted by IP/ARP stack\n");
    }

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

void netlog_flush_now(void) {
    netlog_flush_internal(1);
}

void netlog_poll(void) {
    netlog_flush_internal(0);
}

void netlog_service(void) {
    /* Safe before PIT_Init: the tick-based ARP expiry clock has not advanced. */
    netdev_poll_all(32u);
    arp_poll();
    netlog_flush_now();
}
