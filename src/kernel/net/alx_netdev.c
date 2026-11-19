#include "alx_netdev.h"

#include "alx.h"
#include "netdev.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);

#define ALX_NETDEV_FRAME_BYTES 1518u

static net_device_t g_alx_netdev;
static uint8_t g_rx_frame[ALX_NETDEV_FRAME_BYTES];

static int alx_netdev_transmit(net_device_t *device,
                               const void *frame, size_t length) {
    (void)device;
    if (!frame || length > ALX_NETDEV_FRAME_BYTES)
        return 0;
    return alx_send(frame, (uint16_t)length);
}

static void alx_netdev_poll(net_device_t *device, uint32_t budget) {
    if (!device || budget == 0u || !alx_is_ready())
        return;

    alx_poll();
    device->link_up = alx_link_up() ? 1u : 0u;

    for (uint32_t work = 0; work < budget; ++work) {
        uint16_t frame_length = 0u;
        int received = alx_receive(g_rx_frame,
                                   (uint16_t)sizeof(g_rx_frame),
                                   &frame_length);
        if (received == 0)
            break;
        if (received < 0)
            break; // Avoid spinning on a faulted completion; ALX poll resets the ring.
        netdev_receive(device, g_rx_frame, frame_length);
    }
}

static const netdev_ops_t g_alx_netdev_ops = {
    .transmit = alx_netdev_transmit,
    .poll = alx_netdev_poll
};

int alx_netdev_attach(void) {
    if (!alx_is_ready()) {
        serial_print("[NET] ALX is not initialized; no interface registered\n");
        return 0;
    }

    const uint8_t *mac = alx_mac_address();
    if (!mac) {
        serial_print("[NET] ALX did not provide a MAC address\n");
        return 0;
    }

    size_t index = netdev_count();
    if (index >= 10u) {
        serial_print("[NET] No interface name available for ALX\n");
        return 0;
    }

    g_alx_netdev = (net_device_t){0};
    g_alx_netdev.name[0] = 'e';
    g_alx_netdev.name[1] = 't';
    g_alx_netdev.name[2] = 'h';
    g_alx_netdev.name[3] = (char)('0' + index);
    for (size_t i = 0; i < 6u; ++i)
        g_alx_netdev.mac[i] = mac[i];
    g_alx_netdev.mtu = 1500u;
    g_alx_netdev.flags = NETDEV_FLAG_ETHERNET |
                         NETDEV_FLAG_BROADCAST;
    g_alx_netdev.link_up = alx_link_up() ? 1u : 0u;
    g_alx_netdev.ops = &g_alx_netdev_ops;

    if (!netdev_register(&g_alx_netdev)) {
        serial_print("[NET] Failed to register ALX network interface\n");
        return 0;
    }

    serial_print("[NET] Registered Qualcomm Atheros AR8171 as ");
    serial_print(g_alx_netdev.name);
    serial_print("\n");
    return 1;
}
