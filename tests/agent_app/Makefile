ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
AGENT = ../../sdk/hbc_agent
CFLAGS = -g -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include -I../../sdk
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(AGENT) -L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

agent_app.dol: agent_app.elf
	$(ELF2DOL) $< $@

agent_app.elf: main.c ../../sdk/hbc_netlog.h ../../sdk/hbc_agent.h $(AGENT)/libhbcagent.a
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lhbcagent -lwiiuse -lbte -lfat -lz -logc -lm -o $@

$(AGENT)/libhbcagent.a: FORCE
	$(MAKE) -C $(AGENT) EXTRA_CFLAGS="$(EXTRA_CFLAGS)"

clean:
	rm -f agent_app.elf agent_app.dol

FORCE:
.PHONY: clean FORCE
