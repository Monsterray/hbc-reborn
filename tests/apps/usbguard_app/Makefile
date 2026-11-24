ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I../../../sdk/usbguard -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

usbguard_app.dol: usbguard_app.elf
	$(ELF2DOL) $< $@

usbguard_app.elf: main.c ../../../sdk/usbguard/usbguard.c ../../../sdk/usbguard/usbguard.h
	$(CC) $(CFLAGS) main.c ../../../sdk/usbguard/usbguard.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f usbguard_app.elf usbguard_app.dol

.PHONY: clean
