# Aplikacija ncc-scan.
#   make                                       za racunar na kome se prevodi
#   make CROSS_COMPILE=arm-linux-gnueabihf-    za Zybo
# Pokretanje na ploci (drajver mora biti ucitan): ./ncc-scan

CC = $(CROSS_COMPILE)gcc

ncc-scan: main.c data.c data.h
	$(CC) -O2 -Wall -o ncc-scan main.c data.c

clean:
	rm -f ncc-scan
