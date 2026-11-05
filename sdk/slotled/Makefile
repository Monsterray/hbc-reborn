# libslotled.a: the Wii's disc-slot light for libogc programs (slotled.h).
#
#   make -C sdk/slotled
#   app: -I<hbc-reborn>/sdk/slotled ... -L<hbc-reborn>/sdk/slotled -lslotled -logc
#
# Or copy slotled.c and slotled.h into your project; they need only libogc.

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

PREFIX = $(DEVKITPPC)/bin/powerpc-eabi-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar

CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include

libslotled.a: build/slotled.o
	rm -f $@
	$(AR) rcs $@ $^

build/slotled.o: slotled.c slotled.h
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build libslotled.a

.PHONY: clean
