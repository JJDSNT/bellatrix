; cardldr.S — DiagArea boot ROM for the bellatrix.rtg board.
;
; Copies the pattern of lide.device's bootldr.S (GPL-2.0-only), itself
; based on the open source A4091.device by Stefan Reinauer & Chris
; Hooper. WORDWIDE only: the RTG board window is plain memory.
;
; Layout served by the board window (see src/machine/expansions/rtg):
;   0x0100  this DiagArea (er_InitDiagVec = 0x0100)
;   0x2000  bellatrix.card hunk executable (relocated by reloc.S)
;
; At romtaginit the OS copies the DiagArea, calls DiagEntry (patches the
; CardLoader romtag) and later InitResidents it. CardLoader's Init then
; relocates the card binary from board+0x2000 and InitResidents the
; card's own romtag, putting "bellatrix.card" in the LibList before the
; AROS p96gfx HIDD (residentpri -10) scans for "*.card" libraries.

        include defs.i

VERSION     = 1
CARDOFFSET  = $2000

*******  DiagStart  **************************************************
DiagStart
        dc.b    DAC_WORDWIDE+DAC_CONFIGTIME    ; da_Config
        dc.b    0                              ; da_Flags
        dc.w    EndCopy-DiagStart              ; da_Size
        dc.w    DiagEntry-DiagStart            ; da_DiagPoint
        dc.w    0                              ; da_BootPoint (not a boot device)
        dc.w    CardLdrName-DiagStart          ; da_Name
        dc.w    0                              ; da_Reserved01
        dc.w    0                              ; da_Reserved02

*******  DiagEntry  **************************************************
* success = DiagEntry(BoardBase, DiagCopy, configDev)
* d0                  a0         a2        a3
* Patch the absolute pointers of the CardLoader romtag inside the RAM
* copy and remember BoardBase for Init.
DiagEntry
        movem.l d2-d7/a2-a6,-(sp)
        lea.l   Romtag(pc),a4
        move.l  a2,d0
        add.l   d0,RT_MATCHTAG(a4)
        add.l   d0,RT_ENDSKIP(a4)
        add.l   d0,RT_NAME(a4)
        add.l   d0,RT_IDSTRING(a4)
        add.l   d0,RT_INIT(a4)
        move.l  a0,BoardBase-Romtag(a4)
        movem.l (sp)+,d2-d7/a2-a6
        moveq.l #1,d0
        rts

*******  CardLoader romtag  ******************************************
Romtag
        dc.w    RTC_MATCHWORD      ; UWORD RT_MATCHWORD
        dc.l    Romtag             ; APTR  RT_MATCHTAG   (patched)
        dc.l    EndCopy            ; APTR  RT_ENDSKIP    (patched)
        dc.b    RTF_COLDSTART      ; UBYTE RT_FLAGS
        dc.b    VERSION            ; UBYTE RT_VERSION
        dc.b    0                  ; UBYTE RT_TYPE
        dc.b    0                  ; BYTE  RT_PRI  (> p96gfx's -10)
        dc.l    CardLdrName        ; APTR  RT_NAME      (patched)
        dc.l    CardLdrName        ; APTR  RT_IDSTRING  (patched)
        dc.l    Init               ; APTR  RT_INIT      (patched)

BoardBase:   dc.l 0

*******  Init: relocate the card from ROM and init its romtag  *******
* REG_DEBUG breadcrumbs (host logs [RTG-DBG]):
*   $B0000001 Init entered        $B0000002 relocate ok (next: seg addr)
*   $B00000EE relocate failed     $B0000003 romtag found (next: rt addr)
*   $B00000EF romtag not found    $B0000004 InitResident returned
REG_DEBUG equ $34
Init:   movem.l d2-d7/a2-a6,-(sp)
        move.l  BoardBase(pc),a2
        move.l  #$B0000001,REG_DEBUG(a2)
        move.l  a2,a0
        add.l   #CARDOFFSET,a0     ; card hunk lives at board+CARDOFFSET
        moveq.l #0,d0              ; offset 0: read as plain memory
        bsr     _relocate
        tst.l   d0
        bne.s   .ok
        move.l  #$B00000EE,REG_DEBUG(a2)
        bra.s   .fail
.ok     move.l  #$B0000002,REG_DEBUG(a2)
        move.l  d0,REG_DEBUG(a2)
        bsr     InitRT
        move.l  #$B0000004,REG_DEBUG(a2)
.fail   movem.l (sp)+,d2-d7/a2-a6
        moveq.l #1,d0              ; keep the loader resident copy
        rts

*******  InitRT: find romtag inside relocated segment, InitResident  *
* in: d0 = relocated segment, a2 = BoardBase (kept for breadcrumbs)
InitRT
        movem.l d2/d3/a1,-(sp)
        move.l  #$1000,d3          ; search first 4096 words
        move.l  d0,a1
.findrt cmpi.w  #$4AFC,(a1)
        bne.s   .not_rt
        cmp.l   RT_MATCHTAG(a1),a1
        beq.s   .found
.not_rt addq.l  #2,a1
        dbra    d3,.findrt
        move.l  #$B00000EF,REG_DEBUG(a2)
        movem.l (sp)+,d2/d3/a1
        moveq.l #-1,d0
        rts
.found  move.l  #$B0000003,REG_DEBUG(a2)
        move.l  a1,REG_DEBUG(a2)
        moveq.l #0,d1
        jsr     _LVOInitResident(a6)
        movem.l (sp)+,d2/d3/a1
        moveq.l #0,d0
        rts

CardLdrName
        dc.b    'BellatrixRTG CardLoader',0
        align 1

        include reloc.S
EndCopy:
