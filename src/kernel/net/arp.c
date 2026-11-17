#include "arp.h"

#include "ethernet.h"
#include "interrupts.h"

#include <stddef.h>
#include <stdint.h>

#define ARP_ETHERNET_HARDWARE 1u
#define ARP_IPV4_PROTOCOL     0x0800u
#define ARP_OPCODE_REQUEST    1u
#define ARP_OPCODE_REPLY      2u
#define ARP_PACKET_SIZE       28u
#define ARP_ETHERNET_FRAME_SIZE (ETHERNET_HEADER_SIZE + ARP_PACKET_SIZE)

typedef struct {
    net_device_t *device;
    uint32_t address;
    uint8_t mac[6];
    uint64_t learned_at;
    uint8_t valid;
} arp_cache_entry_t;

typedef struct {
    net_device_t *device;
    uint32_t next_hop;
    uint32_t source_address;
    uint64_t queued_at;
    size_t packet_length;
    uint8_t packet[ARP_MAX_IPV4_PACKET_SIZE];
    uint8_t valid;
} arp_pending_entry_t;

static arp_cache_entry_t g_cache[ARP_CACHE_ENTRIES];
static arp_pending_entry_t g_pending[ARP_PENDING_PACKETS];
static arp_stats_t g_stats;

static uint16_t arp_read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static uint32_t arp_read_ipv4(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void arp_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void arp_write_ipv4(uint8_t *bytes, uint32_t address) {
    bytes[0] = (uint8_t)(address >> 24);
    bytes[1] = (uint8_t)(address >> 16);
    bytes[2] = (uint8_t)(address >> 8);
    bytes[3] = (uint8_t)address;
}

static void arp_copy_bytes(void *destination, const void *source,
                           size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        out[i] = in[i];
}

static int arp_bytes_equal(const uint8_t *a, const uint8_t *b,
                           size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static int arp_mac_is_unicast(const uint8_t mac[6]) {
    uint8_t any = 0;
    uint8_t all_ones = 1;
    for (size_t i = 0; i < 6u; ++i) {
        any |= mac[i];
        if (mac[i] != 0xFFu)
            all_ones = 0;
    }
    return any != 0 && !all_ones && (mac[0] & 1u) == 0;
}

static int arp_ipv4_is_unicast(uint32_t address) {
    uint8_t first = (uint8_t)(address >> 24);
    return address != 0 && address != 0xFFFFFFFFu &&
           first != 127u && first >= 1u && first <= 223u;
}

static int arp_cache_entry_expired(const arp_cache_entry_t *entry,
                                   uint64_t now) {
    return entry->valid &&
           now - entry->learned_at >= ARP_CACHE_TTL_TICKS;
}

static int arp_pending_entry_expired(const arp_pending_entry_t *entry,
                                     uint64_t now) {
    return entry->valid &&
           now - entry->queued_at >= ARP_PENDING_TIMEOUT_TICKS;
}

static int arp_cache_learn(net_device_t *device, uint32_t address,
                           const uint8_t mac[6]) {
    if (!device || !arp_ipv4_is_unicast(address) ||
        !arp_mac_is_unicast(mac))
        return 0;

    uint64_t now = Timer_Ticks();
    size_t free_slot = ARP_CACHE_ENTRIES;

    for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
        arp_cache_entry_t *entry = &g_cache[i];
        if (!entry->valid) {
            if (free_slot == ARP_CACHE_ENTRIES)
                free_slot = i;
            continue;
        }

        if (arp_cache_entry_expired(entry, now)) {
            entry->valid = 0;
            ++g_stats.cache_expired;
            if (free_slot == ARP_CACHE_ENTRIES)
                free_slot = i;
            continue;
        }

        if (entry->device != device || entry->address != address)
            continue;

        if (!arp_bytes_equal(entry->mac, mac, 6u)) {
            /*
             * Do not let an unsolicited or conflicting ARP packet replace a
             * live mapping. The existing mapping remains authoritative until
             * it expires; a later matching packet may refresh it.
             */
            ++g_stats.cache_conflicts;
            return 0;
        }

        entry->learned_at = now;
        return 1;
    }

    if (free_slot == ARP_CACHE_ENTRIES) {
        ++g_stats.cache_full;
        return 0;
    }

    arp_cache_entry_t *entry = &g_cache[free_slot];
    entry->device = device;
    entry->address = address;
    arp_copy_bytes(entry->mac, mac, 6u);
    entry->learned_at = now;
    entry->valid = 1;
    return 1;
}

int arp_cache_lookup(net_device_t *device, uint32_t address,
                     uint8_t mac_out[6]) {
    if (!device || !mac_out || !arp_ipv4_is_unicast(address))
        return 0;

    uint64_t now = Timer_Ticks();
    for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
        arp_cache_entry_t *entry = &g_cache[i];
        if (!entry->valid)
            continue;
        if (arp_cache_entry_expired(entry, now)) {
            entry->valid = 0;
            ++g_stats.cache_expired;
            continue;
        }
        if (entry->device == device && entry->address == address) {
            arp_copy_bytes(mac_out, entry->mac, 6u);
            ++g_stats.cache_hits;
            return 1;
        }
    }

    ++g_stats.cache_misses;
    return 0;
}

static int arp_send_request(net_device_t *device, uint32_t source_address,
                            uint32_t target_address) {
    uint8_t frame[ARP_ETHERNET_FRAME_SIZE] = {0};

    for (size_t i = 0; i < 6u; ++i)
        frame[i] = 0xFFu;
    arp_copy_bytes(frame + 6u, device->mac, 6u);
    arp_write_be16(frame + 12u, ETHERNET_TYPE_ARP);

    uint8_t *packet = frame + ETHERNET_HEADER_SIZE;
    arp_write_be16(packet + 0u, ARP_ETHERNET_HARDWARE);
    arp_write_be16(packet + 2u, ARP_IPV4_PROTOCOL);
    packet[4] = 6u;
    packet[5] = 4u;
    arp_write_be16(packet + 6u, ARP_OPCODE_REQUEST);
    arp_copy_bytes(packet + 8u, device->mac, 6u);
    arp_write_ipv4(packet + 14u, source_address);
    /* Target hardware address remains zero in a request. */
    arp_write_ipv4(packet + 24u, target_address);

    if (!netdev_send(device, frame, sizeof(frame))) {
        ++g_stats.tx_failures;
        return 0;
    }

    ++g_stats.tx_requests;
    return 1;
}

static int arp_send_reply(net_device_t *device, const uint8_t target_mac[6],
                          uint32_t source_address,
                          uint32_t target_address) {
    uint8_t frame[ARP_ETHERNET_FRAME_SIZE] = {0};

    arp_copy_bytes(frame, target_mac, 6u);
    arp_copy_bytes(frame + 6u, device->mac, 6u);
    arp_write_be16(frame + 12u, ETHERNET_TYPE_ARP);

    uint8_t *packet = frame + ETHERNET_HEADER_SIZE;
    arp_write_be16(packet + 0u, ARP_ETHERNET_HARDWARE);
    arp_write_be16(packet + 2u, ARP_IPV4_PROTOCOL);
    packet[4] = 6u;
    packet[5] = 4u;
    arp_write_be16(packet + 6u, ARP_OPCODE_REPLY);
    arp_copy_bytes(packet + 8u, device->mac, 6u);
    arp_write_ipv4(packet + 14u, source_address);
    arp_copy_bytes(packet + 18u, target_mac, 6u);
    arp_write_ipv4(packet + 24u, target_address);

    if (!netdev_send(device, frame, sizeof(frame))) {
        ++g_stats.tx_failures;
        return 0;
    }

    ++g_stats.tx_replies;
    return 1;
}

static int arp_find_pending_local_address(net_device_t *device,
                                          uint32_t address) {
    uint64_t now = Timer_Ticks();
    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        arp_pending_entry_t *entry = &g_pending[i];
        if (entry->valid && !arp_pending_entry_expired(entry, now) &&
            entry->device == device && entry->source_address == address)
            return 1;
    }
    return 0;
}

static int arp_has_matching_pending_reply(net_device_t *device,
                                          uint32_t sender_address,
                                          uint32_t target_address) {
    uint64_t now = Timer_Ticks();
    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        arp_pending_entry_t *entry = &g_pending[i];
        if (entry->valid && !arp_pending_entry_expired(entry, now) &&
            entry->device == device &&
            entry->next_hop == sender_address &&
            entry->source_address == target_address)
            return 1;
    }
    return 0;
}

static int arp_send_ipv4_frame(net_device_t *device,
                               const uint8_t destination_mac[6],
                               const uint8_t *packet, size_t packet_length) {
    uint8_t frame[ETHERNET_HEADER_SIZE + ARP_MAX_IPV4_PACKET_SIZE];

    arp_copy_bytes(frame, destination_mac, 6u);
    arp_copy_bytes(frame + 6u, device->mac, 6u);
    arp_write_be16(frame + 12u, ETHERNET_TYPE_IPV4);
    arp_copy_bytes(frame + ETHERNET_HEADER_SIZE, packet, packet_length);

    return netdev_send(device, frame, ETHERNET_HEADER_SIZE + packet_length);
}

static void arp_flush_pending(net_device_t *device, uint32_t next_hop,
                              uint32_t source_address,
                              const uint8_t destination_mac[6]) {
    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        arp_pending_entry_t *entry = &g_pending[i];
        if (!entry->valid || entry->device != device ||
            entry->next_hop != next_hop ||
            entry->source_address != source_address)
            continue;

        if (arp_send_ipv4_frame(device, destination_mac, entry->packet,
                                entry->packet_length)) {
            entry->valid = 0;
            ++g_stats.pending_sent;
        } else {
            ++g_stats.tx_failures;
        }
    }
}

static void arp_receive(net_device_t *device,
                        const uint8_t destination[6],
                        const uint8_t source[6],
                        const uint8_t *payload,
                        size_t payload_length) {
    (void)destination;

    if (!device || !payload || payload_length < ARP_PACKET_SIZE) {
        ++g_stats.rx_malformed;
        return;
    }

    const uint16_t hardware_type = arp_read_be16(payload + 0u);
    const uint16_t protocol_type = arp_read_be16(payload + 2u);
    const uint16_t opcode = arp_read_be16(payload + 6u);
    const uint32_t sender_address = arp_read_ipv4(payload + 14u);
    const uint32_t target_address = arp_read_ipv4(payload + 24u);
    const uint8_t *sender_mac = payload + 8u;
    const uint8_t *target_mac = payload + 18u;

    if (hardware_type != ARP_ETHERNET_HARDWARE ||
        protocol_type != ARP_IPV4_PROTOCOL ||
        payload[4] != 6u || payload[5] != 4u ||
        (opcode != ARP_OPCODE_REQUEST && opcode != ARP_OPCODE_REPLY) ||
        !arp_mac_is_unicast(sender_mac) ||
        !arp_bytes_equal(sender_mac, source, 6u)) {
        ++g_stats.rx_malformed;
        return;
    }

    if (opcode == ARP_OPCODE_REQUEST) {
        ++g_stats.rx_requests;

        /* An ARP probe (sender IP 0.0.0.0) must never enter the cache. */
        if (arp_ipv4_is_unicast(sender_address))
            (void)arp_cache_learn(device, sender_address, sender_mac);

        /*
         * VexOS has no interface-address configuration yet. A queued IPv4
         * packet supplies the local source address; only answer requests for
         * one of those known local addresses.
         */
        if (arp_ipv4_is_unicast(sender_address) &&
            arp_find_pending_local_address(device, target_address))
            (void)arp_send_reply(device, sender_mac, target_address,
                                 sender_address);
        return;
    }

    ++g_stats.rx_replies;

    /*
     * Accept replies only when they are addressed to this interface and match
     * an outstanding resolution. This avoids learning unsolicited ARP replies.
     */
    if (!arp_bytes_equal(target_mac, device->mac, 6u) ||
        !arp_ipv4_is_unicast(sender_address) ||
        !arp_ipv4_is_unicast(target_address) ||
        !arp_has_matching_pending_reply(device, sender_address,
                                        target_address))
        return;

    if (arp_cache_learn(device, sender_address, sender_mac))
        arp_flush_pending(device, sender_address, target_address, sender_mac);
}

int arp_init(void) {
    for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i)
        g_cache[i] = (arp_cache_entry_t){0};
    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i)
        g_pending[i] = (arp_pending_entry_t){0};
    g_stats = (arp_stats_t){0};

    return ethernet_register_handler(ETHERNET_TYPE_ARP, arp_receive);
}

static int arp_validate_ipv4_packet(const net_device_t *device,
                                    const uint8_t *packet, size_t length,
                                    uint32_t *source_address) {
    if (!device || !packet || !source_address ||
        (device->flags & NETDEV_FLAG_ETHERNET) == 0 ||
        device->mtu == 0 || length < 20u ||
        length > ARP_MAX_IPV4_PACKET_SIZE || length > device->mtu)
        return 0;

    const uint8_t version = (uint8_t)(packet[0] >> 4);
    const size_t header_length = (size_t)(packet[0] & 0x0Fu) * 4u;
    const uint16_t total_length = arp_read_be16(packet + 2u);
    if (version != 4u || header_length < 20u ||
        header_length > length || total_length != length ||
        total_length < header_length)
        return 0;

    uint32_t source = arp_read_ipv4(packet + 12u);
    if (!arp_ipv4_is_unicast(source))
        return 0;

    *source_address = source;
    return 1;
}

int arp_send_ipv4(net_device_t *device, uint32_t next_hop,
                  const uint8_t *ipv4_packet, size_t packet_length) {
    uint32_t source_address = 0;
    if (!arp_ipv4_is_unicast(next_hop) ||
        !arp_validate_ipv4_packet(device, ipv4_packet, packet_length,
                                  &source_address))
        return 0;

    uint8_t destination_mac[6];
    if (arp_cache_lookup(device, next_hop, destination_mac)) {
        if (arp_send_ipv4_frame(device, destination_mac, ipv4_packet,
                                packet_length))
            return 1;
        ++g_stats.tx_failures;
        return 0;
    }

    size_t free_slot = ARP_PENDING_PACKETS;
    int request_already_pending = 0;
    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        arp_pending_entry_t *entry = &g_pending[i];
        if (!entry->valid) {
            if (free_slot == ARP_PENDING_PACKETS)
                free_slot = i;
            continue;
        }
        if (entry->device == device && entry->next_hop == next_hop &&
            entry->source_address == source_address)
            request_already_pending = 1;
    }

    if (free_slot == ARP_PENDING_PACKETS) {
        ++g_stats.pending_dropped;
        return 0;
    }

    arp_pending_entry_t *entry = &g_pending[free_slot];
    entry->device = device;
    entry->next_hop = next_hop;
    entry->source_address = source_address;
    entry->queued_at = Timer_Ticks();
    entry->packet_length = packet_length;
    arp_copy_bytes(entry->packet, ipv4_packet, packet_length);
    entry->valid = 1;
    ++g_stats.pending_queued;

    if (!request_already_pending &&
        !arp_send_request(device, source_address, next_hop)) {
        entry->valid = 0;
        ++g_stats.pending_dropped;
        return 0;
    }

    return 1;
}

void arp_poll(void) {
    uint64_t now = Timer_Ticks();

    for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
        arp_cache_entry_t *entry = &g_cache[i];
        if (arp_cache_entry_expired(entry, now)) {
            entry->valid = 0;
            ++g_stats.cache_expired;
        }
    }

    for (size_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        arp_pending_entry_t *entry = &g_pending[i];
        if (arp_pending_entry_expired(entry, now)) {
            entry->valid = 0;
            ++g_stats.pending_timeouts;
            ++g_stats.pending_dropped;
        }
    }
}

void arp_get_stats(arp_stats_t *out_stats) {
    if (out_stats)
        *out_stats = g_stats;
}
