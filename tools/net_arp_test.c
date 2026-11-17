#include "arp.h"
#include "ethernet.h"

#include <stdio.h>
#include <stdint.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t g_ticks;
static unsigned g_transmit_calls;
static size_t g_last_frame_length;
static uint8_t g_last_frame[ETHERNET_HEADER_SIZE + ARP_MAX_IPV4_PACKET_SIZE];

uint64_t Timer_Ticks(void) {
    return g_ticks;
}

int netdev_send(net_device_t *device, const void *frame, size_t length) {
    (void)device;
    if (!frame || length > sizeof(g_last_frame))
        return 0;
    const uint8_t *bytes = (const uint8_t *)frame;
    for (size_t i = 0; i < length; ++i)
        g_last_frame[i] = bytes[i];
    g_last_frame_length = length;
    ++g_transmit_calls;
    return 1;
}

static void write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_ipv4(uint8_t *bytes, uint32_t address) {
    bytes[0] = (uint8_t)(address >> 24);
    bytes[1] = (uint8_t)(address >> 16);
    bytes[2] = (uint8_t)(address >> 8);
    bytes[3] = (uint8_t)address;
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_ipv4(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       size_t length) {
    for (size_t i = 0; i < length; ++i)
        destination[i] = source[i];
}

static void build_arp_frame(uint8_t *frame, const uint8_t ethernet_destination[6],
                            const uint8_t ethernet_source[6], uint16_t opcode,
                            const uint8_t sender_mac[6], uint32_t sender_ip,
                            const uint8_t target_mac[6], uint32_t target_ip) {
    for (size_t i = 0; i < 6u; ++i) {
        frame[i] = ethernet_destination[i];
        frame[i + 6u] = ethernet_source[i];
    }
    write_be16(frame + 12u, ETHERNET_TYPE_ARP);

    uint8_t *packet = frame + ETHERNET_HEADER_SIZE;
    write_be16(packet + 0u, 1u);
    write_be16(packet + 2u, 0x0800u);
    packet[4] = 6u;
    packet[5] = 4u;
    write_be16(packet + 6u, opcode);
    copy_bytes(packet + 8u, sender_mac, 6u);
    write_ipv4(packet + 14u, sender_ip);
    copy_bytes(packet + 18u, target_mac, 6u);
    write_ipv4(packet + 24u, target_ip);
}

static int dispatch_arp(net_device_t *device, const uint8_t *frame,
                        size_t frame_length) {
    return ethernet_dispatch_frame(device, frame, frame_length) ==
           ETHERNET_DISPATCHED;
}

static void make_ipv4_packet(uint8_t packet[28], uint32_t source,
                             uint32_t destination) {
    for (size_t i = 0; i < 28u; ++i)
        packet[i] = 0;
    packet[0] = 0x45u;
    write_be16(packet + 2u, 28u);
    packet[8] = 64u;
    packet[9] = 17u;
    write_ipv4(packet + 12u, source);
    write_ipv4(packet + 16u, destination);
}

int main(void) {
    net_device_t device = {0};
    device.name[0] = 'e';
    device.name[1] = 't';
    device.device_data = 0;
    device.mtu = 1500u;
    device.flags = NETDEV_FLAG_ETHERNET | NETDEV_FLAG_BROADCAST;
    device.link_up = 1;

    const uint8_t local_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const uint8_t conflict_mac[6] = {0x02, 0xBA, 0xDB, 0xAD, 0x00, 0x01};
    const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t zero_mac[6] = {0};
    const uint32_t local_ip = ARP_IPV4(10, 0, 2, 15);
    const uint32_t peer_ip = ARP_IPV4(10, 0, 2, 2);
    const uint32_t unknown_ip = ARP_IPV4(10, 0, 2, 99);

    copy_bytes(device.mac, local_mac, 6u);
    ethernet_init();
    CHECK(arp_init());

    /* Reject truncated payloads and malformed hardware/protocol fields. */
    uint8_t frame[ETHERNET_HEADER_SIZE + 28u] = {0};
    build_arp_frame(frame, local_mac, peer_mac, 1u, peer_mac, peer_ip,
                    zero_mac, local_ip);
    CHECK(ethernet_dispatch_frame(&device, frame, sizeof(frame) - 1u) ==
          ETHERNET_DISPATCHED);
    frame[ETHERNET_HEADER_SIZE + 4u] = 5u;
    CHECK(dispatch_arp(&device, frame, sizeof(frame)));
    frame[ETHERNET_HEADER_SIZE + 4u] = 6u;
    write_be16(frame + ETHERNET_HEADER_SIZE, 2u);
    CHECK(dispatch_arp(&device, frame, sizeof(frame)));
    write_be16(frame + ETHERNET_HEADER_SIZE, 1u);

    arp_stats_t stats = {0};
    arp_get_stats(&stats);
    CHECK(stats.rx_malformed == 3u);

    /* Valid requests teach a mapping; a conflicting MAC cannot replace it. */
    build_arp_frame(frame, broadcast_mac, peer_mac, 1u, peer_mac, peer_ip,
                    zero_mac, local_ip);
    CHECK(dispatch_arp(&device, frame, sizeof(frame)));
    uint8_t learned_mac[6] = {0};
    CHECK(arp_cache_lookup(&device, peer_ip, learned_mac));
    CHECK(learned_mac[0] == peer_mac[0] && learned_mac[5] == peer_mac[5]);

    build_arp_frame(frame, broadcast_mac, conflict_mac, 1u, conflict_mac,
                    peer_ip, zero_mac, local_ip);
    CHECK(dispatch_arp(&device, frame, sizeof(frame)));
    CHECK(arp_cache_lookup(&device, peer_ip, learned_mac));
    CHECK(learned_mac[0] == peer_mac[0] && learned_mac[5] == peer_mac[5]);
    arp_get_stats(&stats);
    CHECK(stats.cache_conflicts == 1u);

    /* A matching cache entry sends IPv4 directly to the learned MAC. */
    uint8_t ipv4_packet[28];
    make_ipv4_packet(ipv4_packet, local_ip, peer_ip);
    CHECK(arp_send_ipv4(&device, peer_ip, ipv4_packet, sizeof(ipv4_packet)));
    CHECK(g_last_frame_length == ETHERNET_HEADER_SIZE + sizeof(ipv4_packet));
    CHECK(g_last_frame[0] == peer_mac[0] && g_last_frame[5] == peer_mac[5]);
    CHECK(read_be16(g_last_frame + 12u) == ETHERNET_TYPE_IPV4);

    /* A cache miss broadcasts one ARP request and queues a bounded packet copy. */
    CHECK(arp_init());
    ethernet_init();
    /* Re-register after clearing the Ethernet protocol registry. */
    CHECK(arp_init());
    g_transmit_calls = 0;
    make_ipv4_packet(ipv4_packet, local_ip, unknown_ip);
    CHECK(arp_send_ipv4(&device, unknown_ip, ipv4_packet,
                        sizeof(ipv4_packet)));
    CHECK(g_transmit_calls == 1u);
    CHECK(read_be16(g_last_frame + 12u) == ETHERNET_TYPE_ARP);
    CHECK(read_be16(g_last_frame + ETHERNET_HEADER_SIZE + 6u) == 1u);
    CHECK(read_ipv4(g_last_frame + ETHERNET_HEADER_SIZE + 14u) == local_ip);
    CHECK(read_ipv4(g_last_frame + ETHERNET_HEADER_SIZE + 24u) == unknown_ip);

    /* Only a solicited reply addressed to this interface releases the queue. */
    build_arp_frame(frame, local_mac, peer_mac, 2u, peer_mac, unknown_ip,
                    local_mac, local_ip);
    CHECK(dispatch_arp(&device, frame, sizeof(frame)));
    CHECK(g_transmit_calls == 2u);
    CHECK(read_be16(g_last_frame + 12u) == ETHERNET_TYPE_IPV4);
    CHECK(g_last_frame[0] == peer_mac[0] && g_last_frame[5] == peer_mac[5]);
    arp_get_stats(&stats);
    CHECK(stats.pending_queued == 1u);
    CHECK(stats.pending_sent == 1u);
    CHECK(arp_cache_lookup(&device, unknown_ip, learned_mac));

    /* Cache entries expire using the kernel's millisecond PIT tick counter. */
    g_ticks += ARP_CACHE_TTL_TICKS;
    CHECK(!arp_cache_lookup(&device, unknown_ip, learned_mac));
    arp_get_stats(&stats);
    CHECK(stats.cache_expired >= 1u);

    /* The pending queue has a hard capacity and expires unresolved packets. */
    CHECK(arp_init());
    ethernet_init();
    CHECK(arp_init());
    g_ticks = 100u;
    for (uint32_t i = 0; i < ARP_PENDING_PACKETS; ++i) {
        uint32_t target = ARP_IPV4(10, 0, 3, (uint8_t)(i + 1u));
        make_ipv4_packet(ipv4_packet, local_ip, target);
        CHECK(arp_send_ipv4(&device, target, ipv4_packet,
                            sizeof(ipv4_packet)));
    }
    make_ipv4_packet(ipv4_packet, local_ip, ARP_IPV4(10, 0, 3, 99));
    CHECK(!arp_send_ipv4(&device, ARP_IPV4(10, 0, 3, 99), ipv4_packet,
                         sizeof(ipv4_packet)));
    g_ticks += ARP_PENDING_TIMEOUT_TICKS;
    arp_poll();
    arp_get_stats(&stats);
    CHECK(stats.pending_timeouts == ARP_PENDING_PACKETS);
    CHECK(stats.pending_dropped == ARP_PENDING_PACKETS + 1u);

    puts("PASS: ARP parsing, conflict-safe cache, expiry, IPv4 send, pending resolution, and queue bounds");
    return 0;
}
