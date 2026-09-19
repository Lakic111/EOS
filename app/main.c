// (C) main — Linux port (Korak 6) PSDS src/vitis/app/main.c.
// Ista petlja skeniranja 8x8 table i FEN provera kao bare-metal; xil_printf/
// XTime zamenjeni standardnim printf/clock_gettime.

#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "ncc_hw.h"
#include "ncc_app.h"
#include "ncc_logic.h"

static const char FEN_MAP[12] = {'Q','N','K','B','P','R','q','n','k','b','p','r'};
/* Ocekivana pozicija za ulaznu sliku board2, u FEN notaciji.
   Sluzi kao referenca za proveru rezultata: 32 figure. */
static const char FEN_EXPECT[] = "rnbqkbnr/pp5p/4ppp1/2pp4/5P2/1P1BPN2/P1PPQ1PP/RNB1K2R";

static char board[8][8];
static char fen[128];

extern uint64_t prof_load, prof_read, prof_wait;
extern u32 prof_words_load, prof_words_read;

static double ns_to_ms(uint64_t ns)
{
	return (double)ns / 1e6;
}

int main(void)
{
	int m, n, occ = 0;
	struct timespec t0, t1;

	printf("\n=== NCC akcelerator -- prepoznavanje pozicije (Linux/ioctl+mmap) ===\n");
	if (ncc_hw_init()) {
		fprintf(stderr, "init pao\n");
		return 1;
	}

	for (m = 0; m < 8; m++)
		for (n = 0; n < 8; n++)
			board[m][n] = ' ';

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (m = 0; m < 8; m++) {
		for (n = 0; n < 8; n++) {
			square_t s = app_scan_square(m, n);

			if (!s.occupied)
				continue;
			occ++;
			if (s.best_tmpl >= 0)
				board[m][n] = FEN_MAP[s.best_tmpl];

			/* NCC^2 je u formatu Q1.31: 0x80000000 = 1,0. Razlomak se
			 * racuna celobrojno u desetohiljaditim delovima (isto kao
			 * bare-metal, koje nije imalo %f) da izlaz ostane uporediv. */
			{
				uint32_t k = (uint32_t)(((uint64_t)s.best_score * 10000u) / 2147483648ull);

				printf("(%d,%d) %s  %c   NCC2 = %u.%04u\n",
				       m, n, s.is_white ? "bela" : "crna",
				       s.best_tmpl >= 0 ? FEN_MAP[s.best_tmpl] : '?',
				       k / 10000u, k % 10000u);
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	logic_fen((const char (*)[8])board, fen, sizeof fen);
	printf("\nzauzetih polja: %d\n", occ);
	printf("FEN:       %s\n", fen);
	printf("ocekivano: %s\n", FEN_EXPECT);

	{
		double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
				   (t1.tv_nsec - t0.tv_nsec) / 1e6;

		printf("prenos u S01  : %.3f ms, %u reci = %u B\n",
		       ns_to_ms(prof_load), prof_words_load, prof_words_load * 4u);
		printf("citanje rezult: %.3f ms, %u reci = %u B\n",
		       ns_to_ms(prof_read), prof_words_read, prof_words_read * 4u);
		printf("cekanje jezgra: %.3f ms\n", ns_to_ms(prof_wait));
		printf("vreme: %.3f ms\n", total_ms);
	}

	{
		int i, same = 1;

		for (i = 0; fen[i] || FEN_EXPECT[i]; i++)
			if (fen[i] != FEN_EXPECT[i]) {
				same = 0;
				break;
			}
		printf(same ? "Rezultat se poklapa sa ocekivanom pozicijom.\n"
			    : "NESLAGANJE: dobijeni FEN se razlikuje od ocekivanog.\n");
	}

	return 0;
}
