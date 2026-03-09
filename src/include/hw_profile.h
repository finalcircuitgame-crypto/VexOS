#ifndef HW_PROFILE_H
#define HW_PROFILE_H

#include <stdint.h>

typedef struct {
  char cpu_vendor[13];
  char cpu_brand[49];

  uint8_t has_sse;
  uint8_t has_sse2;
  uint8_t has_sse3;
  uint8_t has_ssse3;
  uint8_t has_sse41;
  uint8_t has_sse42;
  uint8_t has_avx;
  uint8_t has_avx2;

  uint64_t ram_usable_bytes;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pitch;

  uint8_t has_pci_display;
  uint16_t gpu_vendor_id;
  uint16_t gpu_device_id;
  uint8_t gpu_subclass;
  uint8_t gpu_prog_if;
} hw_profile_t;

typedef struct {
  uint64_t heap_slack_bytes;
  uint32_t target_fps;
  uint8_t allow_backbuffer;
} hw_tuning_t;

void hw_profile_detect_early(uint64_t ram_usable_bytes, uint32_t fb_width,
                             uint32_t fb_height, uint32_t fb_pitch);
void hw_profile_update_pci_gpu(void);
const hw_profile_t *hw_profile_get(void);
const hw_tuning_t *hw_tuning_get(void);

int hw_tuning_allow_backbuffer(uint64_t backbuffer_bytes);

#endif
