/*
 * Does dma.resource work on this target?
 *
 * The resource is ported and linked, and nothing uses it yet -- the SDHOST
 * backend that would is a controller switch, decided separately. So there is
 * nothing to say whether it initialised, whether the channel pool matches what
 * the VideoCore firmware left free, or whether allocating a channel touches
 * the right registers. A resource with no consumer is indistinguishable from a
 * resource that is broken.
 *
 * This is the smallest consumer that answers that: open it, take a channel,
 * report the number, give it back. It is ISSUE-0013's third and fourth
 * acceptance criteria and nothing more -- it moves no bytes, because moving
 * bytes correctly is a claim that needs a real transfer verified against the
 * same read done in PIO, not a probe.
 *
 * Turn this off once something real opens the resource. Until then it is the
 * only thing that would notice a regression.
 */
#define EMU68_DMA_PROBE 1

#if EMU68_DMA_PROBE

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dma.h>

#include <hardware/bcm2708_dma.h>

/*
 * A resident, at the same slot the scheduler selftest uses and for the same
 * reason: InitCode(RTF_COLDSTART) never returns, so anything the bootstrap
 * schedules after it is unreachable. dma.resource is residentpri 88 and
 * dosboot.resource is -50, so -48 runs with the resource long since up and
 * still before the boot takes over.
 *
 * The tag must live in .aros.romtag rather than plain .rodata -- see
 * boot/emu68.ld: as ordinary .rodata it lands inside the span dosboot's
 * rt_EndSkip tells the ROMTag scanner to jump over, and is never seen.
 */
static AROS_UFH3(void, dma_probe_ResidentInit,
    AROS_UFHA(struct Library *, lh, D0),
    AROS_UFHA(BPTR, segList, A0),
    AROS_UFHA(struct ExecBase *, sysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Library *DMABase;

    (void)lh;
    (void)segList;
    (void)sysBase;

    DMABase = (struct Library *)OpenResource("dma.resource");
    if (DMABase == NULL)
    {
        bug("[DMA:probe] dma.resource absent\n");
    }
    else
    {
        /*
         * DMACHF_IRQ asks the resource to own the channel's completion
         * interrupt, which is the path a real consumer would take and the one
         * worth exercising: it reaches KrnAddIRQHandler(IRQ_DMA0 + channel),
         * and this port has exactly one interrupt delivery path for it to
         * arrive on. See AI_context/issues/ISSUE-0039.md.
         */
        int channel = DMAAllocChannel(DMACHF_IRQ);

        if (channel < 0)
            bug("[DMA:probe] no channel available\n");
        else
        {
            bug("[DMA:probe] allocated channel %ld\n", (long)channel);
            DMAFreeChannel(channel);
            bug("[DMA:probe] released channel %ld\n", (long)channel);
        }
    }

    AROS_USERFUNC_EXIT
}

static const struct Resident emu68_dma_probe_romtag
    __attribute__((section(".aros.romtag"), used)) =
{
    RTC_MATCHWORD,
    (struct Resident *)&emu68_dma_probe_romtag,
    (APTR)(&emu68_dma_probe_romtag + 1),
    RTF_COLDSTART,
    41,
    NT_UNKNOWN,
    -48,
    "emu68dmaprobe",
    "emu68 dma.resource probe 41.1\r\n",
    (APTR)dma_probe_ResidentInit
};

#endif /* EMU68_DMA_PROBE */
