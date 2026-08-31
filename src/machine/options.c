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

/*
 * The one option here that carries a value, and it is spelled the way Emu68
 * spells its own (`ICNT=`, `CCRD=`, `cs_dist=`): find_token() treats a token
 * ending in `=` as a prefix match, so the digits are read from just past it.
 */
#define OPTION_CHIPDIV  "bellatrix.chipdiv="

static int option_rigel;
static int option_vecpage;
static uint32_t option_chipdiv = 1;

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

    /*
     * How much slower than real time the chipset's clock is allowed to run.
     *
     * Chipset time comes from the wall clock and from nothing else
     * (src/amiga/bus.c), which is right for the machine this is: on a Pi 3 the
     * chipset sustains 112% of realtime, so it asks for 3546895 colour clocks
     * a second and can deliver them. On a host that cannot, the same code
     * still asks for all of them, gets about a fifth of the way, and has the
     * rest discarded by the catch-up cap -- measured under QEMU/TCG: 690000
     * CCK/s against 3546895 asked for, the chipset core inside amiga_clock_step
     * 95% of the time, and 81% of the requested colour clocks silently dropped.
     *
     * That is already a divisor. It is simply an implicit one, it is not
     * measured, it varies with load, and it is paid for with a core pegged at
     * 100% believing it is behind -- which is also what makes every CPU access
     * to the classic domain queue behind the chipset lock.
     *
     * So make it explicit. `bellatrix.chipdiv=N` scales the rate at which real
     * time is turned into colour clocks; nothing inside amiga_clock_step()
     * changes, so a colour clock still costs exactly what a colour clock costs
     * and cycle-exactness is untouched. What changes is how many of them a
     * second of real time buys.
     *
     * 1 by default, so hardware is unaffected and this cannot become a
     * performance default that nobody chose. Under QEMU, divide the realtime
     * rate by the "CCK/s" the [BELLATRIX:RIGEL:PERF] line reports and round up
     * generously -- the core wants slack, not a rate it can only just meet.
     *
     * Assigning only when the token is present is the accumulate rule above:
     * a second parse_cmdline() call with an overlay line must not silently
     * reset this to 1.
     */
    {
        const char *tok = find_token(cmdline, OPTION_CHIPDIV);

        if (tok != 0)
        {
            const char *c = &tok[sizeof(OPTION_CHIPDIV) - 1];
            uint32_t val = 0;
            int i;

            for (i = 0; i < 4; i++)
            {
                if (c[i] < '0' || c[i] > '9')
                    break;
                val = val * 10u + (uint32_t)(c[i] - '0');
            }

            /*
             * A missing or zero value means 1, not "stop the chipset". The
             * clamp is there because `pending_cck` would otherwise take
             * minutes of real time to reach a single colour clock, which is a
             * machine that looks hung rather than one that looks slow.
             */
            if (val == 0)
                val = 1;
            if (val > 1024)
                val = 1024;

            option_chipdiv = val;
        }
    }
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

uint32_t bellatrix_chipset_divisor(void)
{
    return option_chipdiv;
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

    /*
     * Only worth a line when it is not 1: on hardware it always is, and a
     * boot log that says "the chipset runs at real time" every time teaches
     * the reader to skip the line that matters.
     */
    if (option_chipdiv != 1)
        kprintf("[BELLATRIX:RIGEL] chipset clock: 1/%u of real time, asked for"
                " by \"" OPTION_CHIPDIV "%u\" on the command line\n",
                (unsigned)option_chipdiv, (unsigned)option_chipdiv);
}
