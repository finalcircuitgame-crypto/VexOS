#include "arp.h"
#include "ethernet.h"
#include "ipv4.h"
#include "udp.h"

#include <stdio.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint64_t g_ticks;
static uint8_t g_frame[ETHERNET_HEADER_SIZE + IPV4_MAX_PACKET_SIZE];
static size_t g_frame_length;
static unsigned g_tx_calls;
static unsigned g_rx_calls;
static uint16_t g_rx_source_port, g_rx_destination_port;
static uint8_t g_rx_payload[64];
static size_t g_rx_payload_length;

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

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void copy(uint8_t *d,const uint8_t *s,size_t n){for(size_t i=0;i<n;++i)d[i]=s[i];}

static uint16_t finish_sum(uint32_t sum) {
    while(sum>>16)sum=(sum&0xFFFFu)+(sum>>16);
    return (uint16_t)~sum;
}
static uint32_t add_sum(uint32_t sum,const uint8_t *p,size_t n) {
    size_t i=0;while(i+1<n){sum+=((uint32_t)p[i]<<8)|p[i+1];i+=2;}
    if(i<n)sum+=(uint32_t)p[i]<<8;
    return sum;
}
static uint32_t pseudo_sum(uint32_t source,uint32_t destination,uint16_t length) {
    uint8_t p[12]={(uint8_t)(source>>24),(uint8_t)(source>>16),(uint8_t)(source>>8),(uint8_t)source,
      (uint8_t)(destination>>24),(uint8_t)(destination>>16),(uint8_t)(destination>>8),(uint8_t)destination,
      0,IPV4_PROTOCOL_UDP,(uint8_t)(length>>8),(uint8_t)length};
    return add_sum(0,p,sizeof(p));
}
static size_t make_udp(uint8_t *p,uint32_t source,uint32_t destination,
                       uint16_t source_port,uint16_t destination_port,
                       const uint8_t *data,size_t data_length) {
    size_t n=8u+data_length;
    for(size_t i=0;i<n;++i)p[i]=0;
    put16(p,source_port);put16(p+2,destination_port);put16(p+4,(uint16_t)n);
    copy(p+8,data,data_length);
    uint16_t sum=finish_sum(add_sum(pseudo_sum(source,destination,(uint16_t)n),p,n));
    put16(p+6,sum?sum:0xFFFFu);
    return n;
}
static size_t make_ipv4(uint8_t *p,uint32_t source,uint32_t destination,
                        const uint8_t *payload,size_t payload_length) {
    size_t n=20u+payload_length;for(size_t i=0;i<n;++i)p[i]=0;
    p[0]=0x45;put16(p+2,(uint16_t)n);p[8]=64;p[9]=IPV4_PROTOCOL_UDP;
    put32(p+12,source);put32(p+16,destination);copy(p+20,payload,payload_length);
    put16(p+10,finish_sum(add_sum(0,p,20u)));return n;
}
static int dispatch_ipv4(net_device_t *device,const uint8_t *packet,size_t n) {
    uint8_t frame[ETHERNET_HEADER_SIZE+IPV4_MAX_PACKET_SIZE]={0};
    const uint8_t dst[6]={2,0,0,0,0,1},src[6]={2,0,0,0,0,2};
    copy(frame,dst,6);copy(frame+6,src,6);put16(frame+12,ETHERNET_TYPE_IPV4);
    copy(frame+ETHERNET_HEADER_SIZE,packet,n);
    return ethernet_dispatch_frame(device,frame,ETHERNET_HEADER_SIZE+n)==ETHERNET_DISPATCHED;
}
static void learn_peer(net_device_t *device,const uint8_t mac[6],
                       uint32_t peer_ip,uint32_t local_ip) {
    uint8_t frame[ETHERNET_HEADER_SIZE+28u]={0};
    const uint8_t broadcast[6]={255,255,255,255,255,255};
    copy(frame,broadcast,6);copy(frame+6,mac,6);put16(frame+12,ETHERNET_TYPE_ARP);
    uint8_t *a=frame+ETHERNET_HEADER_SIZE;
    put16(a,1);put16(a+2,0x0800);a[4]=6;a[5]=4;put16(a+6,1);
    copy(a+8,mac,6);put32(a+14,peer_ip);put32(a+24,local_ip);
    (void)ethernet_dispatch_frame(device,frame,sizeof(frame));
}
static void receive_udp(net_device_t *device,const ipv4_packet_info_t *info,
                        uint16_t source_port,uint16_t destination_port,
                        const uint8_t *payload,size_t length) {
    (void)device;(void)info;
    ++g_rx_calls;g_rx_source_port=source_port;g_rx_destination_port=destination_port;
    g_rx_payload_length=length;
    for(size_t i=0;i<length && i<sizeof(g_rx_payload);++i)g_rx_payload[i]=payload[i];
}

int main(void) {
    net_device_t device={0};device.mtu=1500;
    device.flags=NETDEV_FLAG_ETHERNET|NETDEV_FLAG_BROADCAST;device.link_up=1;
    const uint8_t local_mac[6]={2,0x11,0x22,0x33,0x44,0x55};
    const uint8_t peer_mac[6]={2,0xAA,0xBB,0xCC,0xDD,0xEE};
    const uint32_t local_ip=0xC0A8010Au,peer_ip=0xC0A80114u;
    const uint8_t payload[]={0x56,0x65,0x78,0x4F,0x53,0,0x7F};
    copy(device.mac,local_mac,6);

    ethernet_init();CHECK(arp_init());CHECK(ipv4_init());CHECK(udp_init());
    CHECK(ipv4_configure(&device,local_ip,0xFFFFFF00u,0));
    CHECK(udp_bind(9000u,receive_udp));CHECK(!udp_bind(9000u,receive_udp));
    CHECK(!udp_bind(0u,receive_udp));
    learn_peer(&device,peer_mac,peer_ip,local_ip);

    uint8_t udp[64],ip[128];
    size_t udp_length=make_udp(udp,peer_ip,local_ip,45000u,9000u,payload,sizeof(payload));
    size_t ip_length=make_ipv4(ip,peer_ip,local_ip,udp,udp_length);
    CHECK(dispatch_ipv4(&device,ip,ip_length));
    CHECK(g_rx_calls==1u && g_rx_source_port==45000u && g_rx_destination_port==9000u);
    CHECK(g_rx_payload_length==sizeof(payload));
    for(size_t i=0;i<sizeof(payload);++i)CHECK(g_rx_payload[i]==payload[i]);

    /* A corrupt non-zero checksum is rejected before port dispatch. */
    udp[6]^=1u;ip_length=make_ipv4(ip,peer_ip,local_ip,udp,udp_length);
    CHECK(dispatch_ipv4(&device,ip,ip_length));CHECK(g_rx_calls==1u);

    /* A valid datagram for an unbound port is counted, not delivered. */
    udp_length=make_udp(udp,peer_ip,local_ip,45000u,9001u,payload,sizeof(payload));
    ip_length=make_ipv4(ip,peer_ip,local_ip,udp,udp_length);
    CHECK(dispatch_ipv4(&device,ip,ip_length));CHECK(g_rx_calls==1u);

    /* Outbound send uses IPv4/ARP and includes a valid UDP pseudo-header checksum. */
    g_tx_calls=0;CHECK(udp_send(&device,peer_ip,9000u,45000u,payload,sizeof(payload)));
    CHECK(g_tx_calls==1u && g_frame_length==ETHERNET_HEADER_SIZE+20u+8u+sizeof(payload));
    const uint8_t *out_ip=g_frame+ETHERNET_HEADER_SIZE;
    CHECK(get32(out_ip+12u)==local_ip && get32(out_ip+16u)==peer_ip);
    CHECK(out_ip[9]==IPV4_PROTOCOL_UDP);
    const uint8_t *out_udp=out_ip+20u;
    CHECK(get16(out_udp)==9000u && get16(out_udp+2u)==45000u);
    CHECK(get16(out_udp+4u)==8u+sizeof(payload));
    CHECK(finish_sum(add_sum(pseudo_sum(local_ip,peer_ip,(uint16_t)(8u+sizeof(payload))),out_udp,8u+sizeof(payload)))==0u);
    for(size_t i=0;i<sizeof(payload);++i)CHECK(out_udp[8u+i]==payload[i]);

    udp_stats_t stats={0};udp_get_stats(&stats);
    CHECK(stats.rx_datagrams==2u && stats.rx_delivered==1u && stats.rx_bad_checksum==1u);
    CHECK(stats.rx_no_port==1u && stats.tx_datagrams==1u);
    CHECK(udp_unbind(9000u));CHECK(!udp_unbind(9000u));
    puts("PASS: UDP parsing, checksums, port dispatch, and IPv4/ARP transmission");
    return 0;
}
