/*
    Copyright © 2021 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    This file is based on FreeBSD SDHOST driver, see following license and copyright:
*/

/*
 *
 * Copyright (c) 2018 Klaus P. Ohrhallinger <k@7he.at>
 * All rights reserved.
 *
 * Based on bcm2835_sdhci.c:
 * Copyright (c) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/errors.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/devicetree.h>

#include <libraries/configregs.h>
#include <libraries/configvars.h>

#include <stdint.h>
#include "sdcard.h"
#include "sdhost.h"


/* Registers */
#define HC_COMMAND          0x00	/* Command and flags */
#define HC_ARGUMENT         0x04
#define HC_TIMEOUTCOUNTER   0x08
#define HC_CLOCKDIVISOR     0x0c
#define HC_RESPONSE_0       0x10
#define HC_RESPONSE_1       0x14
#define HC_RESPONSE_2       0x18
#define HC_RESPONSE_3       0x1c
#define HC_HOSTSTATUS       0x20
#define HC_POWER            0x30
#define HC_DEBUG            0x34
#define HC_HOSTCONFIG       0x38
#define HC_BLOCKSIZE        0x3c
#define HC_DATAPORT         0x40
#define HC_BLOCKCOUNT       0x50

/* Flags for HC_COMMAND register */
#define HC_CMD_ENABLE           0x8000
#define HC_CMD_FAILED           0x4000
#define HC_CMD_BUSY             0x0800
#define HC_CMD_RESPONSE_NONE    0x0400
#define HC_CMD_RESPONSE_LONG    0x0200
#define HC_CMD_WRITE            0x0080
#define HC_CMD_READ             0x0040
#define HC_CMD_COMMAND_MASK     0x003f

#define HC_CLOCKDIVISOR_MAXVAL  0x07ff

/* Flags for HC_HOSTSTATUS register */
#define HC_HSTST_HAVEDATA       0x0001
#define HC_HSTST_ERROR_FIFO     0x0008
#define HC_HSTST_ERROR_CRC7     0x0010
#define HC_HSTST_ERROR_CRC16    0x0020
#define HC_HSTST_TIMEOUT_CMD    0x0040
#define HC_HSTST_TIMEOUT_DATA   0x0080
#define HC_HSTST_INT_SDIO       0x0100
#define HC_HSTST_INT_BLOCK      0x0200
#define HC_HSTST_INT_BUSY       0x0400

#define HC_HSTST_RESET          0xffff

#define HC_HSTST_MASK_ERROR_DATA  (HC_HSTST_ERROR_FIFO |                \
    HC_HSTST_ERROR_CRC7 | HC_HSTST_ERROR_CRC16 | HC_HSTST_TIMEOUT_DATA)

#define HC_HSTST_MASK_ERROR_ALL   (HC_HSTST_MASK_ERROR_DATA |           \
    HC_HSTST_TIMEOUT_CMD)

/* Flags for HC_HOSTCONFIG register */
#define HC_HSTCF_REL_CMD_LINE   0x0001
#define HC_HSTCF_INTBUS_WIDE    0x0002
#define HC_HSTCF_EXTBUS_4BIT    0x0004
#define HC_HSTCF_SLOW_CARD      0x0008
#define HC_HSTCF_INT_DATA       0x0010
#define HC_HSTCF_INT_SDIO       0x0020
#define HC_HSTCF_INT_BLOCK      0x0100
#define HC_HSTCF_INT_BUSY       0x0400

/* Flags for HC_DEBUG register */
#define HC_DBG_FIFO_THRESH_WRITE_SHIFT  9
#define HC_DBG_FIFO_THRESH_READ_SHIFT   14
#define HC_DBG_FIFO_THRESH_MASK         0x001f

#define HC_DBG_FSM_MASK           0xf
#define HC_DBG_FSM_IDENTMODE      0x0
#define HC_DBG_FSM_DATAMODE       0x1
#define HC_DBG_FSM_READDATA       0x2
#define HC_DBG_FSM_WRITEDATA      0x3
#define HC_DBG_FSM_READWAIT       0x4
#define HC_DBG_FSM_READCRC        0x5
#define HC_DBG_FSM_WRITECRC       0x6
#define HC_DBG_FSM_WRITEWAIT1     0x7
#define HC_DBG_FSM_POWERDOWN      0x8
#define HC_DBG_FSM_POWERUP        0x9
#define HC_DBG_FSM_WRITESTART1    0xa
#define HC_DBG_FSM_WRITESTART2    0xb
#define HC_DBG_FSM_GENPULSES      0xc
#define HC_DBG_FSM_WRITEWAIT2     0xd
#define HC_DBG_FSM_STARTPOWDOWN   0xf

/* Settings */
#define HC_FIFO_SIZE            16
#define HC_FIFO_THRESH_READ     4
#define HC_FIFO_THRESH_WRITE    4
#define HC_FIFO_BURST           8

#define HC_TIMEOUT_DEFAULT      0x00f00000

asm("
    .globl _sdhost_irq_gate
_sdhost_irq_gate:
    bsr.b _sdhost_irq
    tst.l d0
    rts
");

int sdhost_irq()
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;

    if (rd32((APTR)0xf3000000, 0x34) & 0x80000000)
    {
        wr32((APTR)0xf3000000, 0x38, 0x80000000);
        //bug("EXTER from local timer!\n");
        return 1;
    }
    else {
        return 0;
    }
}

ULONG sdhost_getclock(struct SDCardBase *SDCardBase)
{
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    ULONG clock = 0;
    
    do
    {
        clock = SDCardBase->get_clock_rate(4, SDCardBase);
    } while (clock == 0);

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] sdhost_getclock returns %ld\n", clock);

    return clock;
}

void sdhost_led(int on, struct SDCardBase *SDCardBase)
{
    if (on) {
        wr32((APTR)0xf2200000, 0x1c, 1 << 29);
    }
    else {
        wr32((APTR)0xf2200000, 0x28, 1 << 29);
    }
}

void sdhost_led_inverted(int on, struct SDCardBase *SDCardBase)
{
    if (on) {
        wr32((APTR)0xf2200000, 0x28, 1 << 29);
    }
    else {
        wr32((APTR)0xf2200000, 0x1c, 1 << 29);
    }
}

static void sdhost_dump_regs(struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    if (SDCardBase->sd_Verbose) {
        bug("[brcm-sdhc] SDHOST reg dump\n"
                "[brcm-sdhc]    HC_COMMAND:        0x%08lx\n"
                "[brcm-sdhc]    HC_ARGUMENT:       0x%08lx\n"
                "[brcm-sdhc]    HC_TIMEOUTCOUNTER: 0x%08lx\n"
                "[brcm-sdhc]    HC_CLOCKDIVISOR:   0x%08lx\n"
                "[brcm-sdhc]    HC_RESPONSE_0:     0x%08lx\n"
                "[brcm-sdhc]    HC_RESPONSE_1:     0x%08lx\n"
                "[brcm-sdhc]    HC_RESPONSE_2:     0x%08lx\n"
                "[brcm-sdhc]    HC_RESPONSE_3:     0x%08lx\n"
                "[brcm-sdhc]    HC_HOSTSTATUS:     0x%08lx\n"
                "[brcm-sdhc]    HC_POWER:          0x%08lx\n"
                "[brcm-sdhc]    HC_DEBUG:          0x%08lx\n"
                "[brcm-sdhc]    HC_HOSTCONFIG:     0x%08lx\n"
                "[brcm-sdhc]    HC_BLOCKSIZE:      0x%08lx\n"
                "[brcm-sdhc]    HC_BLOCKCOUNT:     0x%08lx\n", 
            rd32(sc, HC_COMMAND),
            rd32(sc, HC_ARGUMENT),
            rd32(sc, HC_TIMEOUTCOUNTER),
            rd32(sc, HC_CLOCKDIVISOR),
            rd32(sc, HC_RESPONSE_0),
            rd32(sc, HC_RESPONSE_1),
            rd32(sc, HC_RESPONSE_2),
            rd32(sc, HC_RESPONSE_3),
            rd32(sc, HC_HOSTSTATUS),
            rd32(sc, HC_POWER),
            rd32(sc, HC_DEBUG),
            rd32(sc, HC_HOSTCONFIG),
            rd32(sc, HC_BLOCKSIZE),
            rd32(sc, HC_BLOCKCOUNT));
    }
}

static void sdhost_reset(struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;
    uint32_t dbg;

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] SDHOST Reset\n");

    wr32(sc, HC_POWER, 0);

    wr32(sc, HC_COMMAND, 0);
    wr32(sc, HC_ARGUMENT, 0);
    wr32(sc, HC_TIMEOUTCOUNTER, HC_TIMEOUT_DEFAULT);
    wr32(sc, HC_CLOCKDIVISOR, 0);
    wr32(sc, HC_HOSTSTATUS, HC_HSTST_RESET);
    wr32(sc, HC_HOSTCONFIG, 0);
    wr32(sc, HC_BLOCKSIZE, 0);
    wr32(sc, HC_BLOCKCOUNT, 0);

    dbg = rd32(sc, HC_DEBUG);
    dbg &= ~( (HC_DBG_FIFO_THRESH_MASK << HC_DBG_FIFO_THRESH_READ_SHIFT) |
              (HC_DBG_FIFO_THRESH_MASK << HC_DBG_FIFO_THRESH_WRITE_SHIFT) );
    dbg |= (HC_FIFO_THRESH_READ << HC_DBG_FIFO_THRESH_READ_SHIFT) |
           (HC_FIFO_THRESH_WRITE << HC_DBG_FIFO_THRESH_WRITE_SHIFT);
    wr32(sc, HC_DEBUG, dbg);

    SDCardBase->sd_Delay(250000, SDCardBase);

    wr32(sc, HC_POWER, 1);

    SDCardBase->sd_Delay(250000, SDCardBase);

    wr32(sc, HC_CLOCKDIVISOR, HC_CLOCKDIVISOR_MAXVAL);
    wr32(sc, HC_HOSTCONFIG, HC_HSTCF_INT_BUSY | HC_HSTCF_INT_BLOCK | HC_HSTCF_INT_DATA | HC_HSTCF_SLOW_CARD | HC_HSTCF_INTBUS_WIDE);
}

int sdhost_powerCycle(struct SDCardBase *SDCardBase)
{
    sdhost_reset(SDCardBase);
    return 0;
}

static void sdhost_set_clock_rate(ULONG freq, struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    ULONG clock = sdhost_getclock(SDCardBase);
    int divider;

    if (freq == 0)
        divider = HC_CLOCKDIVISOR_MAXVAL;
    else
    {
        divider = clock / freq;

        // If requested frequency is too high limit it
        if (divider < 2)
            divider = 2;
        
        // If selected divider is too low (output frequency higher than requested), increase it
        if (clock / divider > freq)
            divider++;
        
        divider -= 2;
        if (divider > HC_CLOCKDIVISOR_MAXVAL)
            divider = HC_CLOCKDIVISOR_MAXVAL;
    }

    if (SDCardBase->sd_Verbose)
    {
        bug("[brcm-sdhc] Requested clock %ld Hz, activated clock %ld Hz\n", freq,
            clock / (divider + 2));
    }

    wr32(sc, HC_CLOCKDIVISOR, divider);

    // Let the clock to stabilize
    SDCardBase->sd_Delay(250000, SDCardBase);
}

int sdhost_transfer_block(int is_read, struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    int size = SDCardBase->sd_BlockSize / sizeof(ULONG);
    ULONG *buff = SDCardBase->sd_Buffer;
    ULONG transferred = 0;

    while(size)
    {
        int brust_size = HC_FIFO_BURST;
        int words;

        if (brust_size > size)
            brust_size = size;
        
        ULONG dbg = rd32(sc, HC_DEBUG);

        if (is_read)
            words = ((dbg >> 4) & 0x1f);
        else
            words = HC_FIFO_SIZE - ((dbg >> 4) & 0x1f);
        
        if (words < brust_size)
        {
            ULONG dbg = rd32(sc, HC_DEBUG);

            if (is_read)
                words = ((dbg >> 4) & 0x1f);
            else
                words = HC_FIFO_SIZE - ((dbg >> 4) & 0x1f);
            
            asm volatile("nop");

            continue;
        }
        else if (words > size)
            words = size;
        
        size -= words;

        while(words--) {
            if (is_read)
                *buff++ = rd32be(sc, HC_DATAPORT);
            else
                wr32be(sc, HC_DATAPORT, *buff++);
            
            transferred++;
        }

        if (rd32(sc, HC_HOSTSTATUS) & HC_HSTST_MASK_ERROR_ALL)
            break;
    }

    SDCardBase->sd_Buffer = buff;

    return transferred;
}

void sdhost_cmd_int(ULONG command, ULONG arg, ULONG timeout, struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;
    UWORD val2;
    ULONG blocks = SDCardBase->sd_BlocksToTransfer;

    if (SDCardBase->sd_Verbose > 1) {
        bug("[brcm-sdhc]   SDHOST_cmd_int(%02lx, %08lx, %ld)\n", 
            command >> 24,
            arg,
            timeout);
    }

    val2 = ((command >> 24) & HC_CMD_COMMAND_MASK) | HC_CMD_ENABLE;

    // Clear status
    wr32(sc, HC_HOSTSTATUS, HC_HSTST_RESET);

    switch(command & SD_CMD_RSPNS_TYPE_MASK)
    {
        case SD_CMD_RSPNS_TYPE_136:
            val2 |= HC_CMD_RESPONSE_LONG;
            break;
        case SD_CMD_RSPNS_TYPE_48B:
            val2 |= HC_CMD_BUSY;
            break;
        case SD_CMD_RSPNS_TYPE_NONE:
            val2 |= HC_CMD_RESPONSE_NONE;
            break;
    }

    if (command & SD_CMD_ISDATA) {
        if (command & SD_CMD_DAT_DIR_CH) {
            val2 |= HC_CMD_READ;
        }
        else {
            val2 |= HC_CMD_WRITE;
        }
    }

    wr32(sc, HC_BLOCKCOUNT, SDCardBase->sd_BlocksToTransfer);
    wr32(sc, HC_BLOCKSIZE, SDCardBase->sd_BlockSize);
    wr32(sc, HC_ARGUMENT, arg);
    wr32(sc, HC_COMMAND, val2);

    if (command & SD_CMD_ISDATA) {
        ULONG wait_flag = HC_HSTST_HAVEDATA;

        while(blocks--) {
            if (rd32(sc, HC_HOSTSTATUS) & HC_HSTST_MASK_ERROR_ALL)
                break;

            if (val2 & HC_CMD_READ) {
                TIMEOUT_WAIT(rd32(sc, HC_HOSTSTATUS) & wait_flag, timeout);
                wr32(sc, HC_HOSTSTATUS, rd32(sc, HC_HOSTSTATUS) & wait_flag);
                sdhost_transfer_block(command & SD_CMD_DAT_DIR_CH, SDCardBase); 
            }
            else {
                TIMEOUT_WAIT(rd32(sc, HC_HOSTSTATUS) & wait_flag, timeout);
                wr32(sc, HC_HOSTSTATUS, rd32(sc, HC_HOSTSTATUS) & wait_flag);
                sdhost_transfer_block(command & SD_CMD_DAT_DIR_CH, SDCardBase); 

                wait_flag = HC_HSTST_INT_BLOCK;
            }
        }

        if (!(val2 & HC_CMD_READ))
        {
            TIMEOUT_WAIT(rd32(sc, HC_HOSTSTATUS) & wait_flag, timeout);
            wr32(sc, HC_HOSTSTATUS, rd32(sc, HC_HOSTSTATUS) & wait_flag);
        }
    }

    TIMEOUT_WAIT(!(rd32(sc, HC_COMMAND) & HC_CMD_ENABLE) || (rd32(sc, HC_COMMAND) & HC_CMD_FAILED), timeout);

    if (rd32(sc, HC_COMMAND) & HC_CMD_FAILED)
    {
        SDCardBase->sd_LastCMDSuccess = 0;
        SDCardBase->sd_LastError = rd32(sc, HC_HOSTSTATUS) & HC_HSTST_MASK_ERROR_ALL;
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] SDHOST: error sending command\n");
        sdhost_dump_regs(SDCardBase);
    }
    else
    {
        SDCardBase->sd_LastCMDSuccess = 1;
        SDCardBase->sd_LastError = 0;

        switch(command & SD_CMD_RSPNS_TYPE_MASK)
        {
            case SD_CMD_RSPNS_TYPE_136:
                SDCardBase->sd_Res0 = rd32(sc, HC_RESPONSE_0);
                SDCardBase->sd_Res1 = rd32(sc, HC_RESPONSE_1);
                SDCardBase->sd_Res2 = rd32(sc, HC_RESPONSE_2);
                SDCardBase->sd_Res3 = rd32(sc, HC_RESPONSE_3);
                
                SDCardBase->sd_Res0 >>= 8;
                SDCardBase->sd_Res0 |= (SDCardBase->sd_Res1 << 24);

                SDCardBase->sd_Res1 >>= 8;
                SDCardBase->sd_Res1 |= (SDCardBase->sd_Res2 << 24);
                
                SDCardBase->sd_Res2 >>= 8;
                SDCardBase->sd_Res2 |= (SDCardBase->sd_Res3 << 24);
                
                SDCardBase->sd_Res3 >>= 8;

                break;
            case SD_CMD_RSPNS_TYPE_48:  // Fallthrough
            case SD_CMD_RSPNS_TYPE_48B:
                SDCardBase->sd_Res0 = rd32(sc, HC_RESPONSE_0);
                break;
        }

        if (command & SD_CMD_AUTO_CMD_EN_CMD12) {
            SDCardBase->sd_CMD_int(STOP_TRANSMISSION, 0, timeout, SDCardBase);
        }
    }

    SDCardBase->sd_InCommand = 0;
}

void sdhost_cmd(ULONG command, ULONG arg, ULONG timeout, struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;
    UWORD val2;

    if (SDCardBase->sd_Verbose > 1) {
        bug("[brcm-sdhc] SDHOST_cmd(%02lx, %08lx, %ld)\n", 
            command >> 24,
            arg,
            timeout);
    }

    if (SDCardBase->sd_InCommand) {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] Attempting to issue command when other command in progress!\n");
        return;
    }

    SDCardBase->sd_InCommand = 1;
    SDCardBase->sd_AppCommand = (command == APP_CMD);

    // Now run the appropriate commands by calling sd_issue_command_int()
    if(command & IS_APP_CMD)
    {
        command &= 0x7fffffff;

        SDCardBase->sd_LastCMD = APP_CMD;

        uint32_t rca = 0;
        if(SDCardBase->sd_CardRCA)
            rca = SDCardBase->sd_CardRCA << 16;
        SDCardBase->sd_CMD_int(APP_CMD, rca, timeout, SDCardBase);
        if(SDCardBase->sd_LastCMDSuccess)
        {
            SDCardBase->sd_LastCMD = command | IS_APP_CMD;
            SDCardBase->sd_CMD_int(command, arg, timeout, SDCardBase);
        }
    }
    else
    {
        SDCardBase->sd_LastCMD = command;
        SDCardBase->sd_CMD_int(command, arg, timeout, SDCardBase);
    }

    SDCardBase->sd_InCommand = 0;
}

int sdhost_card_init(struct SDCardBase *SDCardBase)
{
    APTR sc = SDCardBase->sd_SDHOST;
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] SDHOST Card Init\n");

    sdhost_reset(SDCardBase);

    sdhost_set_clock_rate(400000, SDCardBase);

    // Put the SD card to idle state before continuing.

    SDCardBase->sd_CMD(GO_IDLE_STATE, 0, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] SDHOST: no CMD0 response\n");
        return -1;
    }

    // Send CMD8 to the card
    // Voltage supplied = 0x1 = 2.7-3.6V (standard)
    // Check pattern = 10101010b (as per PLSS 4.3.13) = 0xAA

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] SDHOST: note a timeout error on the following command (CMD8) is normal "
            "and expected if the SD card version is less than 2.0\n");

    SDCardBase->sd_CMD(SEND_IF_COND, 0x1aa, 500000, SDCardBase);

    int v2_later = 0;
    if(TIMEOUT(SDCardBase))
        v2_later = 0;
    else if(SDCardBase->sd_LastError & HC_HSTST_TIMEOUT_CMD)
    {
        sdhost_reset(SDCardBase);
        v2_later = 0;
    }
    else if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] failure sending CMD8 (%08lx)\n", SDCardBase->sd_LastInterrupt);
        return -1;
    }
    else
    {
        if(((SDCardBase->sd_Res0) & 0xfff) != 0x1aa)
        {
            if (SDCardBase->sd_Verbose) {
                bug("[brcm-sdhc] unusable card\n");
                bug("[brcm-sdhc] CMD8 response %08lx\n", SDCardBase->sd_Res0);
            }
            return -1;
        }
        else
            v2_later = 1;
    }

    // Here we are supposed to check the response to CMD5 (HCSS 3.6)
    // It only returns if the card is a SDIO card
    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] SDHOST: note that a timeout error on the following command (CMD5) is "
           "normal and expected if the card is not a SDIO card.\n");
    SDCardBase->sd_CMD(IO_SET_OP_COND, 0, 10000, SDCardBase);
    if(!TIMEOUT(SDCardBase))
    {
        if(SDCardBase->sd_LastError & HC_HSTST_TIMEOUT_CMD)
        {
            sdhost_reset(SDCardBase);
        }
        else
        {
            if (SDCardBase->sd_Verbose)
                bug("[brcm-sdhc] SDIO card detected - not currently supported\n");
            return -1;
        }
    }

    // Call an inquiry ACMD41 (voltage window = 0) to get the OCR
    SDCardBase->sd_CMD(ACMD_41 | IS_APP_CMD, 0, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] inquiry ACMD41 failed\n");
        return -1;
    }

    // Call initialization ACMD41
    int card_is_busy = 1;
    while(card_is_busy)
    {
        uint32_t v2_flags = 0;
        if(v2_later)
        {
            // Set SDHC support
            v2_flags |= (1 << 30);
        }

        SDCardBase->sd_CMD(ACMD_41 | IS_APP_CMD, 0x00ff8000 | v2_flags, 500000, SDCardBase);

        if(FAIL(SDCardBase))
        {
            if (SDCardBase->sd_Verbose)
                bug("[brcm-sdhc] error issuing ACMD41\n");
            return -1;
        }

        if((SDCardBase->sd_Res0) & 0x80000000)
        {
            // Initialization is complete
            SDCardBase->sd_CardOCR = (SDCardBase->sd_Res0 >> 8) & 0xffff;
            SDCardBase->sd_CardSupportsSDHC = (SDCardBase->sd_Res0 >> 30) & 0x1;

            card_is_busy = 0;
        }
        else
        {
            if (SDCardBase->sd_Verbose)
                bug("[brcm-sdhc] card is still busy, retrying\n");
            SDCardBase->sd_Delay(100000, SDCardBase);
        }
    }

    if (SDCardBase->sd_Verbose)
    {
        bug("[brcm-sdhc] card identified: OCR: %04lx, SDHC support: %ld\n", SDCardBase->sd_CardOCR, SDCardBase->sd_CardSupportsSDHC);
    }

    // At this point, we know the card is definitely an SD card, so will definitely
    //  support SDR12 mode which runs at 25 MHz#
    sdhost_set_clock_rate(SD_CLOCK_NORMAL, SDCardBase);

    // A small wait before the voltage switch
    SDCardBase->sd_Delay(5000, SDCardBase);

    // Send CMD2 to get the cards CID
    SDCardBase->sd_CMD(ALL_SEND_CID, 0, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] error sending ALL_SEND_CID\n");
        return -1;
    }
    uint32_t card_cid_0 = SDCardBase->sd_Res0;
    uint32_t card_cid_1 = SDCardBase->sd_Res1;
    uint32_t card_cid_2 = SDCardBase->sd_Res2;
    uint32_t card_cid_3 = SDCardBase->sd_Res3;

    if (SDCardBase->sd_Verbose)
    {
        bug("[brcm-sdhc] card CID: %08lx%08lx%08lx%08lx\n", card_cid_3, card_cid_2, card_cid_1, card_cid_0);
    }
    
    SDCardBase->sd_CID[0] = card_cid_3;
    SDCardBase->sd_CID[1] = card_cid_2;
    SDCardBase->sd_CID[2] = card_cid_1;
    SDCardBase->sd_CID[3] = card_cid_0;

    // Send CMD3 to enter the data state
    SDCardBase->sd_CMD(SEND_RELATIVE_ADDR, 0, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] error sending SEND_RELATIVE_ADDR\n");
        return -1;
    }

    uint32_t cmd3_resp = SDCardBase->sd_Res0;

    SDCardBase->sd_CardRCA = (cmd3_resp >> 16) & 0xffff;
    uint32_t crc_error = (cmd3_resp >> 15) & 0x1;
    uint32_t illegal_cmd = (cmd3_resp >> 14) & 0x1;
    uint32_t error = (cmd3_resp >> 13) & 0x1;
    uint32_t status = (cmd3_resp >> 9) & 0xf;
    uint32_t ready = (cmd3_resp >> 8) & 0x1;

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] Res0: %08lx\n", cmd3_resp);

    if(crc_error)
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] CRC error\n");
        return -1;
    }

    if(illegal_cmd)
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] illegal command\n");
        return -1;
    }

    if(error)
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] generic error\n");
        return -1;
    }

    if(!ready)
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] not ready for data\n");
        return -1;
    }

    if (SDCardBase->sd_Verbose)
        bug("[brcm-sdhc] RCA: %04lx\n", SDCardBase->sd_CardRCA);

    // Now select the card (toggles it to transfer state)
    SDCardBase->sd_CMD(SELECT_CARD, SDCardBase->sd_CardRCA << 16, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] error sending CMD7\n");
        return -1;
    }

    uint32_t cmd7_resp = SDCardBase->sd_Res0;
    status = (cmd7_resp >> 9) & 0xf;

    if((status != 3) && (status != 4))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] invalid status (%ld)\n", status);
        return -1;
    }

    // If not an SDHC card, ensure BLOCKLEN is 512 bytes
    if(!SDCardBase->sd_CardSupportsSDHC)
    {
        SDCardBase->sd_CMD(SET_BLOCKLEN, 512, 500000, SDCardBase);
        if(FAIL(SDCardBase))
        {
            if (SDCardBase->sd_Verbose)
                bug("[brcm-sdhc] error sending SET_BLOCKLEN\n");
            return -1;
        }
    }
    
    // Get the cards SCR register
    SDCardBase->sd_Buffer = &SDCardBase->sd_SCR;
    SDCardBase->sd_BlocksToTransfer = 1;
    SDCardBase->sd_BlockSize = 8;

    SDCardBase->sd_CMD(SEND_SCR, 0, 500000, SDCardBase);
    SDCardBase->sd_BlockSize = 512;

    if(FAIL(SDCardBase))
    {
        if (SDCardBase->sd_Verbose)
            bug("[brcm-sdhc] error sending SEND_SCR\n");
        return -1;
    }

    // Determine card version
    // Note that the SCR is big-endian
    uint32_t scr0 = SDCardBase->sd_SCR.scr[0];
    SDCardBase->sd_SCR.sd_version = SD_VER_UNKNOWN;
    uint32_t sd_spec = (scr0 >> (56 - 32)) & 0xf;
    uint32_t sd_spec3 = (scr0 >> (47 - 32)) & 0x1;
    uint32_t sd_spec4 = (scr0 >> (42 - 32)) & 0x1;
    uint32_t sd_specx = (scr0 >> (38 - 32)) & 0xf;

    SDCardBase->sd_SCR.sd_bus_widths = (scr0 >> (48 - 32)) & 0xf;
    SDCardBase->sd_SCR.sd_commands = (scr0 & 0xf);

    if(sd_spec == 0)
        SDCardBase->sd_SCR.sd_version = SD_VER_1;
    else if(sd_spec == 1)
        SDCardBase->sd_SCR.sd_version = SD_VER_1_1;
    else if(sd_spec == 2)
    {
        if(sd_spec3 == 0)
            SDCardBase->sd_SCR.sd_version = SD_VER_2;
        else if(sd_spec3 == 1)
        {
            if (sd_specx == 0) {
                if(sd_spec4 == 0)
                    SDCardBase->sd_SCR.sd_version = SD_VER_3;
                else if(sd_spec4 == 1)
                    SDCardBase->sd_SCR.sd_version = SD_VER_4;
            }
            else {
                switch(sd_specx)
                {
                    case 1:
                        SDCardBase->sd_SCR.sd_version = SD_VER_5;
                        break;
                    case 2:
                        SDCardBase->sd_SCR.sd_version = SD_VER_6;
                        break;
                    case 3:
                        SDCardBase->sd_SCR.sd_version = SD_VER_7;
                        break;
                    case 4:
                        SDCardBase->sd_SCR.sd_version = SD_VER_8;
                        break;
                }
            }
        }
    }

    if (SDCardBase->sd_Verbose)
    {
        bug("[brcm-sdhc] SCR[0]: %08lx, SCR[1]: %08lx\n", 
            SDCardBase->sd_SCR.scr[0],
            SDCardBase->sd_SCR.scr[1]);
    }

    if (SDCardBase->sd_Verbose)
    {
        bug("[brcm-sdhc] SCR: version %ld, bus_widths %01lx\n", 
            SDCardBase->sd_SCR.sd_version,
            SDCardBase->sd_SCR.sd_bus_widths);

        if (SDCardBase->sd_SCR.sd_commands) {
            bug("[brcm-sdhc] supported commands:");
            if (SDCardBase->sd_SCR.sd_commands & SD_CMD20_SUPP) {
                bug(" CMD20");
            }
            if (SDCardBase->sd_SCR.sd_commands & SD_CMD23_SUPP) {
                bug(" CMD23");
            }
            if (SDCardBase->sd_SCR.sd_commands & SD_CMD48_49_SUPP) {
                bug(" CMD48/49");
            }
            if (SDCardBase->sd_SCR.sd_commands & SD_CMD58_59_SUPP) {
                bug(" CMD58/59");
            }
            bug("\n");
        }
    }
    
    if(SDCardBase->sd_SCR.sd_bus_widths & 0x4)
    {
        // Set 4-bit transfer mode (ACMD6)
        // See HCSS 3.4 for the algorithm
        bug("[brcm-sdhc] switching to 4-bit data mode\n");

        // Send ACMD6 to change the card's bit mode
        SDCardBase->sd_CMD(SET_BUS_WIDTH, 0x2, 500000, SDCardBase);
        if(FAIL(SDCardBase))
            bug("[brcm-sdhc] switch to 4-bit data mode failed\n");
        else
        {
            // Change bit mode for Host
            ULONG cfg = rd32(sc, HC_HOSTCONFIG);
            cfg |= HC_HSTCF_EXTBUS_4BIT;
            wr32(sc, HC_HOSTCONFIG, cfg);

            SDCardBase->sd_Delay(100000, SDCardBase);
        }
    }

    // Get the CMD6 status register
    SDCardBase->sd_Buffer = &SDCardBase->sd_StatusReg;
    SDCardBase->sd_BlocksToTransfer = 1;
    SDCardBase->sd_BlockSize = 64;

    SDCardBase->sd_CMD(SWITCH_FUNC, 0, 500000, SDCardBase);
    
    if (SDCardBase->sd_DisableHighSpeed == 0 && SDCardBase->sd_StatusReg[13] & 2)
    {
        bug("[brcm-sdhc] Card supports High Speed mode. Switching...\n");

        SDCardBase->sd_Buffer = &SDCardBase->sd_StatusReg;
        SDCardBase->sd_BlocksToTransfer = 1;
        SDCardBase->sd_BlockSize = 64;

        SDCardBase->sd_CMD(SWITCH_FUNC, 0x80fffff1, 500000, SDCardBase);

        SDCardBase->sd_Delay(10000, SDCardBase);

        if (SDCardBase->sd_Overclock != 0)
            sdhost_set_clock_rate(SDCardBase->sd_Overclock, SDCardBase);
        else
            sdhost_set_clock_rate(SD_CLOCK_HIGH, SDCardBase);
    }

    SDCardBase->sd_BlockSize = 512;

    bug("[brcm-sdhc] found a valid version %ld SD card\n", SDCardBase->sd_SCR.sd_version);

    return 0;
}

static int sdhost_ensure_data_mode(struct SDCardBase *SDCardBase)
{
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    if(SDCardBase->sd_CardRCA == 0)
    {
        // Try again to initialise the card
        int ret = SDCardBase->sd_CardInit(SDCardBase);
        if(ret != 0)
            return ret;
    }

    SDCardBase->sd_CMD(SEND_STATUS, SDCardBase->sd_CardRCA << 16, 500000, SDCardBase);
    if(FAIL(SDCardBase))
    {
        bug("[brcm-sdhc] ensure_data_mode() error sending CMD13\n");
        SDCardBase->sd_CardRCA = 0;
        return -1;
    }

    uint32_t status = SDCardBase->sd_Res0;
    uint32_t cur_state = (status >> 9) & 0xf;

    if(cur_state == 3)
    {
        // Currently in the stand-by state - select it
        SDCardBase->sd_CMD(SELECT_CARD, SDCardBase->sd_CardRCA << 16, 500000, SDCardBase);
        if(FAIL(SDCardBase))
        {
            bug("[brcm-sdhc] ensure_data_mode() no response from CMD17\n");
            SDCardBase->sd_CardRCA = 0;
            return -1;
        }
    }
    else if(cur_state == 5)
    {
        // In the data transfer state - cancel the transmission
        SDCardBase->sd_CMD(STOP_TRANSMISSION, 0, 500000, SDCardBase);
        if(FAIL(SDCardBase))
        {
            bug("[brcm-sdhc] ensure_data_mode() no response from CMD12\n");
            SDCardBase->sd_CardRCA = 0;
            return -1;
        }
    }
    else if(cur_state != 4)
    {
        // Not in the transfer state - re-initialise
        int ret = SDCardBase->sd_CardInit(SDCardBase);
        if(ret != 0)
            return ret;
    }

    // Check again that we're now in the correct mode
    if(cur_state != 4)
    {

        bug("[brcm-sdhc] ensure_data_mode() status was %ld, rechecking status: ", cur_state);
        SDCardBase->sd_CMD(SEND_STATUS, SDCardBase->sd_CardRCA << 16, 500000, SDCardBase);
        if(FAIL(SDCardBase))
        {
            bug("[brcm-sdhc] ensure_data_mode() no response from CMD13\n");
            SDCardBase->sd_CardRCA = 0;
            return -1;
        }
        status = SDCardBase->sd_Res0;
        cur_state = (status >> 9) & 0xf;

        bug("%ld\n", cur_state);

        if(cur_state != 4)
        {
            bug("[brcm-sdhc] unable to initialise SD card to "
                    "data mode (state %ld)\n", cur_state);
            SDCardBase->sd_CardRCA = 0;
            return -1;
        }
    }

    return 0;
}

int sdhost_do_data_command(int is_write, uint8_t *buf, uint32_t buf_size, uint32_t block_no, struct SDCardBase *SDCardBase)
{
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

    // PLSS table 4.20 - SDSC cards use byte addresses rather than block addresses
    if(!SDCardBase->sd_CardSupportsSDHC)
        block_no *= SDCardBase->sd_BlockSize;

    // This is as per HCSS 3.7.2.1
    if(buf_size < SDCardBase->sd_BlockSize)
    {
        bug("[brcm-sdhc] do_data_command() called with buffer size (%ld) less than "
            "block size (%ld)\n", buf_size, SDCardBase->sd_BlockSize);
        return -1;
    }

    SDCardBase->sd_BlocksToTransfer = buf_size / SDCardBase->sd_BlockSize;
    if(buf_size % SDCardBase->sd_BlockSize)
    {
        bug("[brcm-sdhc] do_data_command() called with buffer size (%ld) not an "
            "exact multiple of block size (%ld)\n", buf_size, SDCardBase->sd_BlockSize);
        return -1;
    }
    SDCardBase->sd_Buffer = buf;

    // Decide on the command to use
    int command;
    if(is_write)
    {
        SDCardBase->sd_CMD(SET_WR_BLK_ERASE_COUNT, SDCardBase->sd_BlocksToTransfer, 500000, SDCardBase);

        if(SDCardBase->sd_BlocksToTransfer > 1)
            command = WRITE_MULTIPLE_BLOCK;
        else
            command = WRITE_BLOCK;
    }
    else
    {
        if(SDCardBase->sd_BlocksToTransfer > 1)
            command = READ_MULTIPLE_BLOCK;
        else
            command = READ_SINGLE_BLOCK;
    }

    int retry_count = 0;
    const int max_retries = 5;

    for (retry_count = 0; retry_count < max_retries; retry_count++)
    {
        SDCardBase->sd_CMD(command, block_no, 5000000, SDCardBase);

        if(SUCCESS(SDCardBase))
        {
            break;
        }
        else
        {
            // In the data transfer state - cancel the transmission
            SDCardBase->sd_CMD(STOP_TRANSMISSION, 0, 500000, SDCardBase);
            if(FAIL(SDCardBase))
            {
                SDCardBase->sd_CardRCA = 0;
                return -1;
            }

            if (SDCardBase->sd_Verbose) {
                bug("[brcm-sdhc] error sending CMD%ld, error = %08lx.  ", command,
                    SDCardBase->sd_LastError);
            }
            retry_count++;
            
            if (SDCardBase->sd_Verbose) {
                if(retry_count < max_retries)
                    bug("Retrying...\n");
                else
                    bug("Giving up.\n");
            }
        }
    }

    if(retry_count == max_retries)
    {
        SDCardBase->sd_CardRCA = 0;
        return -1;
    }

    return 0;
}

int sdhost_read(uint8_t *buf, uint32_t buf_size, uint32_t block_no, struct SDCardBase *SDCardBase)
{
    // Check the status of the card
    if(sdhost_ensure_data_mode(SDCardBase) != 0)
        return -1;

    if(sdhost_do_data_command(0, buf, buf_size, block_no, SDCardBase) < 0)
        return -1;

    return buf_size;
}

int sdhost_write(uint8_t *buf, uint32_t buf_size, uint32_t block_no, struct SDCardBase *SDCardBase)
{
    struct ExecBase *SysBase = SDCardBase->sd_SysBase;

#if 0
    bug("WRITE NOT 100% SAFE YET!!! buf=%08lx, buf_size=%08lx, block_no=%08lx\n", (ULONG)buf, buf_size, block_no);
#endif

    // Check the status of the card
    if(sdhost_ensure_data_mode(SDCardBase) != 0)
        return -1;

    if(sdhost_do_data_command(1, buf, buf_size, block_no, SDCardBase) < 0)
        return -1;

    return buf_size;
}