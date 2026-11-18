#ifndef VEXOS_ICMP_H
#define VEXOS_ICMP_H

#include <stdint.h>

typedef struct {
    uint64_t rx_echo_requests;
    uint64_t rx_echo_replies;
    uint64_t rx_other;
    uint64_t rx_malformed;
    uint64_t rx_bad_checksum;
    uint64_t tx_echo_replies;
    uint64_t tx_failures;
} icmp_stats_t;

/* Register ICMPv4 echo handling with the IPv4 protocol dispatcher. */
int icmp_init(void);

/* Snapshot ICMP counters. */
void icmp_get_stats(icmp_stats_t *out_stats);

#endif
