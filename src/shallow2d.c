#include <string.h>
#include <math.h>
#include <omp.h>
#include <limits.h>

//ldoc on
/**
 * ## Implementation
 *
 * The actually work of computing the fluxes and speeds is done
 * by local (`static`) helper functions that take as arguments
 * pointers to all the individual fields.  This is helpful to the
 * compilers, since by specifying the `restrict` keyword, we are
 * promising that we will not access the field data through the
 * wrong pointer.  This lets the compiler do a better job with
 * vectorization.
 */


static const float g = 9.8;


static
void shallow2dv_flux(float* restrict fh,
                     float* restrict fhu,
                     float* restrict fhv,
                     float* restrict gh,
                     float* restrict ghu,
                     float* restrict ghv,
                     const float* restrict h,
                     const float* restrict hu,
                     const float* restrict hv,
                     float g,
                     int ncell)
{
    memcpy(fh, hu, ncell * sizeof(float));
    memcpy(gh, hv, ncell * sizeof(float));
    for (int i = 0; i < ncell; ++i) {
        float hi = h[i], hui = hu[i], hvi = hv[i];
        float inv_h = 1.0f/hi;
        fhu[i] = hui*hui*inv_h + (0.5f*g)*hi*hi;
        fhv[i] = hui*hvi*inv_h;
        ghu[i] = hui*hvi*inv_h;
        ghv[i] = hvi*hvi*inv_h + (0.5f*g)*hi*hi;
    }
}


static
void shallow2dv_speed(float* restrict cxy,
                      const float* restrict h,
                      const float* restrict hu,
                      const float* restrict hv,
                      float g,
                      int ncell)
{
    float cx = cxy[0];
    float cy = cxy[1];

    #pragma omp declare reduction \
    (fmaxf:float:omp_out=fmaxf(omp_out,omp_in)) \
    initializer(omp_priv=INT_MIN)

    #pragma omp parallel for reduction(fmaxf: cx,cy)
    for (int i = 0; i < ncell; ++i) {
        float hi = h[i];
        if (fabsf(hi) < 0.00001) {
            continue;
        }
        float inv_hi = 1.0f/h[i];
        float root_gh = sqrtf(g * hi);
        float cxi = fabsf(hu[i] * inv_hi) + root_gh;
        float cyi = fabsf(hv[i] * inv_hi) + root_gh;
        cx = fmaxf(cx,cxi);
        cy = fmaxf(cy,cyi);
    }
    cxy[0] = cx;
    cxy[1] = cy;
}


void shallow2d_flux(float* FU, float* GU, const float* U,
                    int ncell, int field_stride)
{
    shallow2dv_flux(FU, FU+field_stride, FU+2*field_stride,
                    GU, GU+field_stride, GU+2*field_stride,
                    U,  U +field_stride, U +2*field_stride,
                    g, ncell);
}


void shallow2d_speed(float* cxy, const float* U,
                     int ncell, int field_stride)
{
    shallow2dv_speed(cxy, U, U+field_stride, U+2*field_stride, g, ncell);
}
