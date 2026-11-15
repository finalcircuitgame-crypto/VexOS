#ifndef VEXOS_COMPOSITOR_H
#define VEXOS_COMPOSITOR_H

#include <stdint.h>
#include "../../../include/bootinfo.h"

/* Framebuffer surface composition and presentation helpers. */
void compositor_init(BootInfo *boot_info);
int compositor_enable_pageflip(void);
int compositor_pageflip_active(void);
void compositor_wait_vsync(void);
uint32_t *compositor_frontbuffer(void);
uint32_t *compositor_backbuffer(void);

void compositor_blit(uint32_t *destination, const uint32_t *source,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void compositor_present_rect(const uint32_t *scene,
                             uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height);
void compositor_present(const uint32_t *scene, uint32_t width, uint32_t height);

#endif
