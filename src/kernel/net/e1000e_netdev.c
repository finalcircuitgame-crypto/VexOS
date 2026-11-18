#include "e1000e_netdev.h"

#include "e1000e.h"
#include "netdev.h"

#include <stddef.h>
#include <stdint.h>

void serial_print(const char *str);

#define E1000E_NETDEV_FRAME_BYTES 1518u

static net_device_t g_e1000e_netdev;
static uint8_t g_rx_frame[E1000E_NETDEV_FRAME_BYTES];

static int e1000e_netdev_transmit(net_device_t *device,
                                  const void *frame, size_t length) {
    (void)device;
    if (!frame || length > E1000E_NETDEV_FRAME_BYTES)
        return 0;
    return e1000e_send(frame, (uint16_t)length);
}

static void e1000e_netdev_poll(net_device_t *device, uint32_t budget) {
    if (!device || budget == 0u || !e1000e_is_ready())
        return;

    device->link_up = e1000e_link_up() ? 1u : 0u;
    for (uint32_t work = 0; work < budget; ++work) {
        uint16_t frame_length = 0;
        int received = e1000e_receive(g_rx_frame,
                                      (uint16_t)sizeof(g_rx_frame),
                                      &frame_length);
        if (received == 0)
            break;
        if (received < 0)
            continue;

        netdev_receive(device, g_rx_frame, frame_length);
    }
}

static const netdev_ops_t g_e1000e_netdev_ops = {
    .transmit = e1000e_netdev_transmit,
    .poll = e1000e_netdev_poll
};

int e1000e_netdev_attach(void) {
    if (!e1000e_is_ready()) {
        serial_print("[NET] E1000e is not initialized; no network interface registered\n");
        return 0;
    }

    const uint8_t *mac = e1000e_mac_address();
    if (!mac) {
        serial_print("[NET] E1000e did not provide a MAC address\n");
        return 0;
    }

    g_e1000e_netdev = (net_device_t){0};
    g_e1000e_netdev.name[0] = 'e';
    g_e1000e_netdev.name[1] = 't';
    g_e1000e_netdev.name[2] = 'h';
    g_e1000e_netdev.name[3] = '0';
    for (size_t i = 0; i < 6u; ++i)
        g_e1000e_netdev.mac[i] = mac[i];
    g_e1000e_netdev.mtu = 1500u;
    g_e1000e_netdev.flags = NETDEV_FLAG_ETHERNET |
                            NETDEV_FLAG_BROADCAST;
    g_e1000e_netdev.link_up = e1000e_link_up() ? 1u : 0u;
    g_e1000e_netdev.ops = &g_e1000e_netdev_ops;

    if (!netdev_register(&g_e1000e_netdev)) {
        serial_print("[NET] Failed to register E1000e as eth0\n");
        return 0;
    }

    serial_print("[NET] Registered E1000e as eth0; generic Ethernet stack attached\n");
    return 1;
}
