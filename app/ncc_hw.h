// (C) ncc_hw — Linux port (Korak 6). Isti API kao PSDS bare-metal
// (src/vitis/app/ncc_hw.h) da ncc_app.c/ncc_logic.c ostanu netaknuti — menja se
// SAMO implementacija (ncc_hw.c), preko ioctl+mmap umesto Xil_Out32/In32.
#ifndef NCC_HW_H
#define NCC_HW_H

#include <stdint.h>

typedef uint32_t u32;
typedef uint8_t u8;

// Bare-metal ncc_dev_t je nosio fizičke adrese (ctrl_base/mem_base); ovde nosi
// fd otvorenog /dev/nccN i mmap-ovan pokazivač na S01 (128K). NCC0/NCC1 NISU
// const (za razliku od bare-metal-a) — fd/map se popunjavaju u ncc_hw_init()
// u runtime-u, ne mogu biti statički inicijalizovani kao fizičke adrese.
typedef struct {
	const char *devpath;
	int fd;
	void *map;	/* S01 mmap, NULL dok se ncc_hw_init() ne pozove */
} ncc_dev_t;

extern ncc_dev_t NCC0, NCC1;

#define MEM_IMG_OFF  0x00000u
#define MEM_TMPL_OFF 0x08000u
#define MEM_RES_OFF  0x10000u

int  ncc_hw_init  (void);

#define NCC_IMG_MAX_WORDS  8192u
#define NCC_TMPL_MAX_WORDS 8192u

int  ncc_load_image  (const ncc_dev_t *d, const u8 *px, u32 count);
int  ncc_load_tmpl   (const ncc_dev_t *d, const u8 *px, u32 count);
void ncc_read_results(const ncc_dev_t *d, u32 *dst, u32 count);

void ncc_set_dims  (const ncc_dev_t *d, u8 iw, u8 ih, u8 tw, u8 th);
void ncc_start     (const ncc_dev_t *d);
int  ncc_wait_done (const ncc_dev_t *d, u32 timeout_us);
u32  ncc_best_score(const ncc_dev_t *d, u32 n_results, u32 *idx_out);

#endif
