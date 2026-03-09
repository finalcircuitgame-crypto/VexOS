#ifndef PNG_H
#define PNG_H

#include <stdint.h>

int png_decode_rgba(const uint8_t *data, uint32_t size, uint32_t *out_w,
                    uint32_t *out_h, uint8_t **out_pixels);

#endif
