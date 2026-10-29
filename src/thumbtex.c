/* thumbtex.c - pictures as textures, freed after the frame (see thumbtex.h). */
#include "thumbtex.h"
#include <stdlib.h>

static sg_sampler smp;
static thumbtex_t *dead;        /* let go of, not freed yet */
static int ndead, cap;

void thumbtex_make(thumbtex_t *t, const uint32_t *px, int w, int h) {
    if (t->live || !px || w <= 0 || h <= 0) return;
    if (!smp.id)
        smp = sg_make_sampler(&(sg_sampler_desc){ .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_NEAREST,
                                                  .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    t->img = sg_make_image(&(sg_image_desc){
        .width = w, .height = h, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { px, (size_t)w * h * sizeof(uint32_t) } });
    t->view = sg_make_view(&(sg_view_desc){ .texture.image = t->img });
    t->snk = snk_make_image(&(snk_image_desc_t){ .texture_view = t->view, .sampler = smp });
    t->w = w;
    t->h = h;
    t->live = true;
}

void thumbtex_free(thumbtex_t *t) {
    if (!t->live) return;
    if (ndead == cap) {
        int n = cap ? cap * 2 : 64;
        thumbtex_t *d = realloc(dead, (size_t)n * sizeof(*d));
        if (!d) return;         /* leaks this one rather than freeing it too soon */
        dead = d;
        cap = n;
    }
    dead[ndead++] = *t;
    t->live = false;
}

struct nk_image thumbtex_image(const thumbtex_t *t) { return nk_image_handle(snk_nkhandle(t->snk)); }

void thumbtex_collect(void) {
    for (int i = 0; i < ndead; i++) {
        snk_destroy_image(dead[i].snk);
        sg_destroy_view(dead[i].view);
        sg_destroy_image(dead[i].img);
    }
    ndead = 0;
}

void thumbtex_shutdown(void) {
    thumbtex_collect();
    free(dead);
    dead = NULL;
    cap = 0;
    if (smp.id) sg_destroy_sampler(smp);
    smp.id = 0;
}
