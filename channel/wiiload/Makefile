CFLAGS += -Wall -Wextra -Os -g -pipe $(EXTRA_CFLAGS)

# Build natively on each platform; MSYS2 and Cygwin compilers add .exe.
PREFIX ?=
BIN_EXT := $(if $(findstring _NT,$(shell uname -s)),.exe)
LDFLAGS += -lz

ifeq ($(FTDI), 1)
CFLAGS += -DUSE_LIBFTDI
LDFLAGS += -lftdi -lusb
endif

CC = $(PREFIX)gcc
STRIP = $(PREFIX)strip

BIN = wiiload$(BIN_EXT)

OBJS = gecko.o main.o

all: $(BIN)

clean:
	@rm -f *.o core core.* $(BIN)

strip: all
	$(STRIP) $(BIN)

install: strip
	install -m 755 $(BIN) $(DEVKITPPC)/bin

$(BIN): $(OBJS)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean strip install

