#ifndef XHCI_RULES_H
#define XHCI_RULES_H
#include <stdint.h>
/* xHCI 5.4.8: preserve only ordinary RW fields, never echo PED/PR/LWS/W1C. */
#define XHCI_PORT_RW ((1u<<9) | (3u<<14) | (7u<<25))
#define XHCI_PORT_CHANGES (0x7fu<<17)
static inline uint32_t xhci_port_write_value(uint32_t ps, uint32_t action) {
  return (ps & XHCI_PORT_RW) | action;
}
static inline uint32_t xhci_initial_mps(uint32_t speed) {
  return speed <= 2 ? 8 : (speed == 3 ? 64 : 512);
}
/* Interval is log2(microframes). FS/LS descriptors are milliseconds;
   round down to a supported power of two so polling is never less frequent. */
static inline uint32_t xhci_interval(uint32_t speed, uint32_t binterval) {
  if (!binterval || (speed == 2 && binterval < 10)) return 0xffffffffu;
  if (speed >= 3) return binterval <= 16 ? binterval - 1 : 0xffffffffu;
  uint32_t frames = binterval, log = 0;
  while (frames >>= 1) log++;
  return log + 3;
}
#endif
