ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include -I../../sdk
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii

netlog_app.dol: netlog_app.elf
	$(ELF2DOL) $< $@

netlog_app.elf: main.c ../../sdk/hbc_netlog.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -logc -o $@

clean:
	rm -f netlog_app.elf netlog_app.dol

.PHONY: clean
