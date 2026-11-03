// Board image (720x720, 8x8 squares of 90x90) and 12 piece templates, in data.c.
#ifndef DATA_H
#define DATA_H
#define BOARD_W 720
#define BOARD_H 720
extern const unsigned char BOARD_IMG[518400];
extern const unsigned char *const TMPL_FULL[12];     // full resolution
extern const unsigned char *const TMPL_COARSE[12];   // downscaled 2x
extern const int TMPL_FULL_W[12],   TMPL_FULL_H[12];
extern const int TMPL_COARSE_W[12], TMPL_COARSE_H[12];
#endif
