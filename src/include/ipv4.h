#ifndef VEXOS_IPV4_H
#define VEXOS_IPV4_H

#include "netdev.h"

#include <stddef.h>
#include <stdint.h>

#define IPV4_MAX_INTERFACES          NETDEV_MAX_DEVICES
#define IPV4_MAX_PROTOCOL_HANDLERS   16u
#define IPV4_MAX_PACKET_SIZE       1500u
#define IPV4_MIN_HEADER_SIZE          20u
#define IPV4_DEFAULT_TTL              64u
#define IPV4_FLAG_DONT_FRAGMENT   0x4000u
#define IPV4_PROTOCOL_ICMP             1u
#define IPV4_PROTOCOL_TCP              6u
#define IPV4_PROTOCOL_UDP             17u

typedef struct {
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
} ipv4_config_t;

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint16_t identification;
    uint16_t fragment_field;
    uint16_t total_length;
    uint8_t ttl;
    uint8_t protocol;
    uint8_t header_length;
} ipv4_packet_info_t;

typedef struct {
    uint64_t rx_packets;
    uint64_t rx_delivered;
    uint64_t rx_malformed;
    uint64_t rx_bad_checksum;
    uint64_t rx_fragments;
    uint64_t rx_nonlocal;
    uint64_t rx_no_handler;
    uint64_t tx_packets;
    uint64_t tx_dropped;
} ipv4_stats_t;

/*
 * Protocol handlers run synchronously inside the Ethernet receive callback.
 * info and payload are borrowed from the NIC receive buffer and are valid only
 * until the handler returns. Copy anything that must outlive the callback.
 */
typedef void (*ipv4_protocol_handler_fn)(
    net_device_t *device,
    const ipv4_packet_info_t *info,
    const uint8_t *payload,
    size_t payload_length);

/* Register IPv4 with Ethernet and reset the bounded interface/handler tables. */
int ipv4_init(void);

/* Configure one Ethernet interface. A zero gateway means on-link only. */
int ipv4_configure(net_device_t *device, uint32_t address,
                   uint32_t netmask, uint32_t gateway);
int ipv4_get_config(net_device_t *device, ipv4_config_t *out_config);
int ipv4_unconfigure(net_device_t *device);

/* Register one synchronous handler for an IPv4 protocol number. */
int ipv4_register_protocol(uint8_t protocol,
                           ipv4_protocol_handler_fn handler);

/* Build and send one IPv4 datagram. Fragmentation and multicast are unsupported. */
int ipv4_send(net_device_t *device, uint32_t destination,
              uint8_t protocol, const void *payload, size_t payload_length);

void ipv4_get_stats(ipv4_stats_t *out_stats);

#endif
