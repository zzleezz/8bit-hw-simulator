/* png.h - minimal PNG writer (uncompressed deflate blocks), adapted from
 * locky3d. Pixels are RGBA8 (bytes R,G,B,A in memory). */
#ifndef PNG_H
#define PNG_H
#include <stdbool.h>
#include <stdint.h>
/* Build the CRC table now (before writing from several threads). */
void png_init(void);
bool png_write(const char *path, const uint32_t *pixels, int w, int h);
#endif
