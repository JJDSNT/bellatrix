#include "cpu/emu68/emu68_machine_emit.h"

#include "cpu/emu68/emu68_machine_internal.h"

#include "A64.h"
#include "RegisterAllocator.h"

static uint32_t *emit_pointer(uint32_t *ptr, uint8_t reg, uintptr_t value)
{
    *ptr++ = mov64_immed_u16(reg, (uint16_t)value, 0);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 16), 1);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 32), 2);
    *ptr++ = movk64_immed_u16(reg, (uint16_t)(value >> 48), 3);
    return ptr;
}

static uint32_t *emit_native_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend)
{
    switch (width) {
    case 1u:
        *ptr++ = sign_extend ? ldrsb_offset(address_reg, value_reg, 0) :
                              ldrb_offset(address_reg, value_reg, 0);
        break;
    case 2u:
        *ptr++ = sign_extend ? ldrsh_offset(address_reg, value_reg, 0) :
                              ldrh_offset(address_reg, value_reg, 0);
        break;
    case 4u:
        *ptr++ = ldr_offset(address_reg, value_reg, 0);
        break;
    case 8u:
        *ptr++ = ldr64_offset(address_reg, value_reg, 0);
        break;
    default:
        *ptr++ = brk(0x68u);
        break;
    }
    return ptr;
}

static uint32_t *emit_native_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width)
{
    switch (width) {
    case 1u:
        *ptr++ = strb_offset(address_reg, value_reg, 0);
        break;
    case 2u:
        *ptr++ = strh_offset(address_reg, value_reg, 0);
        break;
    case 4u:
        *ptr++ = str_offset(address_reg, value_reg, 0);
        break;
    case 8u:
        *ptr++ = str64_offset(address_reg, value_reg, 0);
        break;
    default:
        *ptr++ = brk(0x68u);
        break;
    }
    return ptr;
}

static uint32_t *emit_access_prefix(uint32_t *ptr, uint8_t address_reg,
                                    uint8_t width, int write,
                                    uint8_t *page_reg, uint8_t *class_reg,
                                    uint32_t **slow_branch,
                                    uint32_t **boundary_branch)
{
    uintptr_t table = write ? (uintptr_t)emu68_machine_write_pages :
                              (uintptr_t)emu68_machine_read_pages;
    uint8_t limit_reg;

    *page_reg = RA_AllocARMRegister(&ptr);
    *class_reg = RA_AllocARMRegister(&ptr);
    limit_reg = RA_AllocARMRegister(&ptr);
    *ptr++ = lsr(*page_reg, address_reg, EMU68_MACHINE_PAGE_SHIFT);
    ptr = emit_pointer(ptr, *class_reg, table);
    *ptr++ = ldrb_regoffset(*class_reg, *class_reg, *page_reg, UXTW);
    *ptr++ = cmp_immed(*class_reg, EMU68_PAGE_DIRECT);
    *slow_branch = ptr++;
    *boundary_branch = NULL;

    if (width > 1u) {
        *ptr++ = ubfx(*page_reg, address_reg, 0,
                      EMU68_MACHINE_PAGE_SHIFT);
        *ptr++ = mov_immed_u16(limit_reg,
                              (uint16_t)(EMU68_MACHINE_PAGE_SIZE - width), 0);
        *ptr++ = cmp_reg(*page_reg, limit_reg, LSL, 0);
        *boundary_branch = ptr++;
        /* Both conditional branches are patched by the caller. The boundary
         * branch is immediately before the direct operation. */
    }
    RA_FreeARMRegister(&ptr, limit_reg);
    return ptr;
}

static uint32_t *emit_slow_call(uint32_t *ptr, uint8_t address_reg,
                                uint8_t value_reg, uint8_t width, int write,
                                int sign_extend, uint32_t metadata)
{
    uint8_t helper = RA_AllocARMRegister(&ptr);
    uint8_t outcome = RA_AllocARMRegister(&ptr);
    uint32_t *not_pending;
    uint32_t *complete;

    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_address);
    *ptr++ = str_offset(helper, address_reg, 0);
    ptr = emit_pointer(ptr, helper,
                       (uintptr_t)&emu68_machine_bridge_write_value);
    if (write)
        *ptr++ = str64_offset(helper, value_reg, 0);
    else {
        *ptr++ = mov64_immed_u16(outcome, 0u, 0);
        *ptr++ = str64_offset(helper, outcome, 0);
    }
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_metadata);
    *ptr++ = mov_immed_u16(outcome, (uint16_t)metadata, 0);
    *ptr++ = str_offset(helper, outcome, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_tu_return);
    *ptr++ = str64_offset(helper, 30, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)emu68_machine_native_bridge);
    *ptr++ = blr(helper);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_tu_return);
    *ptr++ = ldr64_offset(helper, 30, 0);
    ptr = emit_pointer(ptr, helper, (uintptr_t)&emu68_machine_bridge_outcome);
    *ptr++ = ldr64_offset(helper, outcome, 0);
    *ptr++ = cmp_immed(outcome, EMU68_BRIDGE_PENDING);
    not_pending = ptr++;
    *ptr++ = ret();
    *not_pending = b_cc(A64_CC_NE, ptr - not_pending);
    *ptr++ = cmp_immed(outcome, EMU68_BRIDGE_COMPLETE);
    complete = ptr++;
    *ptr++ = ret();
    *complete = b_cc(A64_CC_EQ, ptr - complete);
    if (!write) {
        ptr = emit_pointer(ptr, helper,
                           (uintptr_t)&emu68_machine_bridge_read_value);
        *ptr++ = width == 8u ? ldr64_offset(helper, value_reg, 0) :
                              ldr_offset(helper, value_reg, 0);
        if (sign_extend && width == 1u)
            *ptr++ = sxtb(value_reg, value_reg);
        else if (sign_extend && width == 2u)
            *ptr++ = sxth(value_reg, value_reg);
    }
    RA_FreeARMRegister(&ptr, outcome);
    RA_FreeARMRegister(&ptr, helper);
    return ptr;
}

uint32_t *emu68_machine_emit_load(uint32_t *ptr, uint8_t address_reg,
                                  uint8_t value_reg, uint8_t width,
                                  int sign_extend, uint32_t metadata)
{
    uint8_t page_reg;
    uint8_t class_reg;
    uint32_t *slow_branch;
    uint32_t *boundary_branch;
    uint32_t *done;
    uint32_t *slow;

    if (!emu68_machine_runtime_active())
        return emit_native_load(ptr, address_reg, value_reg, width,
                                sign_extend);
    ptr = emit_access_prefix(ptr, address_reg, width, 0, &page_reg,
                             &class_reg, &slow_branch, &boundary_branch);
    ptr = emit_native_load(ptr, address_reg, value_reg, width, sign_extend);
    done = ptr++;
    slow = ptr;
    ptr = emit_slow_call(ptr, address_reg, value_reg, width, 0, sign_extend,
                         metadata | width);
    *slow_branch = b_cc(A64_CC_NE, slow - slow_branch);
    if (boundary_branch)
        *boundary_branch = b_cc(A64_CC_HI, slow - boundary_branch);
    *done = b(ptr - done);
    RA_FreeARMRegister(&ptr, class_reg);
    RA_FreeARMRegister(&ptr, page_reg);
    return ptr;
}

uint32_t *emu68_machine_emit_store(uint32_t *ptr, uint8_t address_reg,
                                   uint8_t value_reg, uint8_t width,
                                   uint32_t metadata)
{
    uint8_t page_reg;
    uint8_t class_reg;
    uint32_t *slow_branch;
    uint32_t *boundary_branch;
    uint32_t *done;
    uint32_t *slow;

    if (!emu68_machine_runtime_active())
        return emit_native_store(ptr, address_reg, value_reg, width);
    ptr = emit_access_prefix(ptr, address_reg, width, 1, &page_reg,
                             &class_reg, &slow_branch, &boundary_branch);
    ptr = emit_native_store(ptr, address_reg, value_reg, width);
    done = ptr++;
    slow = ptr;
    ptr = emit_slow_call(ptr, address_reg, value_reg, width, 1, 0,
                         metadata | EMU68_BRIDGE_META_WRITE | width);
    *slow_branch = b_cc(A64_CC_NE, slow - slow_branch);
    if (boundary_branch)
        *boundary_branch = b_cc(A64_CC_HI, slow - boundary_branch);
    *done = b(ptr - done);
    RA_FreeARMRegister(&ptr, class_reg);
    RA_FreeARMRegister(&ptr, page_reg);
    return ptr;
}
