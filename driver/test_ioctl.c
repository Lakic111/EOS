// (C) ncc_accel bring-up test — proverava SET_DIMS/START/WAIT_DONE ciklus na
// pravom hardveru. Namerno mali, sintetički ulaz (4x4 slika, 2x2 šablon) —
// cilj je proveriti da kontrolni put radi (busy/done ponašanje), ne da se
// dobije smislen NCC rezultat (to je Korak 6, sa pravim šahovskim slikama).
//
// Upotreba: ./test_ioctl [/dev/ncc0]

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

#include "ncc_accel_uapi.h"

#define MAP_SIZE 0x20000	/* ceo S01 region, 128K */

static double elapsed_ms(struct timespec *a, struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000.0 +
	       (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
	const char *devpath = argc > 1 ? argv[1] : "/dev/ncc0";
	int fd;
	void *map;
	volatile uint32_t *img, *tmpl, *res;
	struct ncc_dims dims;
	uint32_t timeout_ms = 500;	/* referentni slucaj ocekuje ~104ms, ovo je izdasno */
	struct timespec t0, t1;
	int ret;

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", devpath, strerror(errno));
		return 1;
	}
	printf("otvoren %s (fd=%d)\n", devpath, fd);

	map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("mmap uspesan, %d B na %p\n", MAP_SIZE, map);

	img = (volatile uint32_t *)((char *)map + NCC_BRAM_IMG_OFF);
	tmpl = (volatile uint32_t *)((char *)map + NCC_BRAM_TMP_OFF);
	res = (volatile uint32_t *)((char *)map + NCC_BRAM_RES_OFF);

	// Mala sintetička slika 4x4 i šablon 2x2 — vrednosti nisu bitne za ovaj
	// test (proveravamo kontrolni put, ne tačnost NCC racuna).
	memset((void *)img, 0, 4 * 4 * sizeof(uint32_t));
	memset((void *)tmpl, 0, 2 * 2 * sizeof(uint32_t));
	for (int i = 0; i < 16; i++)
		img[i] = (uint32_t)(i * 10);
	for (int i = 0; i < 4; i++)
		tmpl[i] = (uint32_t)(i * 10);

	memset(&dims, 0, sizeof(dims));
	dims.img_w = 4;
	dims.img_h = 4;
	dims.tmp_w = 2;
	dims.tmp_h = 2;

	ret = ioctl(fd, NCC_SET_DIMS, &dims);
	if (ret < 0) {
		fprintf(stderr, "NCC_SET_DIMS: %s (errno=%d)\n", strerror(errno), errno);
		goto out;
	}
	printf("NCC_SET_DIMS OK (img 4x4, tmp 2x2)\n");

	clock_gettime(CLOCK_MONOTONIC, &t0);

	ret = ioctl(fd, NCC_START);
	if (ret < 0) {
		fprintf(stderr, "NCC_START: %s (errno=%d)\n", strerror(errno), errno);
		goto out;
	}
	printf("NCC_START OK\n");

	ret = ioctl(fd, NCC_WAIT_DONE, &timeout_ms);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (ret < 0) {
		fprintf(stderr, "NCC_WAIT_DONE: %s (errno=%d) posle %.2f ms\n",
			strerror(errno), errno, elapsed_ms(&t0, &t1));
		if (errno == ETIMEDOUT)
			fprintf(stderr,
				"  -> TIMEOUT: proveriti da li je uzrok konjunkcija "
				"done&&!busy u ncc_poll_done, ne hardver (vidi komentar "
				"u ncc_accel.c)\n");
		goto out;
	}
	printf("NCC_WAIT_DONE OK, trajalo %.2f ms\n", elapsed_ms(&t0, &t1));

	// res_w = img_w - tmp_w + 1 = 3, res_h = 3 -> 9 rezultata
	printf("Rezultati (Q1.31, 0x80000000 = 1.0):\n");
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 3; x++)
			printf("  [%d][%d] = 0x%08x\n", y, x, res[y * 3 + x]);
	}

	printf("\nTEST PROSAO.\n");
	munmap(map, MAP_SIZE);
	close(fd);
	return 0;

out:
	munmap(map, MAP_SIZE);
	close(fd);
	return 1;
}
