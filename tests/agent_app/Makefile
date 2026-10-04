ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
AGENT = ../../sdk/hbc_agent
# OGC=libogc2 or OGC=libogc-1.8.23 builds agent_app-<OGC>.dol against that
# libogc and its own libhbcagent (the crash hook differs; see ogc_exc.S).
OGC ?= libogc
ifeq ($(OGC),libogc)
APP = agent_app
AGENT_LIB = $(AGENT)
else
APP = agent_app-$(OGC)
AGENT_LIB = $(AGENT)/$(OGC)
endif
# MODE=crash (or trap) builds $(APP)-crash.dol, which crashes with no argv.
ifneq ($(MODE),)
APP := $(APP)-$(MODE)
MODE_CFLAGS = -DAGENT_APP_MODE='"$(MODE)"'
endif
CFLAGS = -g -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(MODE_CFLAGS) $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/$(OGC)/include -I$(DEVKITPRO)/portlibs/ppc/include -I../../sdk
# The agent's failed-allocation reports (sdk/hbc_agent/alloc_wrap.c).
WRAP = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(AGENT_LIB) -L$(DEVKITPRO)/$(OGC)/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib

$(APP).dol: $(APP).elf
	$(ELF2DOL) $< $@

$(APP).elf: main.c ../../sdk/hbc_netlog.h ../../sdk/hbc_agent.h $(AGENT_LIB)/libhbcagent.a
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(WRAP) -lhbcagent -lwiiuse -lbte -lfat -lz -logc -lm -o $@

$(AGENT_LIB)/libhbcagent.a: FORCE
	$(MAKE) -C $(AGENT) OGC=$(OGC) EXTRA_CFLAGS="$(EXTRA_CFLAGS)"

clean:
	rm -f agent_app*.elf agent_app*.dol

FORCE:
.PHONY: clean FORCE
