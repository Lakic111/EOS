# Application ncc-scan.
#   make                                       for the machine it is built on
#   make CROSS_COMPILE=arm-linux-gnueabihf-    for the Zybo
# Running on the board (the driver must be loaded): ./ncc-scan

CC = $(CROSS_COMPILE)gcc

ncc-scan: main.c data.c data.h
	$(CC) -O2 -Wall -o ncc-scan main.c data.c

clean:
	rm -f ncc-scan
