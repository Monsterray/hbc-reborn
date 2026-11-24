ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I../../../sdk/slotled -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

slotled_app.dol: slotled_app.elf
	$(ELF2DOL) $< $@

slotled_app.elf: main.c ../../../sdk/slotled/slotled.c ../../../sdk/slotled/slotled.h
	$(CC) $(CFLAGS) main.c ../../../sdk/slotled/slotled.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f slotled_app.elf slotled_app.dol

.PHONY: clean
