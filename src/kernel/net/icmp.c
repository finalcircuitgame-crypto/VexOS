#include "icmp.h"

#include "ipv4.h"

#include <stddef.h>
#include <stdint.h>

#define ICMP_HEADER_SIZE       8u
#define ICMP_TYPE_ECHO_REPLY   0u
#define ICMP_TYPE_ECHO_REQUEST 8u
#define ICMP_CODE_ECHO         0u

static icmp_stats_t g_stats;

static void icmp_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void icmp_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i)
        out[i] = in[i];
}

/* Internet checksum over the complete ICMP message. A valid message returns 0. */
static uint16_t icmp_checksum(const uint8_t *bytes, size_t length) {
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

static void icmp_receive(net_device_t *device,
                         const ipv4_packet_info_t *info,
                         const uint8_t *message,
                         size_t message_length) {
    if (!device || !info || !message || message_length < ICMP_HEADER_SIZE) {
        ++g_stats.rx_malformed;
        return;
    }

    if (icmp_checksum(message, message_length) != 0u) {
        ++g_stats.rx_bad_checksum;
        return;
    }

    const uint8_t type = message[0];
    const uint8_t code = message[1];

    if (type == ICMP_TYPE_ECHO_REPLY && code == ICMP_CODE_ECHO) {
        ++g_stats.rx_echo_replies;
        return;
    }

    if (type != ICMP_TYPE_ECHO_REQUEST || code != ICMP_CODE_ECHO) {
        ++g_stats.rx_other;
        return;
    }

    ++g_stats.rx_echo_requests;

    /* Never answer an IPv4 broadcast or an unspecified source address. */
    ipv4_config_t config;
    if (info->source == 0u || !ipv4_get_config(device, &config) ||
        info->destination == 0xFFFFFFFFu ||
        info->destination == ((config.address & config.netmask) |
                              ~config.netmask)) {
        ++g_stats.rx_other;
        return;
    }

    uint8_t reply[IPV4_MAX_PACKET_SIZE - IPV4_MIN_HEADER_SIZE];
    if (message_length > sizeof(reply)) {
        ++g_stats.rx_malformed;
        return;
    }

    icmp_copy(reply, message, message_length);
    reply[0] = ICMP_TYPE_ECHO_REPLY;
    reply[1] = ICMP_CODE_ECHO;
    icmp_write_be16(reply + 2u, 0u);
    icmp_write_be16(reply + 2u, icmp_checksum(reply, message_length));

    if (!ipv4_send(device, info->source, IPV4_PROTOCOL_ICMP,
                   reply, message_length)) {
        ++g_stats.tx_failures;
        return;
    }

    ++g_stats.tx_echo_replies;
}

int icmp_init(void) {
    g_stats = (icmp_stats_t){0};
    return ipv4_register_protocol(IPV4_PROTOCOL_ICMP, icmp_receive);
}

void icmp_get_stats(icmp_stats_t *out_stats) {
    if (out_stats)
        *out_stats = g_stats;
}
