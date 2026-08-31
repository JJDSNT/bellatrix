/*
 * src/machine/options.c
 *
 * The boot arguments, read once and turned into what this machine is.
 *
 * Emu68 keeps the mechanism and the embedder keeps the policy -- the same
 * split as the console hand-off (patch 0021) and the memory map (patch 0005).
 * parse_cmdline() calls in here before it decides anything of its own
 * (patch 0025), so every answer below is settled before the MMU is programmed,
 * before the secondary cores are released, and long before the first
 * instruction is translated. Nothing here changes afterwards.
 *
 * Why the chipset is a boot-time choice rather than a build-time one:
 * CONFIG_RIGEL says whether the image *carries* Rigel, and that is a real
 * question -- a build without it links no chipset at all. `rigel` on the
 * command line says whether this boot *uses* it. A card in someone else's
 * hands cannot be rebuilt to answer a question, and the two compositions
 * differ in exactly the ways an investigation wants to alternate between: who
 * owns the low 24 bits, whether the guest gets a separate chip-memory pool,
 * whether three cores are handed out, and how STOP waits.
 */

#include "machine/options.h"

#include "A64.h"
#include "support.h"

/*
 * The name is the bare word `rigel`, not a `bellatrix.rigel=1` key.
 *
 * Emu68's own arguments are bare words -- `enable_cache`, `nofpu`, `ps32` --
 * and cmdline.txt is a single line a person edits with the card in a reader.
 * Absence is the whole of the "off" case: there is no `rigel=0` to get wrong,
 * and no second spelling that has to agree with the first.
 */
#define OPTION_RIGEL    "rigel"
#define OPTION_VECPAGE  "bellatrix.vecpage"

static int option_rigel;
static int option_vecpage;

void bellatrix_parse_cmdline(const char *cmdline)
{
    if (cmdline == 0)
        return;

    /*
     * Accumulate, never assign.
     *
     * Emu68 calls parse_cmdline twice -- once for /chosen/bootargs and again
     * for /emu68/args when an overlay carries one (src/aarch64/start.c). Its
     * own flags assign on each call, so the second string silently clears
     * whatever the first asked for. That is a trap worth not inheriting: an
     * argument that works alone and stops working when an overlay is present
     * is indistinguishable from an argument that was never read.
     */
    if (find_token(cmdline, OPTION_RIGEL))
        option_rigel = 1;
    if (find_token(cmdline, OPTION_VECPAGE))
        option_vecpage = 1;
}

int bellatrix_rigel_enabled(void)
{
#if CONFIG_RIGEL
    return option_rigel;
#else
    return 0;
#endif
}

int bellatrix_vecpage_trapped(void)
{
    return option_vecpage;
}

void bellatrix_options_report(void)
{
#if CONFIG_RIGEL
    if (option_rigel)
        kprintf("[BELLATRIX] chipset: Rigel, asked for by \"" OPTION_RIGEL
                "\" on the command line\n");
    else
        kprintf("[BELLATRIX] chipset: none -- add \"" OPTION_RIGEL "\" to the"
                " command line for the classic chipset\n");
#else
    /*
     * Asked for something this image does not carry. Say so rather than
     * booting a machine that is quietly not the one that was requested: the
     * guest reads the same word and will lay out its memory for a chipset that
     * is not there.
     */
    if (option_rigel)
        kprintf("[BELLATRIX] chipset: \"" OPTION_RIGEL "\" was asked for, but"
                " this image was built with CONFIG_RIGEL=0 and carries none\n");
    else
        kprintf("[BELLATRIX] chipset: none, and none is built in\n");
#endif

    if (option_vecpage)
        kprintf("[BELLATRIX:VECPAGE] armed by the command line:"
                " the first page of chip RAM is fault-driven\n");
}
