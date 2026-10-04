// Which of the flat8 mappers honour row bands: see band.h.

#include "band.h"
#include "fl8tf.h"

// The inner loop initializers whose loops only write the rows of the calling
// thread's band. The scalers (fl8s.c, fl8ns.c) and the blend mapper (fl8bl.c)
// are not among them.
static void (*const band_safe[])() = {
    (void (*)())gri_clut_cpoly_init,
    (void (*)())gri_clut_poly_init,
    (void (*)())gri_clut_spoly_init,
    (void (*)())gri_clut_stpoly_init,
    (void (*)())gri_clut_tpoly_init,
    (void (*)())gri_cpoly_init,
    (void (*)())gri_opaque_clut_floor_umap_init,
    (void (*)())gri_opaque_clut_lin_umap_init,
    (void (*)())gri_opaque_clut_per_umap_hscan_init,
    (void (*)())gri_opaque_clut_per_umap_vscan_init,
    (void (*)())gri_opaque_clut_wall1d_umap_init,
    (void (*)())gri_opaque_clut_wall_umap_init,
    (void (*)())gri_opaque_floor_umap_init,
    (void (*)())gri_opaque_lin_umap_init,
    (void (*)())gri_opaque_lit_floor_umap_init,
    (void (*)())gri_opaque_lit_lin_umap_init,
    (void (*)())gri_opaque_lit_per_umap_hscan_init,
    (void (*)())gri_opaque_lit_per_umap_vscan_init,
    (void (*)())gri_opaque_lit_wall1d_umap_init,
    (void (*)())gri_opaque_lit_wall_umap_init,
    (void (*)())gri_opaque_per_umap_hscan_init,
    (void (*)())gri_opaque_per_umap_vscan_init,
    (void (*)())gri_opaque_wall_umap_init,
    (void (*)())gri_poly_init,
    (void (*)())gri_solid_poly_init,
    (void (*)())gri_spoly_init,
    (void (*)())gri_stpoly_init,
    (void (*)())gri_tluc8_opaque_clut_lin_umap_init,
    (void (*)())gri_tluc8_opaque_lin_umap_init,
    (void (*)())gri_tluc8_trans_clut_lin_umap_init,
    (void (*)())gri_tluc8_trans_lin_umap_init,
    (void (*)())gri_tpoly_init,
    (void (*)())gri_trans_clut_floor_umap_init,
    (void (*)())gri_trans_clut_lin_umap_init,
    (void (*)())gri_trans_clut_per_umap_hscan_init,
    (void (*)())gri_trans_clut_per_umap_vscan_init,
    (void (*)())gri_trans_clut_wall_umap_init,
    (void (*)())gri_trans_floor_umap_init,
    (void (*)())gri_trans_lin_umap_init,
    (void (*)())gri_trans_lit_floor_umap_init,
    (void (*)())gri_trans_lit_lin_umap_init,
    (void (*)())gri_trans_lit_per_umap_hscan_init,
    (void (*)())gri_trans_lit_per_umap_vscan_init,
    (void (*)())gri_trans_lit_wall_umap_init,
    (void (*)())gri_trans_per_umap_hscan_init,
    (void (*)())gri_trans_per_umap_vscan_init,
    (void (*)())gri_trans_solid_floor_umap_init,
    (void (*)())gri_trans_solid_lin_umap_init,
    (void (*)())gri_trans_solid_per_umap_hscan_init,
    (void (*)())gri_trans_solid_per_umap_vscan_init,
    (void (*)())gri_trans_solid_wall_umap_init,
    (void (*)())gri_trans_wall_umap_init,
};

int gr_band_safe_init(void (*init)()) {
    unsigned i;
    for (i = 0; i < sizeof(band_safe) / sizeof(band_safe[0]); i++)
        if (band_safe[i] == init)
            return 1;
    return 0;
}
