#include "netdev.h"

static net_device_t *g_devices[NETDEV_MAX_DEVICES];
static size_t g_device_count;
static netdev_receive_fn g_receive_handler;

static uint64_t netdev_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void netdev_irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

static int netdev_name_is_valid(const char name[NETDEV_NAME_LENGTH]) {
    if (!name || name[0] == '\0')
        return 0;

    for (size_t i = 0; i < NETDEV_NAME_LENGTH; ++i) {
        if (name[i] == '\0')
            return 1;
    }
    return 0;
}

static int netdev_is_registered(const net_device_t *device) {
    if (!device)
        return 0;

    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i] == device)
            return 1;
    }
    return 0;
}

void netdev_init(void) {
    uint64_t flags = netdev_irq_save();
    for (size_t i = 0; i < NETDEV_MAX_DEVICES; ++i)
        g_devices[i] = NULL;
    g_device_count = 0;
    g_receive_handler = NULL;
    netdev_irq_restore(flags);
}

int netdev_register(net_device_t *device) {
    if (!device || !device->ops || !device->ops->transmit ||
        !device->ops->poll || !netdev_name_is_valid(device->name) ||
        device->mtu < 68u || device->mtu > NETDEV_MAX_MTU)
        return 0;

    uint64_t flags = netdev_irq_save();
    if (netdev_is_registered(device) ||
        g_device_count >= NETDEV_MAX_DEVICES) {
        netdev_irq_restore(flags);
        return 0;
    }

    for (size_t i = 0; i < g_device_count; ++i) {
        const char *a = g_devices[i]->name;
        const char *b = device->name;
        size_t j = 0;
        while (j < NETDEV_NAME_LENGTH && a[j] == b[j] &&
               a[j] != '\0')
            ++j;
        if (j < NETDEV_NAME_LENGTH && a[j] == b[j]) {
            netdev_irq_restore(flags);
            return 0;
        }
    }

    device->index = (uint32_t)g_device_count;
    device->flags |= NETDEV_FLAG_REGISTERED;
    g_devices[g_device_count++] = device;
    netdev_irq_restore(flags);
    return 1;
}

size_t netdev_count(void) {
    return g_device_count;
}

net_device_t *netdev_get(size_t index) {
    if (index >= g_device_count)
        return NULL;
    return g_devices[index];
}

void netdev_set_receive_handler(netdev_receive_fn handler) {
    uint64_t flags = netdev_irq_save();
    g_receive_handler = handler;
    netdev_irq_restore(flags);
}

int netdev_send(net_device_t *device, const void *frame, size_t length) {
    if (!netdev_is_registered(device) || !frame ||
        length < NETDEV_ETH_HEADER_SIZE ||
        length > (size_t)device->mtu + NETDEV_ETH_HEADER_SIZE) {
        if (device)
            ++device->tx_dropped;
        return 0;
    }

    if (!device->link_up) {
        ++device->tx_dropped;
        return 0;
    }

    if (!device->ops->transmit(device, frame, length)) {
        ++device->tx_dropped;
        return 0;
    }

    ++device->tx_packets;
    return 1;
}

void netdev_receive(net_device_t *device, const void *frame, size_t length) {
    if (!netdev_is_registered(device) || !frame ||
        length < NETDEV_ETH_HEADER_SIZE ||
        length > (size_t)device->mtu + NETDEV_ETH_HEADER_SIZE) {
        if (device)
            ++device->rx_dropped;
        return;
    }

    ++device->rx_packets;
    netdev_receive_fn handler = g_receive_handler;
    if (handler)
        handler(device, (const uint8_t *)frame, length);
}

void netdev_poll_all(uint32_t budget_per_device) {
    if (budget_per_device == 0)
        return;

    for (size_t i = 0; i < g_device_count; ++i) {
        net_device_t *device = g_devices[i];
        if (device && device->ops && device->ops->poll)
            device->ops->poll(device, budget_per_device);
    }
}
