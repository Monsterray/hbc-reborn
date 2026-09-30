# libhbcagent.a: the in-app agent (../hbc_agent.h). It shares HBC's file
# request, framed transfer and TCP code from channel/channelapp/source, built
# with two transfer slots and a smaller worker stack.
#
#   make -C sdk/hbc_agent
#   app: -I<hbc-reborn>/sdk ... -L<hbc-reborn>/sdk/hbc_agent -lhbcagent -lfat -lz -logc
#
# For an app on libogc2 or libogc 1.x, name the libogc under $(DEVKITPRO):
#   make -C sdk/hbc_agent OGC=libogc2        ->  sdk/hbc_agent/libogc2/libhbcagent.a
#   make -C sdk/hbc_agent OGC=libogc-1.8.23  ->  sdk/hbc_agent/libogc-1.8.23/libhbcagent.a
#   app: ... -L<hbc-reborn>/sdk/hbc_agent/libogc2 -lhbcagent ...

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

PREFIX = $(DEVKITPPC)/bin/powerpc-eabi-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar

HBC_SRC = ../../channel/channelapp/source
OGC ?= libogc
ifeq ($(OGC),libogc)
OUT = libhbcagent.a
BUILD = build
else
OUT = $(OGC)/libhbcagent.a
BUILD = build/$(OGC)
endif

CFLAGS = -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall $(EXTRA_CFLAGS) \
	-DDEVSTREAM_SLOTS=2 -DDEVSTREAM_WORKER_STACK=16384 \
	-I$(HBC_SRC) -I.. \
	-I$(DEVKITPRO)/$(OGC)/include -I$(DEVKITPRO)/portlibs/ppc/include

SRC = agent.c ogc_exc.S overlay.c ov_ui.c ov_draw.c ../wiispk/wiispk.c $(HBC_SRC)/devfile.c $(HBC_SRC)/devstream.c $(HBC_SRC)/tcp.c $(HBC_SRC)/zmem.c
OBJ = $(addprefix $(BUILD)/,$(notdir $(patsubst %.S,%.o,$(SRC:.c=.o))))
HDR = ../hbc_agent.h ../hbc_netlog.h $(wildcard $(HBC_SRC)/*.h) ../../channel/channelapp/config.h

$(OUT): $(OBJ)
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c $(HDR) agent_int.h ogc_flavor.h ov_ui.h ov_draw.h ov_font.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S ogc_flavor.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(HBC_SRC)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: ../wiispk/%.c ../wiispk/wiispk.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf build libhbcagent.a libogc2 libogc-1.*

.PHONY: clean
