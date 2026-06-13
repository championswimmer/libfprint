/*
 * Private copy of NBIS Bozorth3 with the minimum computable minutiae floor
 * lowered from 10 to 1 so tiny-sensor EH577 captures can still produce raw
 * pairwise scores offline.
 *
 * This is for investigation only. It intentionally does NOT change libfprint's
 * live matching path; it only gives us a way to inspect score structure on
 * captures that otherwise all collapse to score=0 under stock NBIS because the
 * images often contain fewer than 10 minutiae.
 */

#define colp    eh577_relaxed_colp
#define scols   eh577_relaxed_scols
#define fcols   eh577_relaxed_fcols
#define scolpt  eh577_relaxed_scolpt
#define fcolpt  eh577_relaxed_fcolpt
#define sc      eh577_relaxed_sc
#define yl      eh577_relaxed_yl
#define rq      eh577_relaxed_rq
#define tq      eh577_relaxed_tq
#define zz      eh577_relaxed_zz
#define rx      eh577_relaxed_rx
#define mm      eh577_relaxed_mm
#define nn      eh577_relaxed_nn
#define qq      eh577_relaxed_qq
#define rk      eh577_relaxed_rk
#define cp      eh577_relaxed_cp
#define rp      eh577_relaxed_rp
#define rf      eh577_relaxed_rf
#define cf      eh577_relaxed_cf
#define bz_y    eh577_relaxed_bz_y

#define bz_comp             eh577_relaxed_bz_comp
#define bz_find             eh577_relaxed_bz_find
#define bz_match            eh577_relaxed_bz_match
#define bz_match_score      eh577_relaxed_bz_match_score
#define bz_sift             eh577_relaxed_bz_sift
#define bozorth_probe_init  eh577_relaxed_bozorth_probe_init
#define bozorth_gallery_init eh577_relaxed_bozorth_gallery_init
#define bozorth_to_gallery  eh577_relaxed_bozorth_to_gallery
#define bozorth_main        eh577_relaxed_bozorth_main

#include <bozorth.h>

#undef MIN_COMPUTABLE_BOZORTH_MINUTIAE
#define MIN_COMPUTABLE_BOZORTH_MINUTIAE 1

#include "../libfprint/nbis/bozorth3/bz_gbls.c"
#include "../libfprint/nbis/bozorth3/bozorth3.c"
#include "../libfprint/nbis/bozorth3/bz_drvrs.c"

int EH577_RELAXED_MIN_COMPUTABLE_BOZORTH_MINUTIAE (void);

int
EH577_RELAXED_MIN_COMPUTABLE_BOZORTH_MINUTIAE (void)
{
  return MIN_COMPUTABLE_BOZORTH_MINUTIAE;
}
