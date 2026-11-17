#ifndef VEXOS_ARP_H
#define VEXOS_ARP_H

#include "netdev.h"

#include <stddef.h>
#include <stdint.h>

#define ARP_CACHE_ENTRIES             32u
#define ARP_PENDING_PACKETS             8u
#define ARP_MAX_IPV4_PACKET_SIZE     1500u
#define ARP_CACHE_TTL_TICKS         60000ull
#define ARP_PENDING_TIMEOUT_TICKS    5000ull

/* IPv4 addresses are represented in host integers with network-order octets. */
#define ARP_IPV4(a, b, c, d) \
    ((((uint32_t)(a) & 0xFFu) << 24) | \
     (((uint32_t)(b) & 0xFFu) << 16) | \
     (((uint32_t)(c) & 0xFFu) << 8) | \
     ((uint32_t)(d) & 0xFFu))

typedef struct {
    uint64_t rx_requests;
    uint64_t rx_replies;
    uint64_t rx_malformed;
    uint64_t tx_requests;
    uint64_t tx_replies;
    uint64_t tx_failures;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_conflicts;
    uint64_t cache_full;
    uint64_t cache_expired;
    uint64_t pending_queued;
    uint64_t pending_sent;
    uint64_t pending_timeouts;
    uint64_t pending_dropped;
} arp_stats_t;

/* Register the ARP EtherType handler and clear the bounded cache/queue. */
int arp_init(void);

/* Look up a live IPv4-to-MAC entry for one interface. */
int arp_cache_lookup(net_device_t *device, uint32_t ipv4_address,
                     uint8_t mac_out[6]);

/*
 * Send a complete IPv4 packet through Ethernet. ipv4_packet must contain one
 * complete IPv4 datagram (its total-length field must equal packet_length).
 * If the next-hop MAC is unknown, a bounded copy is queued and ARP is sent.
 */
int arp_send_ipv4(net_device_t *device, uint32_t next_hop,
                  const uint8_t *ipv4_packet, size_t packet_length);

/* Expire stale cache entries and unresolved queued packets. Call from polling context. */
void arp_poll(void);

void arp_get_stats(arp_stats_t *out_stats);

#endif
