# libwiispk.a: Wii Remote speaker audio for libogc programs (wiispk.h).
#
#   make -C sdk/wiispk
#   app: -I<hbc-reborn>/sdk/wiispk ... -L<hbc-reborn>/sdk/wiispk -lwiispk -lwiiuse -lbte -logc -lm
#
# Or copy wiispk.c and wiispk.h into your project; they need only libogc.

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

PREFIX = $(DEVKITPPC)/bin/powerpc-eabi-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar

CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include

libwiispk.a: build/wiispk.o
	rm -f $@
	$(AR) rcs $@ $^

build/wiispk.o: wiispk.c wiispk.h
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build libwiispk.a

.PHONY: clean
