# (C) Makefile za ncc_accel kernel modul i test program test_ioctl.
#
#   make                  modul ncc_accel.ko za kernel na kojem se gradi (uname -r)
#   make test             test_ioctl, korisnicki program za proveru ioctl/mmap
#   make clean
#
# Unakrsno prevodjenje za Zybo (ARM), protiv izvora kernela iz PetaLinux projekta:
#
#   make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KDIR=<kernel build dir>
#
# KDIR je direktorijum u kome je kernel vec preveden (sadrzi .config i
# Module.symvers). U PetaLinux projektu to je
#   <projekat>/build/tmp/work-shared/<MACHINE>/kernel-build-artifacts
# Modul mora biti preveden protiv ISTOG kernela koji radi na ploci, inace ga
# insmod odbija ("invalid module format").

ifneq ($(KERNELRELEASE),)
# --- Kbuild deo: ovde dolazi kernel build sistem (make -C $(KDIR) M=...) ------
obj-m := ncc_accel.o

else
# --- obican poziv iz komandne linije -------------------------------------------
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Korisnicki program se prevodi istim unakrsnim kompajlerom kao modul.  CC iz
# okruzenja (npr. Yocto/PetaLinux) ili sa komandne linije ima prednost.
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all modules test clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: test_ioctl

test_ioctl: test_ioctl.c ncc_accel_uapi.h
	$(CC) $(CFLAGS) -o $@ test_ioctl.c $(LDFLAGS)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || \
	  rm -f *.o *.ko *.mod *.mod.c *.mod.o .*.cmd modules.order Module.symvers
	rm -f test_ioctl

endif
