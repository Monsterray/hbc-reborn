# libusbguard.a: survive a stuck USB drive, for libogc programs (usbguard.h).
#
#   make -C sdk/usbguard
#   app: -I<hbc-reborn>/sdk/usbguard ... -L<hbc-reborn>/sdk/usbguard -lusbguard -lfat -logc
#
# Or copy usbguard.c and usbguard.h into your project; they need libogc and libfat (usbguard_mount).

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

PREFIX = $(DEVKITPPC)/bin/powerpc-eabi-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar

CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include

libusbguard.a: build/usbguard.o
	rm -f $@
	$(AR) rcs $@ $^

build/usbguard.o: usbguard.c usbguard.h
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build libusbguard.a

.PHONY: clean
