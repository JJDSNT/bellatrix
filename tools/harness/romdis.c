#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

static uint8_t *g_rom;
static uint32_t g_rom_size;
static uint32_t g_rom_base;

static uint32_t rom_offset(uint32_t address)
{
    if (address < g_rom_base)
        return UINT32_MAX;

    address -= g_rom_base;
    if (address >= g_rom_size)
        return UINT32_MAX;

    return address;
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{
    uint32_t off = rom_offset(address);
    if (off == UINT32_MAX)
        return 0;
    return g_rom[off];
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    uint32_t off = rom_offset(address);
    if (off == UINT32_MAX || off + 1u >= g_rom_size)
        return 0;
    return ((unsigned int)g_rom[off] << 8) | (unsigned int)g_rom[off + 1u];
}

unsigned int m68k_read_disassembler_32(unsigned int address)
{
    uint32_t off = rom_offset(address);
    if (off == UINT32_MAX || off + 3u >= g_rom_size)
        return 0;
    return ((unsigned int)g_rom[off] << 24) |
           ((unsigned int)g_rom[off + 1u] << 16) |
           ((unsigned int)g_rom[off + 2u] << 8) |
           (unsigned int)g_rom[off + 3u];
}

unsigned int m68k_read_immediate_16(unsigned int address) { return m68k_read_disassembler_16(address); }
unsigned int m68k_read_immediate_32(unsigned int address) { return m68k_read_disassembler_32(address); }
unsigned int m68k_read_pcrelative_8(unsigned int address) { return m68k_read_disassembler_8(address); }
unsigned int m68k_read_pcrelative_16(unsigned int address) { return m68k_read_disassembler_16(address); }
unsigned int m68k_read_pcrelative_32(unsigned int address) { return m68k_read_disassembler_32(address); }
unsigned int m68k_read_memory_8(unsigned int address) { return m68k_read_disassembler_8(address); }
unsigned int m68k_read_memory_16(unsigned int address) { return m68k_read_disassembler_16(address); }
unsigned int m68k_read_memory_32(unsigned int address) { return m68k_read_disassembler_32(address); }
void m68k_write_memory_8(unsigned int address, unsigned int value) { (void)address; (void)value; }
void m68k_write_memory_16(unsigned int address, unsigned int value) { (void)address; (void)value; }
void m68k_write_memory_32(unsigned int address, unsigned int value) { (void)address; (void)value; }

static uint8_t *load_file(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;

    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = (uint32_t)size;
    return buf;
}

int main(int argc, char **argv)
{
    uint32_t start, count, pc, i;
    char text[128];

    if (argc != 4) {
        fprintf(stderr, "usage: romdis <rom> <start-hex> <count>\n");
        return 1;
    }

    g_rom = load_file(argv[1], &g_rom_size);
    if (!g_rom) {
        fprintf(stderr, "failed to load ROM: %s\n", argv[1]);
        return 1;
    }

    g_rom_base = (g_rom_size <= 256u * 1024u) ? 0x00fc0000u : 0x00f80000u;
    start = (uint32_t)strtoul(argv[2], NULL, 0);
    count = (uint32_t)strtoul(argv[3], NULL, 0);
    pc = start;

    for (i = 0; i < count; i++) {
        unsigned int size = m68k_disassemble(text, pc, M68K_CPU_TYPE_68000);
        printf("%08x: %s\n", pc, text);
        if (!size)
            break;
        pc += size;
    }

    free(g_rom);
    return 0;
}
