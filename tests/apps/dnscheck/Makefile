ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack -L$(DEVKITPRO)/libogc/lib/wii

dnscheck.dol: dnscheck.elf
	$(ELF2DOL) $< $@

dnscheck.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f dnscheck.elf dnscheck.dol

.PHONY: clean
