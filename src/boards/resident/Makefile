# src/boards/resident/Makefile
#
# Builds cdmount.rom — the m68k Autoconfig Z2 DiagArea ROM image.
# AROS expansion library reads er_InitDiagVec=0x0010, copies 0x8000 bytes
# to AllocMem, and calls da_DiagPoint at copy offset 0x000E (ROM 0x001E).
#
# Invoke via the project Docker wrapper (no local toolchain needed):
#   TTY_ENABLED="" emu68/build-scripts/build-m68k-amigaos make -C src/boards/resident
#
# Variables M68K_CC / M68K_OBJCOPY are exported by emu68/examples/Makefile
# and set to m68k-amigaos-gcc / m68k-amigaos-objcopy inside Docker.
M68K_CC      ?= m68k-amigaos-gcc
M68K_OBJCOPY ?= m68k-amigaos-objcopy

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
MOUNTER   := $(REPO_ROOT)/external/mounter
LDSCRIPT  := $(dir $(lastword $(MAKEFILE_LIST)))resident.ld

CFLAGS  = -m68000               \
           -O2                   \
           -fomit-frame-pointer  \
           -nostdlib             \
           -ffreestanding        \
           -fno-builtin          \
           -Wall                 \
           -mpcrel               \
           -I$(MOUNTER)

# resident.ld places:
#   .text.romhdr   at 0x0000  (16 bytes padding)
#   .text.diagarea at 0x0010  (struct DiagArea, 14 bytes)
#   .text          at 0x001E  (diag_point first, then mount_task + mounter)
LDFLAGS = -nostdlib -T $(LDSCRIPT)

TARGET  = cdmount.rom
ELF     = cdmount.elf
SRC     = resident.c

.PHONY: all clean

all: $(TARGET)

$(ELF): $(SRC) $(LDSCRIPT)
	$(M68K_CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(TARGET): $(ELF)
	$(M68K_OBJCOPY) -O binary $< $@
	@echo "ROM size: $$(wc -c < $@) bytes"
	@echo "Deploy:   harness ... --cd-board $$(pwd)/$@"

clean:
	rm -f $(ELF) $(TARGET)
