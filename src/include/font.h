#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// A simple 8x16 bitmap font (partial example for 'A'-'Z', '0'-'9', etc.)
// In a real OS, you'd load a .psf file, but we'll hardcode a few chars for now.
extern uint8_t font_basic[128][16];

#endif
