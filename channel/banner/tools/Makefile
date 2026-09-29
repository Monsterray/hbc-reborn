CFLAGS = -O3 -Wall $(EXTRA_CFLAGS)

CC = $(PREFIX)gcc
STRIP = $(PREFIX)strip

all: mkbns$(EXE) lz77$(EXE)

mkbns$(EXE): mkbns.c
	$(CC) $(CFLAGS) -o $@ mkbns.c -lm

lz77$(EXE): lz77.c
	$(CC) $(CFLAGS) -o $@ lz77.c

clean:
	rm -f mkbns$(EXE) lz77$(EXE)
