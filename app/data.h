// Slika table (720x720, 8x8 polja od 90x90) i 12 sablona figura, u data.c.
#ifndef DATA_H
#define DATA_H
#define BOARD_W 720
#define BOARD_H 720
extern const unsigned char BOARD_IMG[518400];
extern const unsigned char *const TMPL_FULL[12];     // puna rezolucija
extern const unsigned char *const TMPL_COARSE[12];   // umanjeni 2x
extern const int TMPL_FULL_W[12],   TMPL_FULL_H[12];
extern const int TMPL_COARSE_W[12], TMPL_COARSE_H[12];
#endif
