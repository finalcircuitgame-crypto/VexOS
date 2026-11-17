#include "arp.h"
#include "ethernet.h"
#include "ipv4.h"

#include <stdio.h>
#include <stdint.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define TEST_PROTOCOL 253u

static uint64_t g_ticks;
static unsigned g_transmit_calls;
static size_t g_last_frame_length;
static uint8_t g_last_frame[ETHERNET_HEADER_SIZE + IPV4_MAX_PACKET_SIZE];
static unsigned g_handler_calls;
static ipv4_packet_info_t g_seen_info;
static size_t g_seen_payload_length;
static uint8_t g_seen_payload[32];

uint64_t Timer_Ticks(void) { return g_ticks; }

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

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_address(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void write_address(uint8_t *bytes, uint32_t address) {
    bytes[0] = (uint8_t)(address >> 24);
    bytes[1] = (uint8_t)(address >> 16);
    bytes[2] = (uint8_t)(address >> 8);
    bytes[3] = (uint8_t)address;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t length) {
    for (size_t i = 0; i < length; ++i)
        dst[i] = src[i];
}

static uint16_t checksum(const uint8_t *bytes, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1u < length; i += 2u)
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1u];
    if (length & 1u)
        sum += (uint32_t)bytes[length - 1u] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void finish_checksum(uint8_t *packet, size_t header_length) {
    write_be16(packet + 10u, 0u);
    write_be16(packet + 10u, checksum(packet, header_length));
}

static void protocol_handler(net_device_t *device,
                             const ipv4_packet_info_t *info,
                             const uint8_t *payload, size_t length) {
    (void)device;
    ++g_handler_calls;
    g_seen_info = *info;
    g_seen_payload_length = length;
    if (length > sizeof(g_seen_payload))
        length = sizeof(g_seen_payload);
    copy_bytes(g_seen_payload, payload, length);
}

static size_t build_packet(uint8_t *packet, size_t capacity,
                           uint32_t source, uint32_t destination,
                           uint8_t protocol, const uint8_t *payload,
                           size_t payload_length) {
    const size_t total = IPV4_MIN_HEADER_SIZE + payload_length;
    if (total > capacity || total > IPV4_MAX_PACKET_SIZE)
        return 0;
    for (size_t i = 0; i < total; ++i)
        packet[i] = 0;
    packet[0] = 0x45u;
    write_be16(packet + 2u, (uint16_t)total);
    write_be16(packet + 4u, 0x1234u);
    packet[8] = 64u;
    packet[9] = protocol;
    write_address(packet + 12u, source);
    write_address(packet + 16u, destination);
    if (payload_length)
        copy_bytes(packet + IPV4_MIN_HEADER_SIZE, payload, payload_length);
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    return total;
}

static void build_arp_request(uint8_t *frame, const uint8_t peer_mac[6],
                              uint32_t peer_ip, uint32_t local_ip) {
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t zero[6] = {0};
    copy_bytes(frame, broadcast, 6u);
    copy_bytes(frame + 6u, peer_mac, 6u);
    write_be16(frame + 12u, ETHERNET_TYPE_ARP);
    uint8_t *arp = frame + ETHERNET_HEADER_SIZE;
    write_be16(arp, 1u);
    write_be16(arp + 2u, 0x0800u);
    arp[4] = 6u;
    arp[5] = 4u;
    write_be16(arp + 6u, 1u);
    copy_bytes(arp + 8u, peer_mac, 6u);
    write_address(arp + 14u, peer_ip);
    copy_bytes(arp + 18u, zero, 6u);
    write_address(arp + 24u, local_ip);
}

static int dispatch_ipv4(net_device_t *device, const uint8_t *packet,
                         size_t packet_length) {
    uint8_t frame[ETHERNET_HEADER_SIZE + IPV4_MAX_PACKET_SIZE] = {0};
    const uint8_t destination_mac[6] = {0x02, 0, 0, 0, 0, 1};
    const uint8_t source_mac[6] = {0x02, 0, 0, 0, 0, 2};
    copy_bytes(frame, destination_mac, 6u);
    copy_bytes(frame + 6u, source_mac, 6u);
    write_be16(frame + 12u, ETHERNET_TYPE_IPV4);
    copy_bytes(frame + ETHERNET_HEADER_SIZE, packet, packet_length);
    return ethernet_dispatch_frame(device, frame,
        ETHERNET_HEADER_SIZE + packet_length) == ETHERNET_DISPATCHED;
}

int main(void) {
    net_device_t device = {0};
    device.name[0] = 'e';
    device.name[1] = 't';
    device.mtu = 1500u;
    device.flags = NETDEV_FLAG_ETHERNET | NETDEV_FLAG_BROADCAST;
    device.link_up = 1u;

    const uint8_t local_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const uint8_t gateway_mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
    const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t local_ip = 0xC0A8010Au;
    const uint32_t peer_ip = 0xC0A80114u;
    const uint32_t gateway_ip = 0xC0A80101u;
    const uint32_t remote_ip = 0x08080808u;
    const uint8_t message[] = {'V', 'e', 'x', 'O', 'S'};

    copy_bytes(device.mac, local_mac, 6u);
    ethernet_init();
    CHECK(arp_init());
    CHECK(ipv4_init());
    CHECK(ipv4_register_protocol(TEST_PROTOCOL, protocol_handler));
    CHECK(!ipv4_register_protocol(TEST_PROTOCOL, protocol_handler));
    CHECK(!ipv4_register_protocol(17u, 0));

    CHECK(!ipv4_configure(&device, local_ip, 0xFF00FF00u, 0u));
    CHECK(!ipv4_configure(&device, 0xC0A80100u, 0xFFFFFF00u, gateway_ip));
    CHECK(!ipv4_configure(&device, local_ip, 0xFFFFFF00u, 0x0A000001u));
    CHECK(ipv4_configure(&device, local_ip, 0xFFFFFF00u, gateway_ip));

    ipv4_config_t config = {0};
    CHECK(ipv4_get_config(&device, &config));
    CHECK(config.address == local_ip && config.netmask == 0xFFFFFF00u);
    CHECK(config.gateway == gateway_ip);

    /* A local packet is delivered; Ethernet padding is ignored. */
    uint8_t packet[64] = {0};
    size_t packet_length = build_packet(packet, sizeof(packet), peer_ip,
        local_ip, TEST_PROTOCOL, message, sizeof(message));
    CHECK(packet_length == IPV4_MIN_HEADER_SIZE + sizeof(message));
    CHECK(dispatch_ipv4(&device, packet, packet_length + 8u));
    CHECK(g_handler_calls == 1u);
    CHECK(g_seen_info.source == peer_ip && g_seen_info.destination == local_ip);
    CHECK(g_seen_info.protocol == TEST_PROTOCOL && g_seen_info.ttl == 64u);
    CHECK(g_seen_payload_length == sizeof(message));
    CHECK(g_seen_payload[0] == 'V' && g_seen_payload[4] == 'S');

    /* Non-local delivery, checksum errors, malformed headers and fragments. */
    packet_length = build_packet(packet, sizeof(packet), peer_ip,
        0xC0A80163u, TEST_PROTOCOL, message, sizeof(message));
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    CHECK(g_handler_calls == 1u);

    packet_length = build_packet(packet, sizeof(packet), peer_ip, local_ip,
        TEST_PROTOCOL, message, sizeof(message));
    packet[8] = 0u;
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    packet[8] = 64u;
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    packet[10] ^= 1u;
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    packet[10] ^= 1u;
    packet[0] = 0x44u;
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    packet[0] = 0x45u;
    write_be16(packet + 2u, 19u);
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    write_be16(packet + 2u, (uint16_t)packet_length);
    write_be16(packet + 6u, 0x8000u);
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    write_be16(packet + 6u, 0x2000u);
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    CHECK(dispatch_ipv4(&device, packet, packet_length));
    write_be16(packet + 6u, 1u);
    finish_checksum(packet, IPV4_MIN_HEADER_SIZE);
    CHECK(dispatch_ipv4(&device, packet, packet_length));

    ipv4_stats_t stats = {0};
    ipv4_get_stats(&stats);
    CHECK(stats.rx_packets == 1u && stats.rx_delivered == 1u);
    CHECK(stats.rx_nonlocal == 1u && stats.rx_bad_checksum == 1u);
    CHECK(stats.rx_fragments == 2u && stats.rx_malformed == 4u);

    /* ARP learns a peer and gateway. Outbound packets select the right MAC. */
    uint8_t arp_frame[ETHERNET_HEADER_SIZE + 28u] = {0};
    build_arp_request(arp_frame, peer_mac, peer_ip, local_ip);
    CHECK(ethernet_dispatch_frame(&device, arp_frame, sizeof(arp_frame)) ==
          ETHERNET_DISPATCHED);
    build_arp_request(arp_frame, gateway_mac, gateway_ip, local_ip);
    CHECK(ethernet_dispatch_frame(&device, arp_frame, sizeof(arp_frame)) ==
          ETHERNET_DISPATCHED);

    g_transmit_calls = 0;
    CHECK(ipv4_send(&device, peer_ip, TEST_PROTOCOL, message, sizeof(message)));
    CHECK(g_transmit_calls == 1u);
    CHECK(g_last_frame_length == ETHERNET_HEADER_SIZE + IPV4_MIN_HEADER_SIZE +
                                sizeof(message));
    CHECK(g_last_frame[0] == peer_mac[0] && g_last_frame[5] == peer_mac[5]);
    CHECK(read_be16(g_last_frame + 12u) == ETHERNET_TYPE_IPV4);
    const uint8_t *outgoing = g_last_frame + ETHERNET_HEADER_SIZE;
    CHECK(read_address(outgoing + 12u) == local_ip);
    CHECK(read_address(outgoing + 16u) == peer_ip);
    CHECK(outgoing[8] == IPV4_DEFAULT_TTL && outgoing[9] == TEST_PROTOCOL);
    CHECK(read_be16(outgoing + 6u) == IPV4_FLAG_DONT_FRAGMENT);
    CHECK(checksum(outgoing, IPV4_MIN_HEADER_SIZE) == 0u);

    CHECK(ipv4_send(&device, remote_ip, TEST_PROTOCOL, message, sizeof(message)));
    CHECK(g_last_frame[0] == gateway_mac[0] && g_last_frame[5] == gateway_mac[5]);
    outgoing = g_last_frame + ETHERNET_HEADER_SIZE;
    CHECK(read_address(outgoing + 16u) == remote_ip);

    CHECK(ipv4_send(&device, 0xC0A801FFu, TEST_PROTOCOL,
                    message, sizeof(message)));
    CHECK(g_last_frame[0] == broadcast_mac[0] && g_last_frame[5] == broadcast_mac[5]);

    CHECK(!ipv4_send(&device, remote_ip, TEST_PROTOCOL, message, 1481u));
    CHECK(ipv4_unconfigure(&device));
    CHECK(!ipv4_get_config(&device, &config));
    CHECK(!ipv4_send(&device, remote_ip, TEST_PROTOCOL, message, sizeof(message)));

    ipv4_get_stats(&stats);
    CHECK(stats.tx_packets == 3u && stats.tx_dropped >= 2u);

    puts("PASS: IPv4 header validation, checksum, local delivery, fragmentation policy, interface config, routing, ARP handoff, and broadcast send");
    return 0;
}
