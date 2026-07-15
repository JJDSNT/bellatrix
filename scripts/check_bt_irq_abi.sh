#!/usr/bin/env bash
set -euo pipefail

elf=${1:?usage: check_bt_irq_abi.sh Emu68.elf}
objdump=${AARCH64_OBJDUMP:-aarch64-linux-gnu-objdump}
nm=${AARCH64_NM:-aarch64-linux-gnu-nm}

vector_base_hex=$($nm -n "$elf" | awk \
    '$3 == "curr_el_sp0_sync" { value=$1 } END { print value }')
if [[ -z "$vector_base_hex" ]]; then
    echo "[BT-IRQ-ABI] missing vector base" >&2
    exit 1
fi
vector_base=$((16#$vector_base_hex))

vector_symbols=(
    curr_el_sp0_sync:0
    curr_el_sp0_irq:80
    curr_el_sp0_fiq:100
    curr_el_sp0_serror:180
    curr_el_spx_sync:200
    curr_el_spx_irq:280
    curr_el_spx_fiq:300
    curr_el_spx_serror:380
)

for item in "${vector_symbols[@]}"; do
    symbol=${item%%:*}
    expected_hex=${item#*:}
    actual_hex=$($nm -n "$elf" | awk -v wanted="$symbol" \
        '$3 == wanted { value=$1 } END { print value }')
    [[ -n "$actual_hex" ]] || {
        echo "[BT-IRQ-ABI] missing $symbol" >&2
        exit 1
    }
    actual=$((16#$actual_hex))
    expected=$((vector_base + 16#$expected_hex))
    if (( actual != expected )); then
        printf '[BT-IRQ-ABI] %s at +0x%x, expected +0x%s\n' \
            "$symbol" "$((actual - vector_base))" "$expected_hex" >&2
        exit 1
    fi
done

irq_dump=$($objdump -d --no-show-raw-insn --disassemble=curr_el_spx_irq "$elf")
trampoline_dump=$($objdump -d --no-show-raw-insn \
    --disassemble=bellatrix_spx_bt_irq "$elf")
fiq_dump=$($objdump -d --no-show-raw-insn --disassemble=curr_el_spx_fiq "$elf")

grep -q 'tbnz.*#25.*bellatrix_spx_bt_irq' <<<"$irq_dump"
grep -q 'strb.*#200' <<<"$irq_dump"
grep -q 'stp.*q30.*q31' <<<"$trampoline_dump"
grep -qi 'mrs.*fpcr' <<<"$trampoline_dump"
grep -qi 'mrs.*fpsr' <<<"$trampoline_dump"
grep -q 'bellatrix_physical_bt_irq_handler' <<<"$trampoline_dump"
grep -qi 'msr.*fpcr' <<<"$trampoline_dump"
grep -qi 'msr.*fpsr' <<<"$trampoline_dump"
grep -q 'eret' <<<"$trampoline_dump"

if grep -q 'bellatrix_physical' <<<"$fiq_dump"; then
    echo "[BT-IRQ-ABI] FIQ was redirected to a Bellatrix physical handler" >&2
    exit 1
fi
grep -q 'strb.*#200' <<<"$fiq_dump"

echo "[BT-IRQ-ABI] slots, Emu68 fallback, full trampoline and untouched FIQ verified"
