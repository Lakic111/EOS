// ncc-scan -- prepoznavanje sahovske pozicije sa slike table pomocu NCC akceleratora.
//
// Za svako zauzeto polje: grubi pregled svih 6 sablona odgovarajuce boje na
// umanjenoj slici (45x45), pa fina provera dva najbolja na punoj slici (90x90).
// Dva akceleratora (/dev/ncc0 i /dev/ncc1) rade paralelno, svaki svoj sablon.
//
// Pristup drajveru:
//   write("dims iw ih tw th"), write("start")  -- kontrola
//   citanje /dev/nccN -> "done=1 busy=0"       -- status
//   mmap                                       -- slika, sablon i rezultat

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include "data.h"

#define SEG_W            90     // polje table je 90x90 piksela
#define SEG_H            90
#define COLOR_THRESHOLD  140    // iznad ovoga je figura bela
#define S01_SIZE         0x20000
// pomeraji u S01, u 32-bitnim recima (jedan piksel = jedna rec)
#define IMG_OFF          (0x00000 / 4)
#define TMP_OFF          (0x08000 / 4)
#define RES_OFF          (0x10000 / 4)

struct ncc {
	const char *ime;
	int fd;
	volatile unsigned int *mem;   // S01 mapiran u memoriju procesa
};

static struct ncc ncc0 = { "/dev/ncc0", -1, NULL };
static struct ncc ncc1 = { "/dev/ncc1", -1, NULL };

// redosled sablona u data.c: 6 belih pa 6 crnih figura
static const char FEN_MAP[12] = {'Q','N','K','B','P','R','q','n','k','b','p','r'};
static const char FEN_OCEKIVANO[] = "rnbqkbnr/pp5p/4ppp1/2pp4/5P2/1P1BPN2/P1PPQ1PP/RNB1K2R";

static unsigned char seg_full[SEG_W * SEG_H];
static unsigned char seg_coarse[(SEG_W / 2) * (SEG_H / 2)];
static double t_cekanje = 0;   // ukupno vreme cekanja na akceleratore, ms

static double sada_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ======================================================== rad sa drajverom

static int ncc_otvori(struct ncc *d)
{
	d->fd = open(d->ime, O_RDWR);
	if (d->fd < 0) {
		printf("Ne mogu da otvorim %s\n", d->ime);
		return -1;
	}
	d->mem = mmap(NULL, S01_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
	if (d->mem == MAP_FAILED) {
		printf("mmap za %s nije uspeo\n", d->ime);
		return -1;
	}
	return 0;
}

static int ncc_komanda(struct ncc *d, const char *komanda)
{
	if (write(d->fd, komanda, strlen(komanda)) < 0) {
		printf("%s: komanda \"%s\" odbijena\n", d->ime, komanda);
		return -1;
	}
	return 0;
}

static int ncc_dimenzije(struct ncc *d, int iw, int ih, int tw, int th)
{
	char komanda[64];
	sprintf(komanda, "dims %d %d %d %d", iw, ih, tw, th);
	return ncc_komanda(d, komanda);
}

// Cita status (kao "cat /dev/nccN") dok jezgro ne zavrsi: done = 1 i busy = 0.
static int ncc_cekaj(struct ncc *d)
{
	FILE *fp;
	int done, busy, i;

	for (i = 0; i < 20000; i++) {   // najvise oko 2 s
		fp = fopen(d->ime, "r");
		if (fp == NULL)
			return -1;
		if (fscanf(fp, "done=%d busy=%d", &done, &busy) != 2)
			done = 0;
		fclose(fp);
		if (done == 1 && busy == 0)
			return 0;
		usleep(100);
	}
	printf("%s: jezgro nije zavrsilo\n", d->ime);
	return -1;
}

// Piksel od 8 bita se upisuje kao jedna 32-bitna rec.
static void upisi(volatile unsigned int *dst, const unsigned char *px, int n)
{
	int i;
	for (i = 0; i < n; i++)
		dst[i] = px[i];
}

// Najveci rezultat. Poredi se kao unsigned: 0x80000000 (savrseno poklapanje)
// bi kao int bio negativan.
static unsigned int najbolji_skor(struct ncc *d, int n)
{
	unsigned int mx = 0;
	int i;
	for (i = 0; i < n; i++)
		if (d->mem[RES_OFF + i] > mx)
			mx = d->mem[RES_OFF + i];
	return mx;
}

// Sablon ta na ncc0 i (ako je tb >= 0) sablon tb na ncc1, nad slikom koja je vec upisana.
static int par(int ta, int tb, const unsigned char *const *TP, const int *TW, const int *TH,
	       int iw, int ih, unsigned int *sa, unsigned int *sb)
{
	int greska0, greska1 = 0;
	double t;

	*sa = 0;
	*sb = 0;
	upisi(ncc0.mem + TMP_OFF, TP[ta], TW[ta] * TH[ta]);
	if (ncc_dimenzije(&ncc0, iw, ih, TW[ta], TH[ta]))
		return -1;
	if (tb >= 0) {
		upisi(ncc1.mem + TMP_OFF, TP[tb], TW[tb] * TH[tb]);
		if (ncc_dimenzije(&ncc1, iw, ih, TW[tb], TH[tb]))
			return -1;
	}

	if (ncc_komanda(&ncc0, "start"))
		return -1;
	if (tb >= 0 && ncc_komanda(&ncc1, "start"))
		return -1;

	// uvek se cekaju oba, da sledeci par ne pise u memoriju jezgra koje jos radi
	t = sada_ms();
	greska0 = ncc_cekaj(&ncc0);
	if (tb >= 0)
		greska1 = ncc_cekaj(&ncc1);
	t_cekanje += sada_ms() - t;
	if (greska0 || greska1)
		return -1;

	*sa = najbolji_skor(&ncc0, (iw - TW[ta] + 1) * (ih - TH[ta] + 1));
	if (tb >= 0)
		*sb = najbolji_skor(&ncc1, (iw - TW[tb] + 1) * (ih - TH[tb] + 1));
	return 0;
}

// ======================================================== obrada slike

// Umanjenje 2x: prosek 2x2 piksela, sa zaokruzivanjem.
static void umanji(const unsigned char *src, int w, int h, unsigned char *dst)
{
	int x, y, s;
	for (y = 0; y < h / 2; y++)
		for (x = 0; x < w / 2; x++) {
			s = src[(2*y)*w + 2*x]   + src[(2*y)*w + 2*x + 1]
			  + src[(2*y+1)*w + 2*x] + src[(2*y+1)*w + 2*x + 1];
			dst[y*(w/2) + x] = (s + 2) >> 2;
		}
}

// Srednja vrednost centralnih 31x31 piksela polja (redovi i kolone 29..59).
static int srednja_centra(int sx, int sy)
{
	int x, y, sum = 0;
	for (y = 29; y <= 59; y++)
		for (x = 29; x <= 59; x++)
			sum += BOARD_IMG[(sy + y)*BOARD_W + (sx + x)];
	return sum / (31 * 31);
}

// Polje je prazno ako je srednja vrednost centra jednaka pikselu (10,10).
static int prazno(int sx, int sy)
{
	return srednja_centra(sx, sy) == BOARD_IMG[(sy + 10)*BOARD_W + (sx + 10)];
}

struct polje {
	int zauzeto;
	int bela;
	int sablon;          // indeks u FEN_MAP, -1 ako nije prepoznat
	unsigned int skor;
};

static int skeniraj_polje(int m, int n, struct polje *p)
{
	int cands[6], topk[2], x, y, i, j, k, q, vec, najbolji;
	unsigned int cskor[6], sa, sb, bs;
	int cw = SEG_W / 2, ch = SEG_H / 2;
	int sx = n * SEG_W, sy = m * SEG_H;

	p->zauzeto = 0;
	p->bela = 0;
	p->sablon = -1;
	p->skor = 0;
	if (prazno(sx, sy))
		return 0;
	p->zauzeto = 1;
	p->bela = srednja_centra(sx, sy) > COLOR_THRESHOLD;
	for (i = 0; i < 6; i++)
		cands[i] = p->bela ? i : 6 + i;

	// izdvajanje polja iz table (720x720) i umanjena verzija
	for (y = 0; y < SEG_H; y++)
		for (x = 0; x < SEG_W; x++)
			seg_full[y*SEG_W + x] = BOARD_IMG[(sy + y)*BOARD_W + (sx + x)];
	umanji(seg_full, SEG_W, SEG_H, seg_coarse);

	// grubi pregled: slika se upisuje jednom, sabloni se menjaju po dva
	upisi(ncc0.mem + IMG_OFF, seg_coarse, cw * ch);
	upisi(ncc1.mem + IMG_OFF, seg_coarse, cw * ch);
	for (k = 0; k < 6; k += 2) {
		if (par(cands[k], cands[k + 1], TMPL_COARSE, TMPL_COARSE_W, TMPL_COARSE_H, cw, ch, &sa, &sb))
			return -1;
		cskor[k] = sa;
		cskor[k + 1] = sb;
	}

	// dva najbolja po grubom skoru
	for (i = 0; i < 2; i++) {
		najbolji = -1;
		bs = 0;
		for (j = 0; j < 6; j++) {
			vec = 0;
			for (q = 0; q < i; q++)
				if (topk[q] == cands[j])
					vec = 1;
			if (!vec && (najbolji < 0 || cskor[j] > bs)) {
				najbolji = cands[j];
				bs = cskor[j];
			}
		}
		topk[i] = najbolji;
	}

	// fina provera na punoj rezoluciji
	upisi(ncc0.mem + IMG_OFF, seg_full, SEG_W * SEG_H);
	upisi(ncc1.mem + IMG_OFF, seg_full, SEG_W * SEG_H);
	if (par(topk[0], topk[1], TMPL_FULL, TMPL_FULL_W, TMPL_FULL_H, SEG_W, SEG_H, &sa, &sb))
		return -1;
	if (sa >= sb) {
		p->skor = sa;
		p->sablon = topk[0];
	}
	else {
		p->skor = sb;
		p->sablon = topk[1];
	}
	return 0;
}

// Tabla u FEN zapis: uzastopna prazna polja se pisu kao broj, redovi se odvajaju sa '/'.
static void napravi_fen(char tabla[8][8], char *fen)
{
	int m, n, prazna, p = 0;
	for (m = 0; m < 8; m++) {
		prazna = 0;
		for (n = 0; n < 8; n++) {
			if (tabla[m][n] == ' ') {
				prazna++;
				continue;
			}
			if (prazna > 0) {
				fen[p++] = '0' + prazna;
				prazna = 0;
			}
			fen[p++] = tabla[m][n];
		}
		if (prazna > 0)
			fen[p++] = '0' + prazna;
		if (m < 7)
			fen[p++] = '/';
	}
	fen[p] = '\0';
}

// ======================================================== main

int main(void)
{
	char tabla[8][8];
	char fen[128];
	struct polje p;
	int m, n, zauzetih = 0;
	unsigned int k;
	double t0, t1;

	printf("=== NCC akcelerator -- prepoznavanje pozicije ===\n");
	if (ncc_otvori(&ncc0) || ncc_otvori(&ncc1))
		return 1;

	t0 = sada_ms();
	for (m = 0; m < 8; m++) {
		for (n = 0; n < 8; n++) {
			tabla[m][n] = ' ';
			if (skeniraj_polje(m, n, &p)) {
				printf("Greska na polju (%d,%d), prekid.\n", m, n);
				return 1;
			}
			if (!p.zauzeto)
				continue;
			zauzetih++;
			if (p.sablon >= 0)
				tabla[m][n] = FEN_MAP[p.sablon];
			// NCC^2 je u Q1.31 (0x80000000 = 1.0), ispis sa 4 decimale
			k = (unsigned int)(((unsigned long long)p.skor * 10000) / 2147483648ULL);
			printf("(%d,%d) %s  %c   NCC2 = %u.%04u\n", m, n, p.bela ? "bela" : "crna",
			       p.sablon >= 0 ? FEN_MAP[p.sablon] : '?', k / 10000, k % 10000);
		}
	}
	t1 = sada_ms();

	napravi_fen(tabla, fen);
	printf("\nzauzetih polja: %d\n", zauzetih);
	printf("FEN:       %s\n", fen);
	printf("ocekivano: %s\n", FEN_OCEKIVANO);
	printf("vreme: %.3f ms, od toga cekanje akceleratora %.3f ms\n", t1 - t0, t_cekanje);

	if (strcmp(fen, FEN_OCEKIVANO) != 0) {
		printf("NESLAGANJE: dobijeni FEN se razlikuje od ocekivanog.\n");
		return 1;
	}
	printf("Rezultat se poklapa sa ocekivanom pozicijom.\n");
	return 0;
}
