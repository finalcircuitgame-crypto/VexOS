#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/kernel/drivers/usb/xhci/xhci_internal.h"
static unsigned keys;
void serial_print(const char *s) { (void)s; }
void *PMM_AllocatePage(void) { return aligned_alloc(4096,4096); }
void PMM_FreePage(void *p) { free(p); }
void input_kbd_push_char(char c) { assert(c=='a'); keys++; }
void input_pointer_add_rel(int32_t x,int32_t y,uint32_t buttons) { (void)x;(void)y;(void)buttons; }
void input_pointer_set_abs_scaled(int32_t x,int32_t y,int32_t mx,int32_t my,uint32_t b) { (void)x;(void)y;(void)mx;(void)my;(void)b; }
static void reset_events(void) { memset(event_ring,0,4096);event_ring_index=0;event_ring_cycle=1; }
static xhci_trb_t event(uint64_t ptr,unsigned cc,unsigned slot,unsigned dci,unsigned type,unsigned residual) {
 return (xhci_trb_t){ptr,(cc<<24)|residual,(slot<<24)|(dci<<16)|(type<<10)|1};
}
int main(void) {
 /* Regression: clearing reset/change flags must never disable a USB2 port. */
 uint32_t ps=(1u<<0)|(1u<<1)|(1u<<4)|(3u<<10)|XHCI_PORT_CHANGES|(1u<<9)|(7u<<25);
 uint32_t write=xhci_port_write_value(ps,ps & XHCI_PORT_CHANGES);
 assert(!(write & ((1u<<1)|(1u<<4)|(15u<<5)|(1u<<16))));
 assert((write & XHCI_PORT_CHANGES)==XHCI_PORT_CHANGES);
 assert(xhci_port_write_value(ps,1u<<4)&(1u<<4));
 assert(xhci_initial_mps(1)==8 && xhci_initial_mps(2)==8 && xhci_initial_mps(3)==64 && xhci_initial_mps(4)==512);
 assert(xhci_interval(2,9)==0xffffffffu);
 assert(xhci_interval(2,10)==6 && xhci_interval(1,1)==3 && xhci_interval(1,255)==10);
 assert(xhci_interval(3,1)==0 && xhci_interval(4,16)==15 && xhci_interval(3,17)==0xffffffffu);
 /* Run the production enqueue code through FOUR full ring traversals. */
 xhci_trb_t *ring=aligned_alloc(4096,4096);memset(ring,0,4096);
 unsigned index=0;uint8_t cycle=1;
 xhci_trb_t t={0,0,TRB_TYPE_NORMAL<<10};
 for(unsigned round=0;round<4;round++) {
   for(unsigned i=0;i<255;i++) { xhci_ring_enqueue(ring,&index,&cycle,&t);assert((ring[i].control&1)==(1-(round%2))); }
   assert(index==0 && cycle==(round%2));
   assert((ring[255].control&1)==(1-round%2));assert(ring[255].control&(1u<<1));
 }
 runtime_regs=calloc(1,sizeof(*runtime_regs));op_regs=calloc(1,sizeof(*op_regs));doorbell_regs=calloc(256,4);
 event_ring=aligned_alloc(4096,4096);reset_events();
 xhci_device_state_t *dev=&g_devs[1];memset(dev,0,sizeof(*dev));dev->slot_id=1;dev->intr_dci=3;
 dev->intr_ring=ring;dev->intr_mps=8;dev->hid_proto=1;dev->intr_buf=calloc(1,4096);dev->intr_buf[2]=4;
 dev->ep0_setup_trb=0x1000;dev->ep0_data_trb=0x1010;dev->ep0_status_trb=0x1020;dev->control_length=18;
 event_ring[0]=event(0x3000,1,1,3,32,0); /* same slot, unrelated interrupt endpoint */
 event_ring[1]=event(0x1010,13,1,1,32,10); /* short data; status still pending */
 event_ring[2]=event(0x1020,1,1,1,32,0);
 unsigned cc=0;assert(xhci_wait_for_transfer_event(1,&cc));assert(cc==1 && dev->control_length==8);
 assert(keys==1 && event_ring_index==3 && dev->intr_pending);
 reset_events();xhci_trb_t cmd={0,0,9u<<10};xhci_last_command=(uint64_t)&cmd;
 event_ring[0]=event(0x4000,1,5,0,33,0); /* stale command completion */
 event_ring[1]=event((uint64_t)&cmd,1,2,0,33,0);
 unsigned slot=0;assert(xhci_wait_for_command_completion(&slot,&cc));assert(slot==2 && event_ring_index==2);
 reset_events();event_ring_index=255;event_ring[255]=event(0,1,0,0,34,0);
 xhci_trb_t out;assert(xhci_poll_event(&out));assert(event_ring_index==0 && event_ring_cycle==0);
 /* Exercise the production control builders across a Link TRB. */
 dev->ep0_ring=aligned_alloc(4096,4096);memset(dev->ep0_ring,0,4096);
 dev->ep0_index=254;dev->ep0_cycle=1;dev->ep0_failed=0;
 uint8_t descriptor[18]={0};reset_events();
 event_ring[0]=event((uint64_t)&dev->ep0_ring[0],13,1,1,32,10);
 event_ring[1]=event((uint64_t)&dev->ep0_ring[1],1,1,1,32,0);
 assert(xhci_ep0_control_in(dev,0x0012000001000680ull,descriptor,18));
 assert(dev->control_length==8);
 assert(((dev->ep0_ring[254].control>>16)&3)==3 && !(dev->ep0_ring[254].control&(1u<<5)));
 assert((dev->ep0_ring[255].control&(1u<<4)) && (dev->ep0_ring[255].control&1));
 assert((dev->ep0_ring[0].control&(1u<<16)) && !(dev->ep0_ring[0].control&(1u<<5)));
 assert((dev->ep0_ring[1].control&(1u<<5)) && !(dev->ep0_ring[1].control&(1u<<16)));
 reset_events();event_ring[0]=event((uint64_t)&dev->ep0_ring[3],1,1,1,32,0);
 assert(xhci_ep0_control_no_data_out(dev,0x0000000000010900ull));
 assert(((dev->ep0_ring[2].control>>16)&3)==0 && (dev->ep0_ring[3].control&(1u<<16)));
 /* A stalled optional control request resets EP0 and sets a safe dequeue pointer. */
 command_ring=aligned_alloc(4096,4096);memset(command_ring,0,4096);
 command_ring_index=0;command_ring_cycle=1;reset_events();
 event_ring[0]=event((uint64_t)&dev->ep0_ring[5],6,1,1,32,0);
 event_ring[1]=event((uint64_t)&command_ring[0],1,1,0,33,0);
 event_ring[2]=event((uint64_t)&command_ring[1],1,1,0,33,0);
 assert(!xhci_ep0_control_in(dev,0x0012000001000680ull,descriptor,18));
 assert(!dev->ep0_failed && trb_type(command_ring[0].control)==14 && trb_type(command_ring[1].control)==16);
 assert(command_ring[1].data==((uint64_t)&dev->ep0_ring[7] | dev->ep0_cycle));
 reset_events();xhci_running=1;assert(!xhci_wait_for_command_completion(&slot,&cc));assert(!xhci_running);
 puts("PASS: PORTSC safety, LS/FS/HS/SS EP0 and intervals, four producer wraps, event wrap, interleaved HID, short-status completion, command correlation, bounded timeout");
}
