// (C) ncc_hw — Linux port (Korak 6). Zamenjuje Xil_Out32/Xil_In32 direktan
// pristup registrima (PSDS bare-metal, src/vitis/app/ncc_hw.c) sa ioctl (S00
// kontrola) + mmap (S01 podaci) preko našeg ncc_accel drajvera. Nema DMA za
// podatke (odluka iz Koraka 1, isti razlog kao bare-metal: burst>2-beat
// ograničenje na axi_interconnect_0, vidi CLAUDE.md).
//
// INVARIJANTE prenete iz bare-metal-a (moraju ostati iste):
//  - poređenje skorova kao u32, NE int32 (0x80000000 je negativan kao signed)
//  - jedan piksel = jedna 32-bitna reč u S01 (NE 1 bajt) — CLAUDE.md, "Memorije S01"
//  - done_sticky se ne briše čitanjem, samo sledećim CTRL upisom (ovo sad radi
//    kernel drajver interno, isto ponašanje)

#include "ncc_hw.h"
#include "ncc_accel_uapi.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define S01_SIZE 0x20000u

ncc_dev_t NCC0 = { "/dev/ncc0", -1, NULL };
ncc_dev_t NCC1 = { "/dev/ncc1", -1, NULL };

static int open_one(ncc_dev_t *d)
{
	d->fd = open(d->devpath, O_RDWR);
	if (d->fd < 0) {
		fprintf(stderr, "open(%s): %s\n", d->devpath, strerror(errno));
		return -1;
	}

	d->map = mmap(NULL, S01_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
	if (d->map == MAP_FAILED) {
		fprintf(stderr, "mmap(%s): %s\n", d->devpath, strerror(errno));
		close(d->fd);
		d->fd = -1;
		d->map = NULL;
		return -1;
	}

	return 0;
}

int ncc_hw_init(void)
{
	if (open_one(&NCC0))
		return -1;
	if (open_one(&NCC1))
		return -1;
	return 0;
}

/* Širenje 8 -> 32 bita: u S01 je jedan piksel po 32-bitnoj reči, isto kao
 * bare-metal. Procesorski upis (mmap, bez copy_to/from_user) — analogno
 * Xil_Out32 petlji, samo preko mmap-ovane memorije umesto direktne fizičke
 * adrese. */
static int load_region(const ncc_dev_t *d, u32 off, const u8 *px, u32 count, u32 max)
{
	volatile u32 *base;
	u32 i;

	if (count > max)
		return -1;
	if (!d->map)
		return -2;

	base = (volatile u32 *)((char *)d->map + off);
	for (i = 0; i < count; i++)
		base[i] = (u32)px[i];

	return 0;
}

int ncc_load_image(const ncc_dev_t *d, const u8 *px, u32 count)
{
	return load_region(d, MEM_IMG_OFF, px, count, NCC_IMG_MAX_WORDS);
}

int ncc_load_tmpl(const ncc_dev_t *d, const u8 *px, u32 count)
{
	return load_region(d, MEM_TMPL_OFF, px, count, NCC_TMPL_MAX_WORDS);
}

void ncc_read_results(const ncc_dev_t *d, u32 *dst, u32 count)
{
	volatile u32 *base = (volatile u32 *)((char *)d->map + MEM_RES_OFF);
	u32 i;

	for (i = 0; i < count; i++)
		dst[i] = base[i];
}

void ncc_set_dims(const ncc_dev_t *d, u8 iw, u8 ih, u8 tw, u8 th)
{
	struct ncc_dims dims;

	memset(&dims, 0, sizeof(dims));
	dims.img_w = iw;
	dims.img_h = ih;
	dims.tmp_w = tw;
	dims.tmp_h = th;

	if (ioctl(d->fd, NCC_SET_DIMS, &dims) < 0)
		fprintf(stderr, "NCC_SET_DIMS(%s): %s\n", d->devpath, strerror(errno));
}

/* Upis u CTRL preko ioctl-a briše done_sticky u kernelu, isto ponašanje kao
 * bare-metal direktan upis. */
void ncc_start(const ncc_dev_t *d)
{
	if (ioctl(d->fd, NCC_START) < 0)
		fprintf(stderr, "NCC_START(%s): %s\n", d->devpath, strerror(errno));
}

/* Bare-metal API uzima timeout u mikrosekundama (istorijski, iz usleep(100)
 * petlje); drajverov NCC_WAIT_DONE ioctl uzima milisekunde. Nema IRQ-a ni
 * ovde ni tamo — kernel anketira STATUS interno (usleep_range 500-1500us). */
int ncc_wait_done(const ncc_dev_t *d, u32 timeout_us)
{
	u32 timeout_ms = timeout_us / 1000u;

	if (timeout_ms == 0)
		timeout_ms = 1;

	if (ioctl(d->fd, NCC_WAIT_DONE, &timeout_ms) < 0)
		return -1;

	return 0;
}

/* INVARIJANT 1 (isto kao bare-metal): poređenje kao u32, NE int32 —
 * 0x80000000 (NCC^2 = 1,0, savršeno poklapanje) je negativan kao signed, pa bi
 * traženje maksimuma od potpisanog tipa odbacilo baš najbolji rezultat. */
u32 ncc_best_score(const ncc_dev_t *d, u32 n_results, u32 *idx_out)
{
	volatile u32 *base = (volatile u32 *)((char *)d->map + MEM_RES_OFF);
	u32 i, mx = 0u, mi = 0u;

	for (i = 0; i < n_results; i++)
		if (base[i] > mx) {
			mx = base[i];
			mi = i;
		}

	if (idx_out)
		*idx_out = mi;

	return mx;
}
