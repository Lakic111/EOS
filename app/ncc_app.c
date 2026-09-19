// (C) ncc_app — Linux port (Korak 6) PSDS src/vitis/app/ncc_app.c.
// Logika skeniranja (run_pair, app_scan_square: grubi-pa-fini pregled,
// top-2 selekcija, INVARIJANT 1 poređenje kao u32) je NETAKNUTA.
// Jedina izmena: profiling sa XTime (Xilinx) -> clock_gettime (Linux), i
// xil_printf -> fprintf(stderr) za TIMEOUT poruke.

#include "ncc_app.h"
#include "ncc_hw.h"
#include "ncc_logic.h"
#include "data.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Merenje vremena po fazama obrade, u nanosekundama (bare-metal je koristio
 * tikove globalnog tajmera preko XTime_GetTime; ovde clock_gettime). */
uint64_t prof_load = 0;   /* upis slike i sablona u S01 */
uint64_t prof_read = 0;   /* citanje mape rezultata */
uint64_t prof_wait = 0;   /* cekanje da jezgro zavrsi */
u32 prof_words_load = 0;
u32 prof_words_read = 0;

static inline uint64_t ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define PROF_BEGIN	uint64_t _t0 = ns_now()
#define PROF_END(acc)	(acc) += (ns_now() - _t0)

#define COARSE_TOPK 2
#define TIMEOUT_US  2000000u

static u8 seg_full  [SEG_W * SEG_H];        /* 8100 */
static u8 seg_coarse[(SEG_W/2) * (SEG_H/2)];/* 2025 */

/* Pokrece par sablona (drugi je opcion) i vraca njihove najbolje skorove. */
static void run_pair(int ta, int tb,
                     const unsigned char *const *TP, const int *TW, const int *TH,
                     int iw, int ih, u32 *sa, u32 *sb) {
    int rwa = iw - TW[ta] + 1, rha = ih - TH[ta] + 1;
    *sa = 0u; if (sb) *sb = 0u;

    { PROF_BEGIN; ncc_load_tmpl(&NCC0, TP[ta], (u32)(TW[ta] * TH[ta]));
      PROF_END(prof_load); prof_words_load += (u32)(TW[ta] * TH[ta]); }
    ncc_set_dims (&NCC0, (u8)iw, (u8)ih, (u8)TW[ta], (u8)TH[ta]);
    if (tb >= 0) {
        { PROF_BEGIN; ncc_load_tmpl(&NCC1, TP[tb], (u32)(TW[tb] * TH[tb]));
          PROF_END(prof_load); prof_words_load += (u32)(TW[tb] * TH[tb]); }
        ncc_set_dims (&NCC1, (u8)iw, (u8)ih, (u8)TW[tb], (u8)TH[tb]);
    }

    ncc_start(&NCC0);
    if (tb >= 0) ncc_start(&NCC1);

    { PROF_BEGIN;
      if (ncc_wait_done(&NCC0, TIMEOUT_US)) { fprintf(stderr, "ncc0 TIMEOUT\n"); return; }
      PROF_END(prof_wait); }
    { PROF_BEGIN; *sa = ncc_best_score(&NCC0, (u32)(rwa * rha), NULL);
      PROF_END(prof_read); prof_words_read += (u32)(rwa * rha); }

    if (tb >= 0) {
        int rwb = iw - TW[tb] + 1, rhb = ih - TH[tb] + 1;
        if (ncc_wait_done(&NCC1, TIMEOUT_US)) { fprintf(stderr, "ncc1 TIMEOUT\n"); return; }
        { PROF_BEGIN; *sb = ncc_best_score(&NCC1, (u32)(rwb * rhb), NULL);
          PROF_END(prof_read); prof_words_read += (u32)(rwb * rhb); }
    }
}

square_t app_scan_square(int m, int n) {
    square_t r; int x, y, i, k, nc = 0, ncand;
    int cands[6], topk[COARSE_TOPK];
    u32 cscore[6];
    int cw = SEG_W / 2, ch = SEG_H / 2;
    int mean, sx = n * SEG_W, sy = m * SEG_H;

    r.occupied = 0; r.is_white = 0; r.best_tmpl = -1; r.best_score = 0u;

    if (logic_is_empty(BOARD_IMG, BOARD_W, sx, sy)) return r;
    r.occupied = 1;

    mean = logic_center_mean(BOARD_IMG, BOARD_W, sx, sy);
    r.is_white = logic_is_white(mean);
    for (i = 0; i < 6; i++) cands[nc++] = r.is_white ? i : 6 + i;
    ncand = nc;

    /* Izrezi segment iz table (stride 720) i napravi grubu verziju. */
    for (y = 0; y < SEG_H; y++)
        for (x = 0; x < SEG_W; x++)
            seg_full[y*SEG_W + x] = BOARD_IMG[(sy + y)*BOARD_W + (sx + x)];
    logic_downsample2x(seg_full, SEG_W, SEG_H, seg_coarse);

    /* --- GRUBI SCREEN: segment se upisuje JEDNOM, sabloni se menjaju --- */
    { PROF_BEGIN; ncc_load_image(&NCC0, seg_coarse, (u32)(cw * ch));
      ncc_load_image(&NCC1, seg_coarse, (u32)(cw * ch));
      PROF_END(prof_load); prof_words_load += (u32)(2 * cw * ch); }
    for (k = 0; k < ncand; k += 2) {
        int ta = cands[k];
        int tb = (k + 1 < ncand) ? cands[k + 1] : -1;
        u32 sa, sb;
        run_pair(ta, tb, TMPL_COARSE, TMPL_COARSE_W, TMPL_COARSE_H, cw, ch, &sa, &sb);
        cscore[k] = sa;
        if (tb >= 0) cscore[k + 1] = sb;
    }

    /* top-2 po grubom skoru (selekcija, ne sortiranje -- lista ima 6 elemenata) */
    for (i = 0; i < COARSE_TOPK; i++) {
        int best = -1; u32 bs = 0u; int j;
        for (j = 0; j < ncand; j++) {
            int already = 0, q;
            for (q = 0; q < i; q++) if (topk[q] == cands[j]) already = 1;
            if (!already && (best < 0 || cscore[j] > bs)) { best = cands[j]; bs = cscore[j]; }
        }
        topk[i] = best;
    }

    /* --- FINA POTVRDA nad top-2 --- */
    { PROF_BEGIN; ncc_load_image(&NCC0, seg_full, (u32)(SEG_W * SEG_H));
      ncc_load_image(&NCC1, seg_full, (u32)(SEG_W * SEG_H));
      PROF_END(prof_load); prof_words_load += (u32)(2 * SEG_W * SEG_H); }
    {
        u32 sa, sb;
        run_pair(topk[0], topk[1], TMPL_FULL, TMPL_FULL_W, TMPL_FULL_H,
                 SEG_W, SEG_H, &sa, &sb);
        /* INVARIJANT 1: poredjenje kao u32. */
        if (sa >= sb) { r.best_score = sa; r.best_tmpl = topk[0]; }
        else          { r.best_score = sb; r.best_tmpl = topk[1]; }
    }
    return r;
}
