# bellatrix.card — build with the m68k-amigaos docker toolchain:
#   TTY_ENABLED="" ../../emu68/build-scripts/build-m68k-amigaos make
CC = m68k-amigaos-gcc
CFLAGS = -Os -m68020 -fomit-frame-pointer -noixemul -fbaserel \
         -Wall -Wextra -Wno-unused-parameter -Isrc

all: bellatrix.card

bellatrix.card: src/card.c src/boardinfo.h
	$(CC) $(CFLAGS) -nostartfiles -o $@ src/card.c

clean:
	rm -f bellatrix.card

.PHONY: all clean
