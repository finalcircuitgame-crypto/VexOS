#ifndef VEXOS_COMPOSITOR_H
#define VEXOS_COMPOSITOR_H

#include <stdint.h>
#include "../../../include/bootinfo.h"

/* Framebuffer surface composition and presentation helpers. */
void compositor_init(BootInfo *boot_info);
void compositor_blit(uint32_t *destination, const uint32_t *source,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void compositor_present(const uint32_t *scene, uint32_t width, uint32_t height);

#endif
