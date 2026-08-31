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
#include "devicetree.h"
#include "support.h"

#include <stdint.h>

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

/*
 * The guest is told what the machine is by being handed the command line the
 * machine implemented, rather than the one that was asked for.
 *
 * Both halves of this port read `rigel`: the host to build the classic address
 * map, the guest to lay out its memory to match. They can only disagree in one
 * direction -- the word is on the line and this image carries no chipset to
 * enable -- and then the guest builds a chip-memory pool over addresses that
 * fault. Correcting the copy closes that by construction: there is one reader
 * of one string, and the string says what actually happened.
 *
 * It has to be done by correcting the copy in place, and that is not a stylistic
 * choice. `dt_add_property()` edits Emu68's own parsed tree, while the guest
 * receives `memcpy(fdt, dt_fdt_base(), dt_total_size())` -- a byte copy of the
 * original blob, which has no /emu68 node in it at all. Three attempts to
 * publish a property there have already been made and none arrived; see
 * patches/emu68/0007, which corrects the guest's /memory the same way and for
 * the same reason.
 *
 * Blanking is length-preserving, so nothing in the flattened blob moves: the
 * word becomes spaces, which the token scanners on both sides skip.
 *
 * The command line stays the authority in the direction that matters. Nothing
 * here can turn the chipset *on* -- a line without `rigel` is left exactly as
 * it is, whatever the image carries.
 */
void bellatrix_correct_guest_cmdline(void *guest_fdt, const void *own_fdt)
{
    of_node_t *chosen;
    of_property_t *bootargs;
    char *line;
    uint32_t length;
    uint32_t i;
    int blanked = 0;

    if (guest_fdt == 0 || own_fdt == 0 || bellatrix_rigel_enabled() ||
        !option_rigel)
        return;

    chosen = dt_find_node("/chosen");
    bootargs = chosen ? dt_find_property(chosen, "bootargs") : 0;
    if (bootargs == 0 || bootargs->op_value == 0 || bootargs->op_length == 0)
        return;

    line = (char *)((uintptr_t)guest_fdt +
                    ((uintptr_t)bootargs->op_value - (uintptr_t)own_fdt));
    length = bootargs->op_length;

    for (i = 0; i < length; i++)
    {
        uint32_t j;

        /* Whole words only, the same rule Emu68's find_token() applies. */
        if (i != 0 && line[i - 1] != ' ' && line[i - 1] != '\t')
            continue;

        for (j = 0; j < sizeof(OPTION_RIGEL) - 1; j++)
            if (i + j >= length || line[i + j] != OPTION_RIGEL[j])
                break;
        if (j != sizeof(OPTION_RIGEL) - 1)
            continue;

        if (i + j != length && line[i + j] != ' ' && line[i + j] != '\t' &&
            line[i + j] != '\0')
            continue;

        for (j = 0; j < sizeof(OPTION_RIGEL) - 1; j++)
            line[i + j] = ' ';
        blanked = 1;
    }

    if (blanked)
        kprintf("[BELLATRIX] \"" OPTION_RIGEL "\" removed from the command"
                " line handed to the guest: this image carries no chipset\n");
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
