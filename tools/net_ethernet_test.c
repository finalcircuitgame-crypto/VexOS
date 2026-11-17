#include "ethernet.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static unsigned g_handler_calls;
static net_device_t *g_seen_device;
static const uint8_t *g_seen_destination;
static const uint8_t *g_seen_source;
static const uint8_t *g_seen_payload;
static size_t g_seen_payload_length;

static void test_arp_handler(net_device_t *device,
                             const uint8_t destination[6],
                             const uint8_t source[6],
                             const uint8_t *payload,
                             size_t payload_length) {
    ++g_handler_calls;
    g_seen_device = device;
    g_seen_destination = destination;
    g_seen_source = source;
    g_seen_payload = payload;
    g_seen_payload_length = payload_length;
}

static void write_be16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

int main(void) {
    net_device_t device = {0};
    device.name[0] = 'e';
    device.name[1] = 't';
    device.mtu = 1500;
    device.flags = NETDEV_FLAG_ETHERNET;

    uint8_t frame[64] = {0};
    const uint8_t destination[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    const uint8_t source[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    for (size_t i = 0; i < 6; ++i) {
        frame[i] = destination[i];
        frame[i + 6u] = source[i];
    }
    write_be16(frame + 12u, ETHERNET_TYPE_ARP);
    for (size_t i = ETHERNET_HEADER_SIZE; i < sizeof(frame); ++i)
        frame[i] = (uint8_t)i;

    ethernet_init();
    CHECK(ethernet_register_handler(ETHERNET_TYPE_ARP, test_arp_handler));
    CHECK(!ethernet_register_handler(ETHERNET_TYPE_ARP, test_arp_handler));
    CHECK(!ethernet_register_handler(0x0006u, test_arp_handler));
    CHECK(!ethernet_register_handler(ETHERNET_TYPE_IPV4, 0));

    CHECK(ethernet_dispatch_frame(&device, frame, sizeof(frame)) ==
          ETHERNET_DISPATCHED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_seen_device == &device);
    CHECK(g_seen_destination == frame);
    CHECK(g_seen_source == frame + 6u);
    CHECK(g_seen_payload == frame + ETHERNET_HEADER_SIZE);
    CHECK(g_seen_payload_length == sizeof(frame) - ETHERNET_HEADER_SIZE);
    CHECK(g_seen_payload[0] == ETHERNET_HEADER_SIZE);

    /* Short frames, oversized frames, and non-Ethernet devices are rejected. */
    CHECK(ethernet_dispatch_frame(&device, frame, 13u) ==
          ETHERNET_DISPATCH_INVALID);
    CHECK(ethernet_dispatch_frame(&device, frame, 1515u) ==
          ETHERNET_DISPATCH_INVALID);
    net_device_t non_ethernet = device;
    non_ethernet.flags = 0;
    CHECK(ethernet_dispatch_frame(&non_ethernet, frame, sizeof(frame)) ==
          ETHERNET_DISPATCH_INVALID);

    /* 802.3 length fields and VLAN encapsulation are explicitly unsupported. */
    write_be16(frame + 12u, 0x002Eu);
    CHECK(ethernet_dispatch_frame(&device, frame, sizeof(frame)) ==
          ETHERNET_DISPATCH_UNSUPPORTED);
    write_be16(frame + 12u, ETHERNET_TYPE_VLAN);
    CHECK(ethernet_dispatch_frame(&device, frame, sizeof(frame)) ==
          ETHERNET_DISPATCH_UNSUPPORTED);

    /* Valid EtherTypes without a registered handler are counted and dropped. */
    write_be16(frame + 12u, ETHERNET_TYPE_IPV4);
    CHECK(ethernet_dispatch_frame(&device, frame, sizeof(frame)) ==
          ETHERNET_DISPATCH_NO_HANDLER);

    ethernet_stats_t stats = {0};
    ethernet_get_stats(&stats);
    CHECK(stats.rx_frames == 7u);
    CHECK(stats.rx_dispatched == 1u);
    CHECK(stats.rx_invalid == 3u);
    CHECK(stats.rx_unsupported == 2u);
    CHECK(stats.rx_no_handler == 1u);
    CHECK(device.rx_dropped == 6u);
    CHECK(g_handler_calls == 1u);

    puts("PASS: Ethernet II bounds, EtherType dispatch, borrowed-buffer contract, and drop accounting");
    return 0;
}
