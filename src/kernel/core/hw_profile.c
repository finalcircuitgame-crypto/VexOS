#include "../../include/hw_profile.h"
#include "../../include/pci.h"

static hw_profile_t g_hw;
static hw_tuning_t g_tune;

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d) {
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(leaf), "c"(subleaf));
  if (a)
    *a = eax;
  if (b)
    *b = ebx;
  if (c)
    *c = ecx;
  if (d)
    *d = edx;
}

static void hw_detect_cpu(void) {
  uint32_t a, b, c, d;
  for (int i = 0; i < 13; i++)
    g_hw.cpu_vendor[i] = 0;

  cpuid(0, 0, &a, &b, &c, &d);
  ((uint32_t *)g_hw.cpu_vendor)[0] = b;
  ((uint32_t *)g_hw.cpu_vendor)[1] = d;
  ((uint32_t *)g_hw.cpu_vendor)[2] = c;
  g_hw.cpu_vendor[12] = 0;

  for (int i = 0; i < 49; i++)
    g_hw.cpu_brand[i] = 0;

  cpuid(0x80000000u, 0, &a, &b, &c, &d);
  if (a >= 0x80000004u) {
    uint32_t *dst = (uint32_t *)g_hw.cpu_brand;
    cpuid(0x80000002u, 0, &dst[0], &dst[1], &dst[2], &dst[3]);
    cpuid(0x80000003u, 0, &dst[4], &dst[5], &dst[6], &dst[7]);
    cpuid(0x80000004u, 0, &dst[8], &dst[9], &dst[10], &dst[11]);
    g_hw.cpu_brand[48] = 0;
  }

  cpuid(1, 0, &a, &b, &c, &d);
  g_hw.has_sse3 = (uint8_t)((c >> 0) & 1u);
  g_hw.has_ssse3 = (uint8_t)((c >> 9) & 1u);
  g_hw.has_sse41 = (uint8_t)((c >> 19) & 1u);
  g_hw.has_sse42 = (uint8_t)((c >> 20) & 1u);
  g_hw.has_avx = (uint8_t)((c >> 28) & 1u);
  g_hw.has_sse = (uint8_t)((d >> 25) & 1u);
  g_hw.has_sse2 = (uint8_t)((d >> 26) & 1u);

  cpuid(7, 0, &a, &b, &c, &d);
  g_hw.has_avx2 = (uint8_t)((b >> 5) & 1u);
}

static void hw_default_tuning(void) {
  g_tune.heap_slack_bytes = 2ull * 1024ull * 1024ull;
  g_tune.target_fps = 120;
  g_tune.allow_backbuffer = 1;
}

static void hw_apply_tuning(void) {
  hw_default_tuning();

  if (g_hw.ram_usable_bytes < (64ull * 1024ull * 1024ull)) {
    g_tune.heap_slack_bytes = 512ull * 1024ull;
    g_tune.allow_backbuffer = 0;
    g_tune.target_fps = 60;
  } else if (g_hw.ram_usable_bytes < (256ull * 1024ull * 1024ull)) {
    g_tune.heap_slack_bytes = 1ull * 1024ull * 1024ull;
    g_tune.target_fps = 90;
  } else {
    g_tune.heap_slack_bytes = 4ull * 1024ull * 1024ull;
    g_tune.target_fps = 120;
  }

  if ((uint64_t)g_hw.fb_width * (uint64_t)g_hw.fb_height >=
      (1920ull * 1080ull)) {
    if (g_hw.ram_usable_bytes < (256ull * 1024ull * 1024ull)) {
      g_tune.allow_backbuffer = 0;
    }
  }
}

void hw_profile_detect_early(uint64_t ram_usable_bytes, uint32_t fb_width,
                            uint32_t fb_height, uint32_t fb_pitch) {
  for (uint32_t i = 0; i < (uint32_t)sizeof(g_hw); i++) {
    ((uint8_t *)&g_hw)[i] = 0;
  }

  g_hw.ram_usable_bytes = ram_usable_bytes;
  g_hw.fb_width = fb_width;
  g_hw.fb_height = fb_height;
  g_hw.fb_pitch = fb_pitch;

  hw_detect_cpu();
  hw_apply_tuning();
}

void hw_profile_update_pci_gpu(void) {
  g_hw.has_pci_display = 0;
  g_hw.gpu_vendor_id = 0;
  g_hw.gpu_device_id = 0;
  g_hw.gpu_subclass = 0;
  g_hw.gpu_prog_if = 0;

  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      for (uint8_t func = 0; func < 8; func++) {
        uint32_t vendor_device = pci_config_read_dword((uint8_t)bus, slot, func, 0);
        if ((uint16_t)vendor_device == 0xFFFF)
          continue;

        uint32_t class_info = pci_config_read_dword((uint8_t)bus, slot, func, 0x08);
        uint8_t class_code = (uint8_t)((class_info >> 24) & 0xFF);
        uint8_t subclass = (uint8_t)((class_info >> 16) & 0xFF);
        uint8_t prog_if = (uint8_t)((class_info >> 8) & 0xFF);

        if (class_code == 0x03) {
          g_hw.has_pci_display = 1;
          g_hw.gpu_vendor_id = (uint16_t)(vendor_device & 0xFFFFu);
          g_hw.gpu_device_id = (uint16_t)((vendor_device >> 16) & 0xFFFFu);
          g_hw.gpu_subclass = subclass;
          g_hw.gpu_prog_if = prog_if;
          hw_apply_tuning();
          return;
        }

        if (func == 0) {
          uint32_t header_type = pci_config_read_dword((uint8_t)bus, slot, func, 0x0C);
          if (!((header_type >> 16) & 0x80))
            break;
        }
      }
    }
  }
}

const hw_profile_t *hw_profile_get(void) { return &g_hw; }

const hw_tuning_t *hw_tuning_get(void) { return &g_tune; }

int hw_tuning_allow_backbuffer(uint64_t backbuffer_bytes) {
  if (!g_tune.allow_backbuffer)
    return 0;

  uint64_t min_free = 16ull * 1024ull * 1024ull;
  if (g_hw.ram_usable_bytes < min_free)
    min_free = g_hw.ram_usable_bytes;

  if (backbuffer_bytes + min_free > g_hw.ram_usable_bytes)
    return 0;

  return 1;
}
