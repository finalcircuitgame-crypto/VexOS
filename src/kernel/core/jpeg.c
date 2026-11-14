#include "jpeg.h"
#include "picojpeg.h"
#include "heap.h"

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
