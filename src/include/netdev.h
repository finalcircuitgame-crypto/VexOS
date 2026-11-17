#ifndef VEXOS_NETDEV_H
#define VEXOS_NETDEV_H

#include <stddef.h>
#include <stdint.h>

#define NETDEV_NAME_LENGTH       16u
#define NETDEV_MAX_DEVICES        8u
#define NETDEV_MAX_MTU         9000u
#define NETDEV_ETH_HEADER_SIZE   14u

#define NETDEV_FLAG_ETHERNET   (1u << 0)
#define NETDEV_FLAG_BROADCAST  (1u << 1)
#define NETDEV_FLAG_REGISTERED (1u << 2)

struct net_device;

typedef struct net_device net_device_t;

typedef struct {
    int (*transmit)(net_device_t *device, const void *frame, size_t length);
    void (*poll)(net_device_t *device, uint32_t budget);
} netdev_ops_t;

/*
 * Network-device contract:
 * - Drivers own their hardware state and packet buffers.
 * - transmit() copies or consumes the frame before returning.
 * - poll() runs in kernel task context, not in an interrupt handler.
 * - receive callbacks receive a borrowed frame pointer valid only until the
 *   callback returns; protocol code must copy data it needs to retain.
 */
struct net_device {
    char name[NETDEV_NAME_LENGTH];
    uint8_t mac[6];
    uint16_t mtu;
    uint16_t reserved;
    uint32_t flags;
    uint32_t index;
    uint8_t link_up;
    uint8_t reserved_state[7];
    const netdev_ops_t *ops;
    void *driver_data;

    uint64_t rx_packets;
    uint64_t rx_dropped;
    uint64_t tx_packets;
    uint64_t tx_dropped;
};

typedef void (*netdev_receive_fn)(net_device_t *device,
                                  const uint8_t *frame,
                                  size_t length);

void netdev_init(void);
int netdev_register(net_device_t *device);
size_t netdev_count(void);
net_device_t *netdev_get(size_t index);

void netdev_set_receive_handler(netdev_receive_fn handler);
int netdev_send(net_device_t *device, const void *frame, size_t length);
void netdev_receive(net_device_t *device, const void *frame, size_t length);
void netdev_poll_all(uint32_t budget_per_device);

#endif
