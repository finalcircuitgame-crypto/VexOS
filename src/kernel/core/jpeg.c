#include "jpeg.h"
#include "picojpeg.h"
#include "heap.h"

static const uint8_t k_jpeg_baseline_fixture[] = {
  0xFFu, 0xD8u, 0xFFu, 0xE0u, 0x00u, 0x10u, 0x4Au, 0x46u, 0x49u, 0x46u, 0x00u, 0x01u,
  0x01u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0xFFu, 0xDBu, 0x00u, 0x43u,
  0x00u, 0x06u, 0x04u, 0x05u, 0x06u, 0x05u, 0x04u, 0x06u, 0x06u, 0x05u, 0x06u, 0x07u,
  0x07u, 0x06u, 0x08u, 0x0Au, 0x10u, 0x0Au, 0x0Au, 0x09u, 0x09u, 0x0Au, 0x14u, 0x0Eu,
  0x0Fu, 0x0Cu, 0x10u, 0x17u, 0x14u, 0x18u, 0x18u, 0x17u, 0x14u, 0x16u, 0x16u, 0x1Au,
  0x1Du, 0x25u, 0x1Fu, 0x1Au, 0x1Bu, 0x23u, 0x1Cu, 0x16u, 0x16u, 0x20u, 0x2Cu, 0x20u,
  0x23u, 0x26u, 0x27u, 0x29u, 0x2Au, 0x29u, 0x19u, 0x1Fu, 0x2Du, 0x30u, 0x2Du, 0x28u,
  0x30u, 0x25u, 0x28u, 0x29u, 0x28u, 0xFFu, 0xDBu, 0x00u, 0x43u, 0x01u, 0x07u, 0x07u,
  0x07u, 0x0Au, 0x08u, 0x0Au, 0x13u, 0x0Au, 0x0Au, 0x13u, 0x28u, 0x1Au, 0x16u, 0x1Au,
  0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u,
  0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u,
  0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u,
  0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u, 0x28u,
  0x28u, 0x28u, 0xFFu, 0xC0u, 0x00u, 0x11u, 0x08u, 0x00u, 0x08u, 0x00u, 0x08u, 0x03u,
  0x01u, 0x11u, 0x00u, 0x02u, 0x11u, 0x01u, 0x03u, 0x11u, 0x01u, 0xFFu, 0xC4u, 0x00u,
  0x14u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
  0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x06u, 0xFFu, 0xC4u, 0x00u, 0x14u, 0x10u,
  0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
  0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0xC4u, 0x00u, 0x15u, 0x01u, 0x01u, 0x01u,
  0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
  0x00u, 0x00u, 0x06u, 0x07u, 0xFFu, 0xC4u, 0x00u, 0x14u, 0x11u, 0x01u, 0x00u, 0x00u,
  0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
  0x00u, 0x00u, 0x00u, 0xFFu, 0xDAu, 0x00u, 0x0Cu, 0x03u, 0x01u, 0x00u, 0x02u, 0x11u,
  0x03u, 0x11u, 0x00u, 0x3Fu, 0x00u, 0x20u, 0x2Eu, 0xB4u, 0x3Fu, 0xFFu, 0xD9u,
};

typedef struct {
  const uint8_t *data;
  uint32_t size;
  uint32_t pos;
} jpeg_input_t;

static unsigned char jpeg_need_bytes(unsigned char *dst,
                                     unsigned char capacity,
                                     unsigned char *actual,
                                     void *opaque) {
  jpeg_input_t *in = (jpeg_input_t *)opaque;
  if (!in || !dst || !actual)
    return 1;

  uint32_t remaining = in->pos < in->size ? in->size - in->pos : 0u;
  uint32_t count = remaining < (uint32_t)capacity
                       ? remaining
                       : (uint32_t)capacity;
  if (count == 0u) {
    *actual = 0;
    return 1;
  }

  for (uint32_t i = 0; i < count; ++i)
    dst[i] = in->data[in->pos + i];

  in->pos += count;
  *actual = (unsigned char)count;
  return 0;
}

static uint32_t jpeg_mcu_component_offset(uint32_t x, uint32_t y) {
  uint32_t block_x = x >> 3;
  uint32_t block_y = y >> 3;
  uint32_t in_x = x & 7u;
  uint32_t in_y = y & 7u;
  return (block_y * 2u + block_x) * 64u + in_y * 8u + in_x;
}

int jpeg_decode_rgba(const uint8_t *data, uint32_t size, uint32_t *out_w,
                     uint32_t *out_h, uint8_t **out_pixels) {
  pjpeg_image_info_t info;
  jpeg_input_t input;
  uint8_t *pixels = 0;
  uint64_t pixel_count;
  uint64_t byte_count;

  if (out_pixels)
    *out_pixels = 0;
  if (out_w)
    *out_w = 0;
  if (out_h)
    *out_h = 0;

  if (!data || size < 4u || data[0] != 0xFFu || data[1] != 0xD8u ||
      !out_w || !out_h || !out_pixels)
    return 0;

  input.data = data;
  input.size = size;
  input.pos = 0;

  unsigned char status =
      pjpeg_decode_init(&info, jpeg_need_bytes, &input, 0);
  if (status != 0u)
    return 0;

  if (info.m_width <= 0 || info.m_height <= 0 ||
      info.m_width > 8192 || info.m_height > 8192 ||
      info.m_MCUWidth <= 0 || info.m_MCUHeight <= 0 ||
      info.m_MCUWidth > 16 || info.m_MCUHeight > 16 ||
      info.m_MCUSPerRow <= 0 || info.m_MCUSPerCol <= 0 ||
      !info.m_pMCUBufR)
    return 0;

  pixel_count = (uint64_t)(uint32_t)info.m_width *
                (uint64_t)(uint32_t)info.m_height;
  byte_count = pixel_count * 4ull;
  if (pixel_count > 2ull * 1024ull * 1024ull ||
      byte_count > 0xFFFFFFFFull)
    return 0;

  pixels = (uint8_t *)kmalloc((uint32_t)byte_count);
  if (!pixels)
    return 0;

  for (uint32_t my = 0; my < (uint32_t)info.m_MCUSPerCol; ++my) {
    for (uint32_t mx = 0; mx < (uint32_t)info.m_MCUSPerRow; ++mx) {
      status = pjpeg_decode_mcu();
      if (status != 0u)
        goto fail;

      for (uint32_t ly = 0; ly < (uint32_t)info.m_MCUHeight; ++ly) {
        uint32_t y = my * (uint32_t)info.m_MCUHeight + ly;
        if (y >= (uint32_t)info.m_height)
          continue;

        for (uint32_t lx = 0; lx < (uint32_t)info.m_MCUWidth; ++lx) {
          uint32_t x = mx * (uint32_t)info.m_MCUWidth + lx;
          if (x >= (uint32_t)info.m_width)
            continue;

          uint32_t src = jpeg_mcu_component_offset(lx, ly);
          uint32_t dst = (y * (uint32_t)info.m_width + x) * 4u;
          uint8_t r = info.m_pMCUBufR[src];
          uint8_t g = info.m_comps == 1 ? r : info.m_pMCUBufG[src];
          uint8_t b = info.m_comps == 1 ? r : info.m_pMCUBufB[src];

          pixels[dst + 0u] = r;
          pixels[dst + 1u] = g;
          pixels[dst + 2u] = b;
          pixels[dst + 3u] = 255u;
        }
      }
    }
  }

  *out_w = (uint32_t)info.m_width;
  *out_h = (uint32_t)info.m_height;
  *out_pixels = pixels;
  return 1;

fail:
  kfree(pixels);
  return 0;
}

#ifdef VEXOS_CI_INPUT_TEST
int jpeg_ci_self_test(void) {
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint8_t *pixels = 0;

  if (!jpeg_decode_rgba(k_jpeg_baseline_fixture,
                        (uint32_t)sizeof(k_jpeg_baseline_fixture),
                        &width, &height, &pixels)) {
    return 0;
  }

  int valid_pixels =
      width == 8u && height == 8u && pixels != 0 &&
      pixels[0] >= 180u && pixels[1] <= 80u && pixels[2] <= 70u &&
      pixels[3] == 255u;
  if (pixels)
    kfree(pixels);
  if (!valid_pixels)
    return 0;

  /* Turn the fixture's SOF0 marker into SOF2. The decoder must reject it. */
  uint8_t progressive_header[sizeof(k_jpeg_baseline_fixture)];
  for (uint32_t i = 0; i < (uint32_t)sizeof(k_jpeg_baseline_fixture); ++i)
    progressive_header[i] = k_jpeg_baseline_fixture[i];
  uint32_t sof_offset = 0u;
  for (uint32_t i = 0; i + 1u < (uint32_t)sizeof(progressive_header); ++i) {
    if (progressive_header[i] == 0xFFu &&
        progressive_header[i + 1u] == 0xC0u) {
      sof_offset = i + 1u;
      break;
    }
  }
  if (sof_offset == 0u)
    return 0;
  progressive_header[sof_offset] = 0xC2u;
  if (jpeg_decode_rgba(progressive_header,
                       (uint32_t)sizeof(progressive_header),
                       &width, &height, &pixels)) {
    if (pixels)
      kfree(pixels);
    return 0;
  }
  if (pixels || width != 0u || height != 0u)
    return 0;

  if (jpeg_decode_rgba(k_jpeg_baseline_fixture, 3u, &width, &height,
                       &pixels)) {
    if (pixels)
      kfree(pixels);
    return 0;
  }
  if (pixels || width != 0u || height != 0u)
    return 0;

  return 1;
}
#endif
