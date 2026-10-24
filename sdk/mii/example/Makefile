# sdk/mii's example: make -C sdk/mii/example, then send mii_example.dol
# with Wiiload (or put it in sd:/apps/mii_example/boot.dol).
#
# In your own Makefile, the parts that matter are compiling mii.c as C and
# linking -lwiiuse -lbte -logc -lm, in that order.

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I.. -I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack -L$(DEVKITPRO)/libogc/lib/wii

mii_example.dol: mii_example.elf
	$(ELF2DOL) $< $@

mii_example.elf: main.c ../mii.c ../mii.h
	$(CC) $(CFLAGS) main.c ../mii.c $(LDFLAGS) -lwiiuse -lbte -logc -lm -o $@

clean:
	rm -f mii_example.elf mii_example.dol

.PHONY: clean
