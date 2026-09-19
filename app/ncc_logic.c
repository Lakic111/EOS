#include "ncc_logic.h"

void logic_downsample2x(const unsigned char *src, int w, int h, unsigned char *dst) {
    int ow = w / 2, oh = h / 2, x, y;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++) {
            int s = src[(2*y)*w + 2*x]     + src[(2*y)*w + 2*x + 1]
                  + src[(2*y+1)*w + 2*x]   + src[(2*y+1)*w + 2*x + 1];
            /* ZAOKRUZIVANJE, ne odsecanje -- referentni tb.cpp radi (s + 2) >> 2.
               Sa s / 4 se 33,5 % grubih piksela razlikuje od izvrsne specifikacije. */
            dst[y*ow + x] = (unsigned char)((s + 2) >> 2);
        }
}

/* Srednja vrednost centralnih 31x31 (redovi 29..59, kolone 29..59) segmenta
   ciji je gornji levi ugao na (sx, sy) u slici sirine img_w. */
int logic_center_mean(const unsigned char *img, int img_w, int sx, int sy) {
    int sum = 0, cnt = 0, x, y;
    for (y = 29; y <= 59; y++)
        for (x = 29; x <= 59; x++) { sum += img[(sy + y)*img_w + (sx + x)]; cnt++; }
    return sum / cnt;
}

/* Prazno polje: srednja vrednost centra jednaka pikselu (10,10) -- ujednacena
   povrsina bez figure. Isti kriterijum kao referentni tb.cpp. */
int logic_is_empty(const unsigned char *img, int img_w, int sx, int sy) {
    int mean = logic_center_mean(img, img_w, sx, sy);
    return mean == (int)img[(sy + 10)*img_w + (sx + 10)];
}

int logic_is_white(int mean) { return mean > COLOR_THRESHOLD; }

/* INVARIJANT 1: poredjenje kao u32. 0x80000000 je NCC^2 = 1,0 (savrseno
   poklapanje); kao int32 je negativan, pa bi trazenje maksimuma od -1 odbacilo
   bas najbolji rezultat. */
unsigned int logic_max_u32(const unsigned int *v, unsigned int n) {
    unsigned int i, mx = 0u;
    for (i = 0; i < n; i++) if (v[i] > mx) mx = v[i];
    return mx;
}

void logic_fen(const char board[8][8], char *out, unsigned int out_sz) {
    unsigned int p = 0; int m, n;
    for (m = 0; m < 8; m++) {
        int empty = 0;
        for (n = 0; n < 8; n++) {
            if (board[m][n] == ' ') { empty++; continue; }
            if (empty > 0 && p + 1 < out_sz) { out[p++] = (char)('0' + empty); empty = 0; }
            if (p + 1 < out_sz) out[p++] = board[m][n];
        }
        if (empty > 0 && p + 1 < out_sz) out[p++] = (char)('0' + empty);
        if (m < 7 && p + 1 < out_sz)     out[p++] = '/';
    }
    out[p < out_sz ? p : out_sz - 1] = '\0';
}
