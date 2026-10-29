/* icon_data.h - the program's icon, for the window and the taskbar
 * (icon_data.c, made by tools/make_icon.ps1). */
#ifndef ICON_DATA_H
#define ICON_DATA_H
#include <stddef.h>

typedef struct { int w, h; const unsigned char *rgba; size_t size; } icon_image_t;

#define ICON_NIMAGES 3          /* 48, 32 and 16 pixels */
extern const icon_image_t icon_images[ICON_NIMAGES];

#endif
