#include "udp.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    udp_receive_fn handler;
} udp_port_entry_t;

static udp_port_entry_t g_ports[UDP_MAX_PORT_HANDLERS];
static size_t g_port_count;
static udp_stats_t g_stats;

static uint16_t udp_read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void udp_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void udp_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        out[i] = in[i];
}

static uint32_t udp_checksum_add(uint32_t sum, const uint8_t *bytes,
                                 size_t length) {
    size_t i = 0;
    while (i + 1u < length) {
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1u];
        i += 2u;
    }
    if (i < length)
        sum += (uint32_t)bytes[i] << 8;
    return sum;
}

static uint16_t udp_checksum_finish(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t udp_pseudo_sum(uint32_t sum, uint32_t source,
                               uint32_t destination, uint16_t length) {
    uint8_t pseudo[12] = {
        (uint8_t)(source >> 24), (uint8_t)(source >> 16),
        (uint8_t)(source >> 8), (uint8_t)source,
        (uint8_t)(destination >> 24), (uint8_t)(destination >> 16),
        (uint8_t)(destination >> 8), (uint8_t)destination,
        0u, IPV4_PROTOCOL_UDP,
        (uint8_t)(length >> 8), (uint8_t)length
    };
    return udp_checksum_add(sum, pseudo, sizeof(pseudo));
}

static int udp_checksum_valid(const ipv4_packet_info_t *info,
                              const uint8_t *datagram, size_t length) {
    /* IPv4 permits a zero UDP checksum to mean checksum disabled. */
    if (udp_read_be16(datagram + 6u) == 0u)
        return 1;
    uint32_t sum = udp_pseudo_sum(0u, info->source, info->destination,
                                 (uint16_t)length);
    sum = udp_checksum_add(sum, datagram, length);
    return udp_checksum_finish(sum) == 0u;
}

static void udp_receive(net_device_t *device,
                        const ipv4_packet_info_t *info,
                        const uint8_t *datagram,
                        size_t datagram_length) {
    if (!device || !info || !datagram || datagram_length < UDP_HEADER_SIZE ||
        datagram_length > UINT16_MAX) {
        ++g_stats.rx_malformed;
        return;
    }

    const uint16_t length = udp_read_be16(datagram + 4u);
    if (length < UDP_HEADER_SIZE || length != datagram_length) {
        ++g_stats.rx_malformed;
        return;
    }
    if (!udp_checksum_valid(info, datagram, length)) {
        ++g_stats.rx_bad_checksum;
        return;
    }

    ++g_stats.rx_datagrams;
    const uint16_t source_port = udp_read_be16(datagram);
    const uint16_t destination_port = udp_read_be16(datagram + 2u);
    for (size_t i = 0; i < g_port_count; ++i) {
        if (g_ports[i].port == destination_port) {
            ++g_stats.rx_delivered;
            g_ports[i].handler(device, info, source_port, destination_port,
                               datagram + UDP_HEADER_SIZE,
                               length - UDP_HEADER_SIZE);
            return;
        }
    }
    ++g_stats.rx_no_port;
}

int udp_init(void) {
    for (size_t i = 0; i < UDP_MAX_PORT_HANDLERS; ++i)
        g_ports[i] = (udp_port_entry_t){0};
    g_port_count = 0;
    g_stats = (udp_stats_t){0};
    return ipv4_register_protocol(IPV4_PROTOCOL_UDP, udp_receive);
}

int udp_bind(uint16_t port, udp_receive_fn handler) {
    if (port == 0u || !handler || g_port_count >= UDP_MAX_PORT_HANDLERS)
        return 0;
    for (size_t i = 0; i < g_port_count; ++i) {
        if (g_ports[i].port == port)
            return 0;
    }
    g_ports[g_port_count].port = port;
    g_ports[g_port_count].handler = handler;
    ++g_port_count;
    return 1;
}

int udp_unbind(uint16_t port) {
    for (size_t i = 0; i < g_port_count; ++i) {
        if (g_ports[i].port == port) {
            for (size_t j = i + 1u; j < g_port_count; ++j)
                g_ports[j - 1u] = g_ports[j];
            --g_port_count;
            g_ports[g_port_count] = (udp_port_entry_t){0};
            return 1;
        }
    }
    return 0;
}

int udp_send(net_device_t *device, uint32_t destination,
             uint16_t source_port, uint16_t destination_port,
             const void *payload, size_t payload_length) {
    ipv4_config_t config;
    if (!device || source_port == 0u || destination_port == 0u ||
        (!payload && payload_length != 0u) ||
        payload_length > IPV4_MAX_PACKET_SIZE - IPV4_MIN_HEADER_SIZE -
                         UDP_HEADER_SIZE ||
        !ipv4_get_config(device, &config) ||
        payload_length + UDP_HEADER_SIZE + IPV4_MIN_HEADER_SIZE > device->mtu) {
        ++g_stats.tx_dropped;
        return 0;
    }

    const size_t datagram_length = UDP_HEADER_SIZE + payload_length;
    uint8_t datagram[IPV4_MAX_PACKET_SIZE - IPV4_MIN_HEADER_SIZE] = {0};
    udp_write_be16(datagram, source_port);
    udp_write_be16(datagram + 2u, destination_port);
    udp_write_be16(datagram + 4u, (uint16_t)datagram_length);
    if (payload_length)
        udp_copy(datagram + UDP_HEADER_SIZE, payload, payload_length);

    uint32_t sum = udp_pseudo_sum(0u, config.address, destination,
                                  (uint16_t)datagram_length);
    sum = udp_checksum_add(sum, datagram, datagram_length);
    uint16_t checksum = udp_checksum_finish(sum);
    if (checksum == 0u)
        checksum = 0xFFFFu;
    udp_write_be16(datagram + 6u, checksum);

    if (!ipv4_send(device, destination, IPV4_PROTOCOL_UDP,
                   datagram, datagram_length)) {
        ++g_stats.tx_dropped;
        return 0;
    }
    ++g_stats.tx_datagrams;
    return 1;
}

void udp_get_stats(udp_stats_t *out_stats) {
    if (out_stats)
        *out_stats = g_stats;
}
