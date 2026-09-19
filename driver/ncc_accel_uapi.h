// (C) ncc_accel UAPI — deljeno između kernel drajvera i userspace aplikacije.
//
// IZVOR ISTINE: EOS Vault/CLAUDE.md (izvučen iz stvarnog bare-metal bring-up-a),
// POTVRĐEN direktnom proverom PSDS RTL izvora (VHDL) — vidi komentar u ncc_accel.c.
//
// Userspace upisuje sliku i šablon kroz mmap S01 regiona (na fiksnim BRAM
// offsetima definisanim u ncc_accel.c) i čita rezultate iz istog regiona.
// Rezultati su NCC² u Q1.31 (0x80000000 = 1.0 = savršeno poklapanje), jedan u32
// po poziciji prozora, row-major, dimenzija (img_w - tmp_w + 1) x (img_h - tmp_h + 1).
// VAŽNO: skorovi se porede kao u32, ne int32 (0x80000000 je negativan kao signed).

#ifndef NCC_ACCEL_UAPI_H
#define NCC_ACCEL_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define NCC_IOC_MAGIC	'n'

struct ncc_dims {
	__u32 img_w;
	__u32 img_h;
	__u32 tmp_w;
	__u32 tmp_h;
	__u32 reserved[2];	/* mora biti 0; IMG_ADDR/TMP_ADDR nemaju efekat na hardver */
};

#define NCC_SET_DIMS	_IOW(NCC_IOC_MAGIC, 1, struct ncc_dims)
#define NCC_START	_IO (NCC_IOC_MAGIC, 2)
#define NCC_WAIT_DONE	_IOW(NCC_IOC_MAGIC, 3, __u32)	/* timeout u ms */

/* S01 raspored (mmap na /dev/nccN, 0x20000 B ukupno). PIKSEL = JEDNA
 * 32-BITNA REČ (4B), ne bajt — potvrđeno i tekstom CLAUDE.md i aritmetikom
 * rasporeda (0x08000/4 = 8192 reči = "32 KB (8192 reči)"). */
#define NCC_BRAM_IMG_OFF	0x00000	/* slika,    kapacitet 8192 reči (~90x90) */
#define NCC_BRAM_TMP_OFF	0x08000	/* šablon,   kapacitet 8192 reči */
#define NCC_BRAM_RES_OFF	0x10000	/* rezultat, kapacitet 16384 reči */
/* Piksel (x,y) slike:  ((u32 *)(map + NCC_BRAM_IMG_OFF))[y * img_w + x]
 * Rezultat (x,y):      ((u32 *)(map + NCC_BRAM_RES_OFF))[y * res_w + x]
 *                      res_w = img_w - tmp_w + 1
 * Rezultat je NCC^2 u Q1.31: 0x80000000 = 1.0 */

#endif /* NCC_ACCEL_UAPI_H */
