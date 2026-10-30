ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I../../sdk/mii -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

mii_app.dol: mii_app.elf
	$(ELF2DOL) $< $@

mii_app.elf: main.c ../../sdk/mii/mii.c ../../sdk/mii/mii.h ../../sdk/mii/miidraw.c ../../sdk/mii/miidraw.h ../../sdk/mii/miibody.c ../../sdk/mii/miifx.c ../../sdk/mii/miinand.c
	$(CC) $(CFLAGS) main.c ../../sdk/mii/mii.c ../../sdk/mii/miidraw.c ../../sdk/mii/miibody.c ../../sdk/mii/miifx.c ../../sdk/mii/miinand.c $(LDFLAGS) -lfat -lwiiuse -lbte -logc -lm -o $@

clean:
	rm -f mii_app.elf mii_app.dol

.PHONY: clean
