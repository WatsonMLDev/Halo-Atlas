#ifndef HALO_ATLAS_SHIM_MATH_H
#define HALO_ATLAS_SHIM_MATH_H

static inline double floor(double v) {
    int i = (int)v;
    return (v < (double)i) ? (double)(i - 1) : (double)i;
}

static inline double sqrt(double v) {
    if (v <= 0.0) {
        return 0.0;
    }
    double x = v;
    for (int i = 0; i < 20; ++i) {
        x = 0.5 * (x + v / x);
    }
    return x;
}

#endif /* HALO_ATLAS_SHIM_MATH_H */
