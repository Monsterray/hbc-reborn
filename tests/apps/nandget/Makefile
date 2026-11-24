ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

nandget.dol: nandget.elf
	$(ELF2DOL) $< $@

nandget.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f nandget.elf nandget.dol

.PHONY: clean
