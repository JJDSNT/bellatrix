; defs.i — minimal NDK constants (the docker image lacks the asm NDK).
; Values from AmigaOS NDK exec/resident.h, exec/memory.h,
; libraries/configvars.h and exec LVOs.

; exec/resident.h
RTC_MATCHWORD   EQU $4AFC
RT_MATCHTAG     EQU 2
RT_ENDSKIP      EQU 6
RT_FLAGS        EQU 10
RT_VERSION      EQU 11
RT_TYPE         EQU 12
RT_PRI          EQU 13
RT_NAME         EQU 14
RT_IDSTRING     EQU 18
RT_INIT         EQU 22
RTF_AUTOINIT    EQU $80
RTF_COLDSTART   EQU $01

; libraries/configvars.h (DiagArea da_Config) — bus width is in the HIGH
; nibble (0xC0 mask); getting this wrong makes AROS decode the DiagArea
; as NIBBLEWIDE (reading every 4th byte) instead of WORDWIDE (plain
; memcpy), producing garbage da_Size/da_DiagPoint (cost a full debugging
; session on 2026-07-03 — verify against external/aros/compiler/include/
; libraries/configregs.h if this ever needs touching again).
DAC_BUSWIDTH    EQU $C0
DAC_NIBBLEWIDE  EQU $00
DAC_BYTEWIDE    EQU $40
DAC_WORDWIDE    EQU $80
DAC_BOOTTIME    EQU $30
DAC_CONFIGTIME  EQU $10

; exec/memory.h
MEMF_PUBLIC     EQU $0001
MEMF_CLEAR      EQU $10000

; exec LVOs
_LVOInitResident EQU -102
_LVOAllocMem     EQU -198
_LVOFreeMem      EQU -210
