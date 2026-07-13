/** @file surface.c @brief See surface.h. */
#include "surface.h"

#include <stdlib.h>

int surface_init(surface_t *s, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return -1;
    }

    s->px = calloc((size_t) w * h, 4);
    if (s->px == NULL) {
        return -1;
    }

    s->w = w;
    s->h = h;

    return 0;
}

void surface_free(surface_t *s)
{
    free(s->px);

    s->px = NULL;
    s->w = 0;
    s->h = 0;
}
