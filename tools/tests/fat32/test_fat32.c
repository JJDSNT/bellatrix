// Host-side test for src/storage/fat: LFN listing, unicode, open, read.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "storage/fat/fat32.h"

int kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

static FILE *g_img;

static bool file_read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) return false;
    return fread(buf, 1, 512, g_img) == 512;
}

static int g_fail;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); g_fail++; } \
} while (0)

int main(int argc, char **argv)
{
    g_img = fopen(argv[1], "rb");
    if (!g_img) { perror("open"); return 1; }

    Fat32State fs;
    CHECK(fat32_init_with_reader(&fs, file_read_block, NULL), "init");

    static char names[32][FAT32_NAME_MAX];
    uint32_t n = fat32_list(&fs, names, 32);
    printf("listed %u files:\n", n);
    for (uint32_t i = 0; i < n; i++) printf("  [%u] '%s'\n", i, names[i]);

    CHECK(n == 4, "4 regular files listed (dir + volume label skipped)");

    bool found_lfn = false, found_iso = false, found_83 = false, found_txt = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(names[i], "Turrican II - The Final Fight (1991) Edi\xc3\xa7\xc3\xa3o A\xc3\xa7\xc3\xa3o\xc3\xa7\xc3\xa9.adf")) found_lfn = true;
        if (!strcmp(names[i], "Some Really Long CD Image Name With Many Many Words In It.iso")) found_iso = true;
        if (!strcmp(names[i], "GAME.ADF")) found_83 = true;
        if (!strcmp(names[i], "README.txt")) found_txt = true;
    }
    CHECK(found_lfn, "unicode LFN listed (UTF-8, accents intact)");
    CHECK(found_iso, "long ASCII LFN listed");
    CHECK(found_83,  "plain 8.3 name listed");
    CHECK(found_txt, "non-media file listed (no filter in FAT layer)");

    // open by exact LFN
    Fat32File f;
    CHECK(fat32_open(&fs, "Some Really Long CD Image Name With Many Many Words In It.iso", &f), "open by LFN");
    char buf[64] = {0};
    uint32_t got = fat32_read(&f, buf, sizeof(buf));
    CHECK(got == 9 && !strcmp(buf, "ISOISOISO"), "read content via LFN open");

    // open case-insensitively
    CHECK(fat32_open(&fs, "some really long cd image name with many many words in it.ISO", &f), "open LFN case-insensitive");

    // open by 8.3 alias of an LFN file
    CHECK(fat32_open(&fs, "SOMERE~1.ISO", &f), "open by 8.3 alias");

    // open plain 8.3 file, lowercase
    CHECK(fat32_open(&fs, "game.adf", &f), "open 8.3 case-insensitive");
    memset(buf, 0, sizeof(buf));
    got = fat32_read(&f, buf, sizeof(buf));
    CHECK(got == 25 && !strcmp(buf, "conteudo ADF teste 12345\n"), "read content via 8.3 open");

    // negative
    CHECK(!fat32_open(&fs, "missing.adf", &f), "missing file rejected");

    printf("%s (%d failures)\n", g_fail ? "TEST FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
