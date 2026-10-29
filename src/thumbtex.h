/* thumbtex.h - pictures shown in the interface's windows (the screenshot
 * diagnosis, the atlas browser) as textures.
 *
 * A texture freed while a frame is being built may already be in that
 * frame's draw commands: snk_render() would draw it as plain white. So
 * thumbtex_free() only hides it, and the textures go when
 * thumbtex_collect() runs, after snk_render(). */
#ifndef THUMBTEX_H
#define THUMBTEX_H
#include "sokol_headers.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct { sg_image img; sg_view view; snk_image_t snk; bool live; int w, h; } thumbtex_t;

/* T from W x H RGBA pixels PX (nothing if there are none). */
void thumbtex_make(thumbtex_t *t, const uint32_t *px, int w, int h);
/* T no longer shown; freed at the next thumbtex_collect(). */
void thumbtex_free(thumbtex_t *t);
/* The pictures shown as T, for nk_image(). */
struct nk_image thumbtex_image(const thumbtex_t *t);
/* Free the textures thumbtex_free() let go (after snk_render()). */
void thumbtex_collect(void);
/* Free them all and the sampler (before snk_shutdown()). */
void thumbtex_shutdown(void);

#endif
