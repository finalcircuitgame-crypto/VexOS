#include "ipv4.h"

#include "arp.h"
#include "ethernet.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    net_device_t *device;
    ipv4_config_t config;
    uint8_t valid;
} ipv4_interface_t;

typedef struct {
    uint8_t protocol;
    ipv4_protocol_handler_fn handler;
} ipv4_protocol_entry_t;

static ipv4_interface_t g_interfaces[IPV4_MAX_INTERFACES];
static ipv4_protocol_entry_t g_protocols[IPV4_MAX_PROTOCOL_HANDLERS];
static size_t g_protocol_count;
static ipv4_stats_t g_stats;
static uint16_t g_next_identification;

static uint16_t ipv4_read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static uint32_t ipv4_read_address(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void ipv4_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void ipv4_write_address(uint8_t *bytes, uint32_t address) {
    bytes[0] = (uint8_t)(address >> 24);
    bytes[1] = (uint8_t)(address >> 16);
    bytes[2] = (uint8_t)(address >> 8);
    bytes[3] = (uint8_t)address;
}

static void ipv4_copy_bytes(void *destination, const void *source,
                            size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        out[i] = in[i];
}

/* Returns zero for a valid header when the checksum field is included. */
static uint16_t ipv4_checksum(const uint8_t *bytes, size_t length) {
    uint32_t sum = 0;
    size_t i = 0;

    while (i + 1u < length) {
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1u];
        i += 2u;
    }
    if (i < length)
        sum += (uint32_t)bytes[i] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static int ipv4_is_unicast(uint32_t address) {
    const uint8_t first = (uint8_t)(address >> 24);
    return address != 0 && address != 0xFFFFFFFFu &&
           first != 127u && first >= 1u && first <= 223u;
}

static int ipv4_mask_is_contiguous(uint32_t mask) {
    if (mask == 0u || mask > 0xFFFFFFFCu)
        return 0; /* This milestone supports conventional /1 through /30 LANs. */
    uint32_t inverse = ~mask;
    return (inverse & (inverse + 1u)) == 0u;
}

static int ipv4_config_is_valid(uint32_t address, uint32_t netmask,
                                uint32_t gateway) {
    if (!ipv4_is_unicast(address) || !ipv4_mask_is_contiguous(netmask))
        return 0;

    const uint32_t host = address & ~netmask;
    if (host == 0u || host == ~netmask)
        return 0; /* Interface address cannot be the subnet or broadcast. */

    if (gateway != 0u) {
        if (!ipv4_is_unicast(gateway) || gateway == address ||
            (gateway & netmask) != (address & netmask))
            return 0;
        const uint32_t gateway_host = gateway & ~netmask;
        if (gateway_host == 0u || gateway_host == ~netmask)
            return 0;
    }
    return 1;
}

static ipv4_interface_t *ipv4_find_interface(net_device_t *device) {
    if (!device)
        return 0;
    for (size_t i = 0; i < IPV4_MAX_INTERFACES; ++i) {
        if (g_interfaces[i].valid && g_interfaces[i].device == device)
            return &g_interfaces[i];
    }
    return 0;
}

static int ipv4_is_broadcast(const ipv4_config_t *config,
                             uint32_t address) {
    if (address == 0xFFFFFFFFu)
        return 1;
    const uint32_t subnet_broadcast =
        (config->address & config->netmask) | ~config->netmask;
    return address == subnet_broadcast;
}

static void ipv4_receive(net_device_t *device,
                         const uint8_t destination_mac[6],
                         const uint8_t source_mac[6],
                         const uint8_t *packet,
                         size_t packet_length) {
    (void)destination_mac;
    (void)source_mac;

    if (!device || !packet || packet_length < IPV4_MIN_HEADER_SIZE) {
        ++g_stats.rx_malformed;
        return;
    }

    ipv4_interface_t *interface = ipv4_find_interface(device);
    if (!interface) {
        ++g_stats.rx_nonlocal;
        return;
    }

    const uint8_t version = (uint8_t)(packet[0] >> 4);
    const size_t header_length = (size_t)(packet[0] & 0x0Fu) * 4u;
    const uint16_t total_length = ipv4_read_be16(packet + 2u);
    const uint16_t fragment_field = ipv4_read_be16(packet + 6u);

    if (version != 4u || header_length < IPV4_MIN_HEADER_SIZE ||
        header_length > 60u || header_length > packet_length ||
        total_length < header_length || total_length > packet_length ||
        total_length > device->mtu || total_length > IPV4_MAX_PACKET_SIZE ||
        packet[8] == 0u || (fragment_field & 0x8000u) != 0u) {
        ++g_stats.rx_malformed;
        return;
    }

    if (ipv4_checksum(packet, header_length) != 0u) {
        ++g_stats.rx_bad_checksum;
        return;
    }

    /*
     * This milestone deliberately does not reassemble fragments. Accept DF,
     * but drop MF or any non-zero fragment offset rather than delivering a
     * partial transport payload to a protocol handler.
     */
    if ((fragment_field & 0x2000u) != 0u ||
        (fragment_field & 0x1FFFu) != 0u) {
        ++g_stats.rx_fragments;
        return;
    }

    const uint32_t source = ipv4_read_address(packet + 12u);
    const uint32_t destination = ipv4_read_address(packet + 16u);
    if (source != 0u && !ipv4_is_unicast(source)) {
        ++g_stats.rx_malformed;
        return;
    }

    if (destination != interface->config.address &&
        !ipv4_is_broadcast(&interface->config, destination)) {
        ++g_stats.rx_nonlocal;
        return;
    }

    ipv4_protocol_handler_fn handler = 0;
    for (size_t i = 0; i < g_protocol_count; ++i) {
        if (g_protocols[i].protocol == packet[9]) {
            handler = g_protocols[i].handler;
            break;
        }
    }

    ++g_stats.rx_packets;
    if (!handler) {
        ++g_stats.rx_no_handler;
        return;
    }

    ipv4_packet_info_t info = {
        .source = source,
        .destination = destination,
        .identification = ipv4_read_be16(packet + 4u),
        .fragment_field = fragment_field,
        .total_length = total_length,
        .ttl = packet[8],
        .protocol = packet[9],
        .header_length = (uint8_t)header_length
    };

    ++g_stats.rx_delivered;
    handler(device, &info, packet + header_length,
            (size_t)total_length - header_length);
}

int ipv4_init(void) {
    for (size_t i = 0; i < IPV4_MAX_INTERFACES; ++i)
        g_interfaces[i] = (ipv4_interface_t){0};
    for (size_t i = 0; i < IPV4_MAX_PROTOCOL_HANDLERS; ++i)
        g_protocols[i] = (ipv4_protocol_entry_t){0};
    g_protocol_count = 0;
    g_stats = (ipv4_stats_t){0};
    g_next_identification = 1u;
    return ethernet_register_handler(ETHERNET_TYPE_IPV4, ipv4_receive);
}

int ipv4_configure(net_device_t *device, uint32_t address,
                   uint32_t netmask, uint32_t gateway) {
    if (!device || (device->flags & NETDEV_FLAG_ETHERNET) == 0 ||
        device->mtu < 68u ||
        !ipv4_config_is_valid(address, netmask, gateway))
        return 0;

    ipv4_interface_t *slot = ipv4_find_interface(device);
    if (!slot) {
        for (size_t i = 0; i < IPV4_MAX_INTERFACES; ++i) {
            if (!g_interfaces[i].valid) {
                slot = &g_interfaces[i];
                break;
            }
        }
    }
    if (!slot)
        return 0;

    slot->device = device;
    slot->config.address = address;
    slot->config.netmask = netmask;
    slot->config.gateway = gateway;
    slot->valid = 1;
    return 1;
}

int ipv4_get_config(net_device_t *device, ipv4_config_t *out_config) {
    ipv4_interface_t *interface = ipv4_find_interface(device);
    if (!interface || !out_config)
        return 0;
    *out_config = interface->config;
    return 1;
}

int ipv4_unconfigure(net_device_t *device) {
    ipv4_interface_t *interface = ipv4_find_interface(device);
    if (!interface)
        return 0;
    *interface = (ipv4_interface_t){0};
    return 1;
}

int ipv4_register_protocol(uint8_t protocol,
                           ipv4_protocol_handler_fn handler) {
    if (!handler || g_protocol_count >= IPV4_MAX_PROTOCOL_HANDLERS)
        return 0;
    for (size_t i = 0; i < g_protocol_count; ++i) {
        if (g_protocols[i].protocol == protocol)
            return 0;
    }
    g_protocols[g_protocol_count].protocol = protocol;
    g_protocols[g_protocol_count].handler = handler;
    ++g_protocol_count;
    return 1;
}

static int ipv4_send_broadcast(net_device_t *device,
                               const uint8_t *packet, size_t packet_length) {
    uint8_t frame[ETHERNET_HEADER_SIZE + IPV4_MAX_PACKET_SIZE];
    for (size_t i = 0; i < 6u; ++i)
        frame[i] = 0xFFu;
    ipv4_copy_bytes(frame + 6u, device->mac, 6u);
    ipv4_write_be16(frame + 12u, ETHERNET_TYPE_IPV4);
    ipv4_copy_bytes(frame + ETHERNET_HEADER_SIZE, packet, packet_length);
    return netdev_send(device, frame, ETHERNET_HEADER_SIZE + packet_length);
}

int ipv4_send(net_device_t *device, uint32_t destination,
              uint8_t protocol, const void *payload,
              size_t payload_length) {
    ipv4_interface_t *interface = ipv4_find_interface(device);
    if (!interface || (!payload && payload_length != 0u) ||
        payload_length > IPV4_MAX_PACKET_SIZE - IPV4_MIN_HEADER_SIZE ||
        payload_length + IPV4_MIN_HEADER_SIZE > device->mtu ||
        (!ipv4_is_unicast(destination) &&
         !ipv4_is_broadcast(&interface->config, destination)) ||
        destination == interface->config.address) {
        ++g_stats.tx_dropped;
        return 0;
    }

    const size_t packet_length = IPV4_MIN_HEADER_SIZE + payload_length;
    uint8_t packet[IPV4_MAX_PACKET_SIZE] = {0};
    packet[0] = 0x45u;
    packet[1] = 0u;
    ipv4_write_be16(packet + 2u, (uint16_t)packet_length);
    ipv4_write_be16(packet + 4u, g_next_identification++);
    ipv4_write_be16(packet + 6u, IPV4_FLAG_DONT_FRAGMENT);
    packet[8] = IPV4_DEFAULT_TTL;
    packet[9] = protocol;
    ipv4_write_address(packet + 12u, interface->config.address);
    ipv4_write_address(packet + 16u, destination);
    if (payload_length)
        ipv4_copy_bytes(packet + IPV4_MIN_HEADER_SIZE, payload, payload_length);
    ipv4_write_be16(packet + 10u, ipv4_checksum(packet, IPV4_MIN_HEADER_SIZE));

    int sent;
    if (ipv4_is_broadcast(&interface->config, destination)) {
        sent = ipv4_send_broadcast(device, packet, packet_length);
    } else {
        uint32_t next_hop = destination;
        if ((destination & interface->config.netmask) !=
            (interface->config.address & interface->config.netmask)) {
            next_hop = interface->config.gateway;
            if (next_hop == 0u) {
                ++g_stats.tx_dropped;
                return 0;
            }
        }
        sent = arp_send_ipv4(device, next_hop, packet, packet_length);
    }

    if (!sent) {
        ++g_stats.tx_dropped;
        return 0;
    }
    ++g_stats.tx_packets;
    return 1;
}

void ipv4_get_stats(ipv4_stats_t *out_stats) {
    if (out_stats)
        *out_stats = g_stats;
}
