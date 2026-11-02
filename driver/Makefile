# Prevodjenje modula ncc_driver.ko (isti oblik kao Makefile iz vezbe 9).
#
# Za Zybo (ARM), protiv kernela iz PetaLinux projekta:
#   make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KERNELDIR=<kernel build dir>

# If KERNELRELEASE is defined, we've been invoked from
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	obj-m := ncc_driver.o
# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f *~

endif
