#ifndef HALO_ATLAS_SHIM_STDLIB_H
#define HALO_ATLAS_SHIM_STDLIB_H

#include <stddef.h>

static inline int abs(int v) {
    return (v < 0) ? -v : v;
}

#endif /* HALO_ATLAS_SHIM_STDLIB_H */
