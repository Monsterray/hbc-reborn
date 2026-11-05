ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -g -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii

# wpad_queue_app.dol is libogc as it is: slots 3 to 5 get no command queue.
# wpad_queue_app-fixed.dol links the channel's wpadqueue.c, as the channel does.
all: wpad_queue_app.dol wpad_queue_app-fixed.dol

%.dol: %.elf
	$(ELF2DOL) $< $@

wpad_queue_app.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lwiiuse -lbte -logc -lm -o $@

wpad_queue_app-fixed.elf: main.c ../../channel/channelapp/source/wpadqueue.c
	$(CC) $(CFLAGS) main.c ../../channel/channelapp/source/wpadqueue.c $(LDFLAGS) \
		-Wl,--wrap=wiiuse_init_cmd_queue -lwiiuse -lbte -logc -lm -o $@

clean:
	rm -f wpad_queue_app*.elf wpad_queue_app*.dol

.PHONY: all clean
