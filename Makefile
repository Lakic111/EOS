# (C) Glavni Makefile za EOS projekat: kernel drajver + korisnicka aplikacija.
#
#   make                  drajver (ncc_accel.ko), test_ioctl i aplikacija (ncc-scan)
#   make driver           samo kernel modul
#   make test             samo test_ioctl
#   make app              samo aplikacija
#   make clean
#
# Za Zybo ploce (ARM) promenljive se zadaju jednom, ovde, i prenose u
# poddirektorijume:
#
#   make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=<kernel build dir>
#
# Vidi driver/Makefile za to sta je KDIR.

.PHONY: all driver test app clean

all: driver test app

driver:
	$(MAKE) -C driver modules

test:
	$(MAKE) -C driver test

app:
	$(MAKE) -C app

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C app clean
