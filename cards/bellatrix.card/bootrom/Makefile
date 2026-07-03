# bellatrix.rtg boot ROM — DiagArea loader + relocated card.
# Build inside the docker toolchain:
#   TTY_ENABLED="" ../../../emu68/build-scripts/build-m68k-amigaos make
AS = vasmm68k_mot
ASFLAGS = -Fbin -quiet -align -DROM
OBJDIR = obj

# window offsets (see docs/rtg_design.md): loader served at 0x0100,
# card hunk at 0x2000; the rom image starts at window offset 0x0100.
CARD_FILE_OFF = 7936   # 0x2000 - 0x0100

all: ../rtg.rom

$(OBJDIR)/cardldr: cardldr.S reloc.S defs.i
	@mkdir -p $(OBJDIR)
	$(AS) $(ASFLAGS) -o $@ cardldr.S

../rtg.rom: $(OBJDIR)/cardldr ../bellatrix.card
	@SIZE=$$(wc -c < $(OBJDIR)/cardldr); \
	if [ $$SIZE -gt $(CARD_FILE_OFF) ]; then \
	    echo "cardldr too large: $$SIZE > $(CARD_FILE_OFF)"; exit 1; fi
	cp $(OBJDIR)/cardldr $@.tmp
	@SIZE=$$(wc -c < $@.tmp); \
	dd if=/dev/zero bs=1 count=$$(( $(CARD_FILE_OFF) - $$SIZE )) >> $@.tmp 2>/dev/null
	cat ../bellatrix.card >> $@.tmp
	mv $@.tmp $@
	@echo "rtg.rom: $$(wc -c < $@) bytes"

clean:
	rm -rf $(OBJDIR) ../rtg.rom

.PHONY: all clean
