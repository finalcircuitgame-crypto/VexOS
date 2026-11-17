#include "ethernet.h"

typedef struct {
    uint16_t ether_type;
    ethernet_protocol_handler_fn handler;
} ethernet_handler_entry_t;

static ethernet_handler_entry_t g_handlers[ETHERNET_MAX_HANDLERS];
static size_t g_handler_count;
static ethernet_stats_t g_stats;

static uint16_t ethernet_read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static int ethernet_is_vlan_type(uint16_t ether_type) {
    return ether_type == ETHERNET_TYPE_VLAN ||
           ether_type == ETHERNET_TYPE_PROVIDER_VLAN ||
           ether_type == ETHERNET_TYPE_QINQ;
}

void ethernet_init(void) {
    for (size_t i = 0; i < ETHERNET_MAX_HANDLERS; ++i) {
        g_handlers[i].ether_type = 0;
        g_handlers[i].handler = 0;
    }

    g_handler_count = 0;
    g_stats = (ethernet_stats_t){0};
}

int ethernet_register_handler(uint16_t ether_type,
                              ethernet_protocol_handler_fn handler) {
    /* Values below 0x0600 are IEEE 802.3 length fields, not EtherTypes. */
    if (ether_type < 0x0600u || !handler ||
        ethernet_is_vlan_type(ether_type) ||
        g_handler_count >= ETHERNET_MAX_HANDLERS)
        return 0;

    for (size_t i = 0; i < g_handler_count; ++i) {
        if (g_handlers[i].ether_type == ether_type)
            return 0;
    }

    g_handlers[g_handler_count].ether_type = ether_type;
    g_handlers[g_handler_count].handler = handler;
    ++g_handler_count;
    return 1;
}

ethernet_dispatch_result_t ethernet_dispatch_frame(
    net_device_t *device, const uint8_t *frame, size_t length) {
    ++g_stats.rx_frames;

    if (!device || !frame ||
        (device->flags & NETDEV_FLAG_ETHERNET) == 0 ||
        length < ETHERNET_HEADER_SIZE ||
        length > (size_t)device->mtu + ETHERNET_HEADER_SIZE) {
        ++g_stats.rx_invalid;
        if (device)
            ++device->rx_dropped;
        return ETHERNET_DISPATCH_INVALID;
    }

    const uint16_t ether_type = ethernet_read_be16(frame + 12u);

    /*
     * This milestone intentionally handles untagged Ethernet II only.
     * 802.3 length-format frames and VLAN/QinQ encapsulation remain unsupported
     * until their parsing and nested-header bounds are implemented.
     */
    if (ether_type < 0x0600u || ethernet_is_vlan_type(ether_type)) {
        ++g_stats.rx_unsupported;
        ++device->rx_dropped;
        return ETHERNET_DISPATCH_UNSUPPORTED;
    }

    ethernet_protocol_handler_fn handler = 0;
    for (size_t i = 0; i < g_handler_count; ++i) {
        if (g_handlers[i].ether_type == ether_type) {
            handler = g_handlers[i].handler;
            break;
        }
    }

    if (!handler) {
        ++g_stats.rx_no_handler;
        ++device->rx_dropped;
        return ETHERNET_DISPATCH_NO_HANDLER;
    }

    ++g_stats.rx_dispatched;

    /*
     * No allocation or copy occurs here. The E1000 driver reuses this receive
     * buffer as soon as netdev_receive returns, so handlers must finish using
     * all frame data synchronously or copy the bytes they need.
     */
    handler(device, frame, frame + 6u,
            frame + ETHERNET_HEADER_SIZE,
            length - ETHERNET_HEADER_SIZE);
    return ETHERNET_DISPATCHED;
}

void ethernet_receive(net_device_t *device, const uint8_t *frame,
                      size_t length) {
    (void)ethernet_dispatch_frame(device, frame, length);
}

void ethernet_get_stats(ethernet_stats_t *out_stats) {
    if (out_stats)
        *out_stats = g_stats;
}
