#include "arp.h"
#include "ethernet.h"
#include "icmp.h"
#include "ipv4.h"

#include <stdio.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint64_t g_ticks;
static uint8_t g_frame[ETHERNET_HEADER_SIZE + IPV4_MAX_PACKET_SIZE];
static size_t g_frame_length;
static unsigned g_tx_calls;

uint64_t Timer_Ticks(void) { return g_ticks; }

int netdev_send(net_device_t *device, const void *frame, size_t length) {
    (void)device;
    if (!frame || length > sizeof(g_frame)) return 0;
    const uint8_t *src = (const uint8_t *)frame;
    for (size_t i = 0; i < length; ++i) g_frame[i] = src[i];
    g_frame_length = length;
    ++g_tx_calls;
    return 1;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void copy(uint8_t *d, const uint8_t *s, size_t n) { for (size_t i=0;i<n;++i)d[i]=s[i]; }

static uint16_t checksum(const uint8_t *p, size_t n) {
    uint32_t sum=0; size_t i=0;
    while(i+1<n){sum+=((uint32_t)p[i]<<8)|p[i+1];i+=2;}
    if(i<n)sum+=(uint32_t)p[i]<<8;
    while(sum>>16)sum=(sum&0xFFFFu)+(sum>>16);
    return (uint16_t)~sum;
}

static size_t make_icmp(uint8_t *p, uint8_t type, uint8_t code,
                        uint16_t id, uint16_t sequence,
                        const uint8_t *data, size_t data_length) {
    size_t n=8u+data_length;
    for(size_t i=0;i<n;++i)p[i]=0;
    p[0]=type;p[1]=code;put16(p+4,id);put16(p+6,sequence);
    copy(p+8,data,data_length);
    put16(p+2,checksum(p,n));
    return n;
}

static size_t make_ipv4(uint8_t *p, uint32_t source, uint32_t destination,
                        const uint8_t *payload, size_t payload_length) {
    size_t n=20u+payload_length;
    for(size_t i=0;i<n;++i)p[i]=0;
    p[0]=0x45;put16(p+2,(uint16_t)n);p[8]=64;p[9]=IPV4_PROTOCOL_ICMP;
    put32(p+12,source);put32(p+16,destination);
    copy(p+20,payload,payload_length);
    put16(p+10,checksum(p,20u));
    return n;
}

static int dispatch_ipv4(net_device_t *device, const uint8_t *packet, size_t n) {
    uint8_t frame[ETHERNET_HEADER_SIZE+IPV4_MAX_PACKET_SIZE]={0};
    const uint8_t dst[6]={0x02,0,0,0,0,1}, src[6]={0x02,0,0,0,0,2};
    copy(frame,dst,6);copy(frame+6,src,6);put16(frame+12,ETHERNET_TYPE_IPV4);
    copy(frame+ETHERNET_HEADER_SIZE,packet,n);
    return ethernet_dispatch_frame(device,frame,ETHERNET_HEADER_SIZE+n)==ETHERNET_DISPATCHED;
}

static void learn_peer(net_device_t *device, const uint8_t peer_mac[6],
                       uint32_t peer_ip, uint32_t local_ip) {
    uint8_t frame[ETHERNET_HEADER_SIZE+28u]={0};
    const uint8_t broadcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    copy(frame,broadcast,6);copy(frame+6,peer_mac,6);put16(frame+12,ETHERNET_TYPE_ARP);
    uint8_t *a=frame+ETHERNET_HEADER_SIZE;
    put16(a,1);put16(a+2,0x0800);a[4]=6;a[5]=4;put16(a+6,1);
    copy(a+8,peer_mac,6);put32(a+14,peer_ip);put32(a+24,local_ip);
    (void)ethernet_dispatch_frame(device,frame,sizeof(frame));
}

int main(void) {
    net_device_t device={0};
    device.mtu=1500;device.flags=NETDEV_FLAG_ETHERNET|NETDEV_FLAG_BROADCAST;
    device.link_up=1;
    const uint8_t local_mac[6]={0x02,0x11,0x22,0x33,0x44,0x55};
    const uint8_t peer_mac[6]={0x02,0xAA,0xBB,0xCC,0xDD,0xEE};
    const uint32_t local_ip=0xC0A8010Au, peer_ip=0xC0A80114u;
    const uint8_t data[]={0x56,0x65,0x78,0x4F,0x53,0x00,0x7F};
    copy(device.mac,local_mac,6);

    ethernet_init();CHECK(arp_init());CHECK(ipv4_init());CHECK(icmp_init());
    CHECK(ipv4_configure(&device,local_ip,0xFFFFFF00u,0));
    learn_peer(&device,peer_mac,peer_ip,local_ip);

    uint8_t icmp[64], ip[128];
    size_t icmp_length=make_icmp(icmp,8,0,0x1234,0x0042,data,sizeof(data));
    size_t ip_length=make_ipv4(ip,peer_ip,local_ip,icmp,icmp_length);
    g_tx_calls=0;
    CHECK(dispatch_ipv4(&device,ip,ip_length));
    CHECK(g_tx_calls==1u);
    CHECK(g_frame_length==ETHERNET_HEADER_SIZE+20u+icmp_length);
    CHECK(get16(g_frame+12u)==ETHERNET_TYPE_IPV4);
    const uint8_t *out_ip=g_frame+ETHERNET_HEADER_SIZE;
    CHECK(get32(out_ip+12u)==local_ip && get32(out_ip+16u)==peer_ip);
    CHECK(out_ip[9]==IPV4_PROTOCOL_ICMP && checksum(out_ip,20u)==0u);
    const uint8_t *reply=out_ip+20u;
    CHECK(reply[0]==0u && reply[1]==0u);
    CHECK(get16(reply+4u)==0x1234u && get16(reply+6u)==0x0042u);
    CHECK(checksum(reply,icmp_length)==0u);
    for(size_t i=0;i<sizeof(data);++i)CHECK(reply[8u+i]==data[i]);

    /* Corrupt ICMP checksum: IPv4 still accepts the packet, ICMP drops it. */
    icmp[2]^=0x01u;
    ip_length=make_ipv4(ip,peer_ip,local_ip,icmp,icmp_length);
    CHECK(dispatch_ipv4(&device,ip,ip_length));
    CHECK(g_tx_calls==1u);

    /* Unsupported ICMP type is counted and never answered. */
    icmp_length=make_icmp(icmp,3,1,0,0,data,sizeof(data));
    ip_length=make_ipv4(ip,peer_ip,local_ip,icmp,icmp_length);
    CHECK(dispatch_ipv4(&device,ip,ip_length));
    CHECK(g_tx_calls==1u);

    icmp_stats_t stats={0};icmp_get_stats(&stats);
    CHECK(stats.rx_echo_requests==1u && stats.tx_echo_replies==1u);
    CHECK(stats.rx_bad_checksum==1u && stats.rx_other==1u);
    puts("PASS: ICMPv4 echo request/reply, payload preservation, checksums, and unsupported-message handling");
    return 0;
}
