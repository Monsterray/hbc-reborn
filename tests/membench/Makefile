ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include -I../../sdk
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

membench.dol: membench.elf
	$(ELF2DOL) $< $@

membench.elf: main.c ../../sdk/hbc_netlog.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lfat -lz -logc -o $@

clean:
	rm -f membench.elf membench.dol

.PHONY: clean
