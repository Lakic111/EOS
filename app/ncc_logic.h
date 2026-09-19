#ifndef NCC_LOGIC_H
#define NCC_LOGIC_H
/* Cista logika: BEZ Xilinx headera, kompajlira se i na hostu. */

#define COLOR_THRESHOLD 140
#define SEG_W 90
#define SEG_H 90

void         logic_downsample2x(const unsigned char *src, int w, int h, unsigned char *dst);
int          logic_center_mean (const unsigned char *img, int img_w, int sx, int sy);
int          logic_is_empty    (const unsigned char *img, int img_w, int sx, int sy);
int          logic_is_white    (int mean);
unsigned int logic_max_u32     (const unsigned int *v, unsigned int n);
void         logic_fen         (const char board[8][8], char *out, unsigned int out_sz);
#endif
