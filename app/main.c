// ncc-scan -- recognition of a chess position from a board image using the NCC accelerator.
//
// For every occupied square: a coarse pass over all 6 templates of the matching color on
// the downscaled image (45x45), then a fine check of the two best ones on the full image (90x90).
// The two accelerators (/dev/ncc0 and /dev/ncc1) work in parallel, each on its own template.
//
// Driver access:
//   write("dims iw ih tw th"), write("start")  -- control
//   reading /dev/nccN -> "done=1 busy=0"       -- status
//   mmap                                       -- image, template and result

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include "data.h"

#define SEG_W            90     // a board square is 90x90 pixels
#define SEG_H            90
#define COLOR_THRESHOLD  140    // above this the piece is white
#define S01_SIZE         0x20000
// offsets in S01, in 32-bit words (one pixel = one word)
#define IMG_OFF          (0x00000 / 4)
#define TMP_OFF          (0x08000 / 4)
#define RES_OFF          (0x10000 / 4)

struct ncc {
	const char *name;
	int fd;
	volatile unsigned int *mem;   // S01 mapped into the process memory
};

static struct ncc ncc0 = { "/dev/ncc0", -1, NULL };
static struct ncc ncc1 = { "/dev/ncc1", -1, NULL };

// template order in data.c: 6 white pieces, then 6 black pieces
static const char FEN_MAP[12] = {'Q','N','K','B','P','R','q','n','k','b','p','r'};
static const char FEN_EXPECTED[] = "rnbqkbnr/pp5p/4ppp1/2pp4/5P2/1P1BPN2/P1PPQ1PP/RNB1K2R";

static unsigned char seg_full[SEG_W * SEG_H];
static unsigned char seg_coarse[(SEG_W / 2) * (SEG_H / 2)];
static double t_wait = 0;   // total time spent waiting for the accelerators, ms

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ======================================================== driver access

static int ncc_open(struct ncc *d)
{
	d->fd = open(d->name, O_RDWR);
	if (d->fd < 0) {
		printf("Cannot open %s\n", d->name);
		return -1;
	}
	d->mem = mmap(NULL, S01_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
	if (d->mem == MAP_FAILED) {
		printf("mmap for %s failed\n", d->name);
		return -1;
	}
	return 0;
}

static int ncc_command(struct ncc *d, const char *cmd)
{
	if (write(d->fd, cmd, strlen(cmd)) < 0) {
		printf("%s: command \"%s\" rejected\n", d->name, cmd);
		return -1;
	}
	return 0;
}

static int ncc_dims(struct ncc *d, int iw, int ih, int tw, int th)
{
	char cmd[64];
	sprintf(cmd, "dims %d %d %d %d", iw, ih, tw, th);
	return ncc_command(d, cmd);
}

// Reads the status (like "cat /dev/nccN") until the core finishes: done = 1 and busy = 0.
static int ncc_wait(struct ncc *d)
{
	FILE *fp;
	int done, busy, i;

	for (i = 0; i < 20000; i++) {   // at most about 2 s
		fp = fopen(d->name, "r");
		if (fp == NULL)
			return -1;
		if (fscanf(fp, "done=%d busy=%d", &done, &busy) != 2)
			done = 0;
		fclose(fp);
		if (done == 1 && busy == 0)
			return 0;
		usleep(100);
	}
	printf("%s: the core did not finish\n", d->name);
	return -1;
}

// An 8-bit pixel is written as one 32-bit word.
static void write_pixels(volatile unsigned int *dst, const unsigned char *px, int n)
{
	int i;
	for (i = 0; i < n; i++)
		dst[i] = px[i];
}

// Largest result. Compared as unsigned: 0x80000000 (perfect match)
// would be negative as an int.
static unsigned int best_score(struct ncc *d, int n)
{
	unsigned int mx = 0;
	int i;
	for (i = 0; i < n; i++)
		if (d->mem[RES_OFF + i] > mx)
			mx = d->mem[RES_OFF + i];
	return mx;
}

// Template ta on ncc0 and (if tb >= 0) template tb on ncc1, over an image that is already written.
static int run_pair(int ta, int tb, const unsigned char *const *TP, const int *TW, const int *TH,
		    int iw, int ih, unsigned int *sa, unsigned int *sb)
{
	int err0, err1 = 0;
	double t;

	*sa = 0;
	*sb = 0;
	write_pixels(ncc0.mem + TMP_OFF, TP[ta], TW[ta] * TH[ta]);
	if (ncc_dims(&ncc0, iw, ih, TW[ta], TH[ta]))
		return -1;
	if (tb >= 0) {
		write_pixels(ncc1.mem + TMP_OFF, TP[tb], TW[tb] * TH[tb]);
		if (ncc_dims(&ncc1, iw, ih, TW[tb], TH[tb]))
			return -1;
	}

	if (ncc_command(&ncc0, "start"))
		return -1;
	if (tb >= 0 && ncc_command(&ncc1, "start"))
		return -1;

	// always wait for both, so the next pair does not write into the memory of a core that is still running
	t = now_ms();
	err0 = ncc_wait(&ncc0);
	if (tb >= 0)
		err1 = ncc_wait(&ncc1);
	t_wait += now_ms() - t;
	if (err0 || err1)
		return -1;

	*sa = best_score(&ncc0, (iw - TW[ta] + 1) * (ih - TH[ta] + 1));
	if (tb >= 0)
		*sb = best_score(&ncc1, (iw - TW[tb] + 1) * (ih - TH[tb] + 1));
	return 0;
}

// ======================================================== image processing

// 2x downscale: average of 2x2 pixels, with rounding.
static void downscale(const unsigned char *src, int w, int h, unsigned char *dst)
{
	int x, y, s;
	for (y = 0; y < h / 2; y++)
		for (x = 0; x < w / 2; x++) {
			s = src[(2*y)*w + 2*x]   + src[(2*y)*w + 2*x + 1]
			  + src[(2*y+1)*w + 2*x] + src[(2*y+1)*w + 2*x + 1];
			dst[y*(w/2) + x] = (s + 2) >> 2;
		}
}

// Mean of the central 31x31 pixels of the square (rows and columns 29..59).
static int center_mean(int sx, int sy)
{
	int x, y, sum = 0;
	for (y = 29; y <= 59; y++)
		for (x = 29; x <= 59; x++)
			sum += BOARD_IMG[(sy + y)*BOARD_W + (sx + x)];
	return sum / (31 * 31);
}

// A square is empty if the mean of its center equals the pixel at (10,10).
static int is_empty(int sx, int sy)
{
	return center_mean(sx, sy) == BOARD_IMG[(sy + 10)*BOARD_W + (sx + 10)];
}

struct square {
	int occupied;
	int white;
	int tmpl;            // index into FEN_MAP, -1 if not recognized
	unsigned int score;
};

static int scan_square(int m, int n, struct square *p)
{
	int cands[6], topk[2], x, y, i, j, k, q, taken, best;
	unsigned int cscore[6], sa, sb, bs;
	int cw = SEG_W / 2, ch = SEG_H / 2;
	int sx = n * SEG_W, sy = m * SEG_H;

	p->occupied = 0;
	p->white = 0;
	p->tmpl = -1;
	p->score = 0;
	if (is_empty(sx, sy))
		return 0;
	p->occupied = 1;
	p->white = center_mean(sx, sy) > COLOR_THRESHOLD;
	for (i = 0; i < 6; i++)
		cands[i] = p->white ? i : 6 + i;

	// cut the square out of the board (720x720) and make the downscaled version
	for (y = 0; y < SEG_H; y++)
		for (x = 0; x < SEG_W; x++)
			seg_full[y*SEG_W + x] = BOARD_IMG[(sy + y)*BOARD_W + (sx + x)];
	downscale(seg_full, SEG_W, SEG_H, seg_coarse);

	// coarse pass: the image is written once, the templates change two at a time
	write_pixels(ncc0.mem + IMG_OFF, seg_coarse, cw * ch);
	write_pixels(ncc1.mem + IMG_OFF, seg_coarse, cw * ch);
	for (k = 0; k < 6; k += 2) {
		if (run_pair(cands[k], cands[k + 1], TMPL_COARSE, TMPL_COARSE_W, TMPL_COARSE_H, cw, ch, &sa, &sb))
			return -1;
		cscore[k] = sa;
		cscore[k + 1] = sb;
	}

	// the two best by coarse score
	for (i = 0; i < 2; i++) {
		best = -1;
		bs = 0;
		for (j = 0; j < 6; j++) {
			taken = 0;
			for (q = 0; q < i; q++)
				if (topk[q] == cands[j])
					taken = 1;
			if (!taken && (best < 0 || cscore[j] > bs)) {
				best = cands[j];
				bs = cscore[j];
			}
		}
		topk[i] = best;
	}

	// fine check at full resolution
	write_pixels(ncc0.mem + IMG_OFF, seg_full, SEG_W * SEG_H);
	write_pixels(ncc1.mem + IMG_OFF, seg_full, SEG_W * SEG_H);
	if (run_pair(topk[0], topk[1], TMPL_FULL, TMPL_FULL_W, TMPL_FULL_H, SEG_W, SEG_H, &sa, &sb))
		return -1;
	if (sa >= sb) {
		p->score = sa;
		p->tmpl = topk[0];
	}
	else {
		p->score = sb;
		p->tmpl = topk[1];
	}
	return 0;
}

// Board to FEN: consecutive empty squares are written as a number, rows are separated by '/'.
static void make_fen(char board[8][8], char *fen)
{
	int m, n, empty, p = 0;
	for (m = 0; m < 8; m++) {
		empty = 0;
		for (n = 0; n < 8; n++) {
			if (board[m][n] == ' ') {
				empty++;
				continue;
			}
			if (empty > 0) {
				fen[p++] = '0' + empty;
				empty = 0;
			}
			fen[p++] = board[m][n];
		}
		if (empty > 0)
			fen[p++] = '0' + empty;
		if (m < 7)
			fen[p++] = '/';
	}
	fen[p] = '\0';
}

// ======================================================== main

int main(void)
{
	char board[8][8];
	char fen[128];
	struct square p;
	int m, n, occupied = 0;
	unsigned int k;
	double t0, t1;

	printf("=== NCC accelerator -- position recognition ===\n");
	if (ncc_open(&ncc0) || ncc_open(&ncc1))
		return 1;

	t0 = now_ms();
	for (m = 0; m < 8; m++) {
		for (n = 0; n < 8; n++) {
			board[m][n] = ' ';
			if (scan_square(m, n, &p)) {
				printf("Error on square (%d,%d), stopping.\n", m, n);
				return 1;
			}
			if (!p.occupied)
				continue;
			occupied++;
			if (p.tmpl >= 0)
				board[m][n] = FEN_MAP[p.tmpl];
			// NCC^2 is in Q1.31 (0x80000000 = 1.0), printed with 4 decimals
			k = (unsigned int)(((unsigned long long)p.score * 10000) / 2147483648ULL);
			printf("(%d,%d) %s  %c   NCC2 = %u.%04u\n", m, n, p.white ? "white" : "black",
			       p.tmpl >= 0 ? FEN_MAP[p.tmpl] : '?', k / 10000, k % 10000);
		}
	}
	t1 = now_ms();

	make_fen(board, fen);
	printf("\noccupied squares: %d\n", occupied);
	printf("FEN:      %s\n", fen);
	printf("expected: %s\n", FEN_EXPECTED);
	printf("time: %.3f ms, of which waiting for the accelerator %.3f ms\n", t1 - t0, t_wait);

	if (strcmp(fen, FEN_EXPECTED) != 0) {
		printf("MISMATCH: the FEN differs from the expected one.\n");
		return 1;
	}
	printf("The result matches the expected position.\n");
	return 0;
}
