#ifndef JPEG_H
#define JPEG_H

#include <stdint.h>

int jpeg_decode_rgba(const uint8_t *data, uint32_t size, uint32_t *out_w,
                     uint32_t *out_h, uint8_t **out_pixels);

#ifdef VEXOS_CI_INPUT_TEST
int jpeg_ci_self_test(void);
#endif

#endif
