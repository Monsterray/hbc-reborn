ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii

rtc_shift.dol: rtc_shift.elf
	$(ELF2DOL) $< $@

rtc_shift.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -logc -o $@

clean:
	rm -f rtc_shift.elf rtc_shift.dol

.PHONY: clean
