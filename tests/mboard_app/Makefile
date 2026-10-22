ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I../../sdk/mboard -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

mboard_app.dol: mboard_app.elf
	$(ELF2DOL) $< $@

mboard_app.elf: main.c ../../sdk/mboard/mboard.c ../../sdk/mboard/mboard.h
	$(CC) $(CFLAGS) main.c ../../sdk/mboard/mboard.c $(LDFLAGS) -lfat -logc -o $@

# The Dolphin test's build (tests/dolphin_mboard.py).
dolphin: main.c ../../sdk/mboard/mboard.c ../../sdk/mboard/mboard.h
	$(CC) $(CFLAGS) -DMBOARD_APP_DOLPHIN main.c ../../sdk/mboard/mboard.c $(LDFLAGS) -lfat -logc \
		-o mboard_app_dolphin.elf
	$(ELF2DOL) mboard_app_dolphin.elf mboard_app_dolphin.dol

# The same for the full message (tests/dolphin_mboard.py --full).
dolphin-full: main.c ../../sdk/mboard/mboard.c ../../sdk/mboard/mboard.h
	$(CC) $(CFLAGS) -DMBOARD_APP_DOLPHIN_FULL main.c ../../sdk/mboard/mboard.c $(LDFLAGS) -lfat -logc \
		-o mboard_app_dolphin_full.elf
	$(ELF2DOL) mboard_app_dolphin_full.elf mboard_app_dolphin_full.dol

clean:
	rm -f mboard_app.elf mboard_app.dol mboard_app_dolphin.elf mboard_app_dolphin.dol \
		mboard_app_dolphin_full.elf mboard_app_dolphin_full.dol

.PHONY: clean dolphin dolphin-full
