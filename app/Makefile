# (C) Makefile za korisnicku aplikaciju ncc-scan (prepoznavanje sahovske pozicije
# preko /dev/ncc0 i /dev/ncc1).
#
#   make                                          prevodjenje za masinu na kojoj se gradi
#   make CROSS_COMPILE=arm-linux-gnueabihf-       unakrsno, za Zybo (ARM)
#   make clean
#
# Pokretanje na ploci (drajver ncc_accel mora biti ucitan):  sudo ./ncc-scan

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -O2 -Wall -Wextra

TARGET := ncc-scan

SRCS := main.c ncc_app.c ncc_hw.c ncc_logic.c \
        data_board.c data_golden.c data_tmpl.c
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# -MMD -MP: kompajler sam pravi .d fajlove sa zavisnostima od headera, pa se
# posle izmene npr. ncc_hw.h ponovo prevode svi fajlovi koji ga ukljucuju.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)
