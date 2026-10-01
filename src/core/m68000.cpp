#include "core/m68000.hpp"

#include "core/log.hpp"
#include "core/sound_bus.hpp"

#include <bit>
#include <iostream>
#include <utility>

namespace model1 {

namespace {

constexpr uint32_t k_cycles_interrupt = 44;
constexpr uint32_t k_cycles_nop = 4;
constexpr uint32_t k_cycles_stop = 4;
constexpr uint32_t k_cycles_rte = 20;
constexpr uint32_t k_cycles_bra = 10;
constexpr uint32_t k_cycles_moveq = 4;

// Effective-address calculation times (MC68000 user's manual, table 8-1):
// [mode 0-7 / mode 7 sub-register] for byte/word and long operands.
constexpr uint32_t ea_cycles(uint32_t mode, uint32_t reg, bool is_long)
{
    switch (mode) {
    case 0: case 1: return 0;                       // Dn, An
    case 2: case 3: return is_long ? 8 : 4;         // (An), (An)+
    case 4: return is_long ? 10 : 6;                // -(An)
    case 5: return is_long ? 12 : 8;                // d16(An)
    case 6: return is_long ? 14 : 10;               // d8(An,Xn)
    default:
        switch (reg) {
        case 0: return is_long ? 12 : 8;            // abs.W
        case 1: return is_long ? 16 : 12;           // abs.L
        case 2: return is_long ? 12 : 8;            // d16(PC)
        case 3: return is_long ? 14 : 10;           // d8(PC,Xn)
        default: return is_long ? 8 : 4;            // #imm
        }
    }
}

} // namespace

M68000::M68000(SoundBus& bus)
    : m_bus(bus)
{
}

void M68000::reset()
{
    m_d.fill(0);
    m_a.fill(0);
    m_other_sp = 0;
    m_sr = k_reset_sr;
    m_a[7] = m_bus.read_long(0x000000); // initial SSP
    m_pc = m_bus.read_long(0x000004);   // initial PC
    m_irq_level = 0;
    m_stopped = false;
    m_halted = false;
    m_cycles = 0;
    m_instructions = 0;
    m_interrupts = 0;
    std::cerr << "[68000] Reset, SSP=" << Hex{m_a[7]} << " PC=" << Hex{m_pc} << '\n';
}

void M68000::set_sr(uint16_t value)
{
    const bool was_supervisor = supervisor();
    m_sr = value;
    if (was_supervisor != supervisor()) {
        std::swap(m_a[7], m_other_sp); // switch between SSP and USP
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

uint32_t M68000::execute_cycle()
{
    uint32_t cycles = 0;
    if (m_halted) {
        cycles = k_idle_cycles;
    } else if (m_irq_level == 7 || m_irq_level > ((m_sr & k_sr_mask) >> k_sr_mask_shift)) {
        take_interrupt(m_irq_level);
        cycles = k_cycles_interrupt;
    } else if (m_stopped) {
        cycles = k_idle_cycles;
    } else {
        m_instruction_pc = m_pc;
        if ((m_pc & 1) != 0) {
            halt("address error: instruction fetch from an odd address (exception not emulated)", 0, false);
            m_cycles += k_idle_cycles;
            return k_idle_cycles;
        }
        if (m_trace_remaining > 0) {
            --m_trace_remaining;
            std::cerr << "[68000 trace] " << Hex{m_pc} << ':';
            for (uint32_t i = 0; i < 8; i += 2) {
                const auto word = static_cast<uint32_t>((m_bus.peek_byte(m_pc + i) << 8) | m_bus.peek_byte(m_pc + i + 1));
                std::cerr << ' ' << Hex{word, 4};
            }
            std::cerr << '\n';
        }
        const uint16_t opcode = fetch_word();

        cycles = execute_instruction(opcode);
        if (!m_halted) {
            ++m_instructions;
        }
    }
    m_cycles += cycles;
    return cycles;
}

void M68000::take_interrupt(int level)
{
    m_stopped = false; // an accepted interrupt ends STOP
    const uint16_t old_sr = m_sr;
    uint16_t new_sr = static_cast<uint16_t>((m_sr & ~(k_sr_trace | k_sr_mask)) | k_sr_supervisor);
    new_sr = static_cast<uint16_t>(new_sr | (level << k_sr_mask_shift));
    set_sr(new_sr);
    push_long(m_pc);
    push_word(old_sr);
    const uint32_t vector = k_autovector_base + static_cast<uint32_t>(level);
    m_pc = m_bus.read_long(vector * 4);
    ++m_interrupts;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint16_t M68000::fetch_word()
{
    const uint16_t value = m_bus.read_word(m_pc);
    m_pc += 2;
    return value;
}

uint32_t M68000::fetch_long()
{
    const uint32_t value = m_bus.read_long(m_pc);
    m_pc += 4;
    return value;
}

void M68000::push_word(uint16_t value)
{
    m_a[7] -= 2;
    m_bus.write_word(m_a[7], value);
}

void M68000::push_long(uint32_t value)
{
    m_a[7] -= 4;
    m_bus.write_long(m_a[7], value);
}

uint16_t M68000::pop_word()
{
    const uint16_t value = m_bus.read_word(m_a[7]);
    m_a[7] += 2;
    return value;
}

uint32_t M68000::pop_long()
{
    const uint32_t value = m_bus.read_long(m_a[7]);
    m_a[7] += 4;
    return value;
}

void M68000::halt(const char* reason, uint16_t opcode, bool show_opcode)
{
    if (m_halted) {
        return;
    }
    m_halted = true;
    std::cerr << "[68000] CRITICAL: " << reason;
    if (show_opcode) {
        std::cerr << " " << Hex{opcode, 4};
    }
    std::cerr << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
    m_pc = m_instruction_pc;
}

// ---------------------------------------------------------------------------
// Effective addresses
// ---------------------------------------------------------------------------

M68000::Ea M68000::decode_ea(uint32_t mode, uint32_t reg, Size size)
{
    const uint32_t bytes = size_bytes(size);
    Ea ea;
    switch (mode) {
    case 0: ea.kind = Ea::Kind::DataReg; ea.reg = reg; return ea;
    case 1: ea.kind = Ea::Kind::AddrReg; ea.reg = reg; return ea;
    case 2: ea.kind = Ea::Kind::Memory; ea.address = m_a[reg]; return ea;
    case 3: {
        // (An)+: byte accesses through A7 step by 2 to keep SP even.
        const uint32_t step = (reg == 7 && bytes == 1) ? 2 : bytes;
        ea.kind = Ea::Kind::Memory;
        ea.address = m_a[reg];
        m_a[reg] += step;
        return ea;
    }
    case 4: {
        const uint32_t step = (reg == 7 && bytes == 1) ? 2 : bytes;
        m_a[reg] -= step;
        ea.kind = Ea::Kind::Memory;
        ea.address = m_a[reg];
        return ea;
    }
    case 5:
        ea.kind = Ea::Kind::Memory;
        ea.address = m_a[reg] + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fetch_word())));
        return ea;
    case 6:
        ea.kind = Ea::Kind::Memory;
        ea.address = indexed_address(m_a[reg]);
        return ea;
    default:
        break;
    }
    switch (reg) {
    case 0: // abs.W (sign-extended)
        ea.kind = Ea::Kind::Memory;
        ea.address = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fetch_word())));
        return ea;
    case 1: // abs.L
        ea.kind = Ea::Kind::Memory;
        ea.address = fetch_long();
        return ea;
    case 2: { // d16(PC): relative to the extension word's address
        const uint32_t base = m_pc;
        ea.kind = Ea::Kind::Memory;
        ea.address = base + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fetch_word())));
        return ea;
    }
    case 3: { // d8(PC,Xn)
        const uint32_t base = m_pc;
        ea.kind = Ea::Kind::Memory;
        ea.address = indexed_address(base);
        return ea;
    }
    case 4: // #imm: bytes and words take one extension word, longs two
        ea.kind = Ea::Kind::Immediate;
        ea.value = size == Size::Long ? fetch_long() : (size == Size::Byte ? (fetch_word() & 0xFFu) : fetch_word());
        return ea;
    default:
        return ea; // Invalid: reserved mode
    }
}

// Brief extension word: bit 15 = An/Dn, bits 14-12 index register,
// bit 11 = long index (else sign-extended word), bits 7-0 displacement.
uint32_t M68000::indexed_address(uint32_t base)
{
    const uint16_t extension = fetch_word();
    const uint32_t reg = (extension >> 12) & 7;
    uint32_t index = (extension & 0x8000) != 0 ? m_a[reg] : m_d[reg];
    if ((extension & 0x0800) == 0) {
        index = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(index)));
    }
    return base + index + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(extension & 0xFF)));
}

bool M68000::check_alignment(const Ea& ea, Size size)
{
    if (ea.kind == Ea::Kind::Memory && size != Size::Byte && (ea.address & 1) != 0) {
        halt("address error: word/long access at an odd address (exception not emulated)", 0, false);
        return false;
    }
    return true;
}

uint32_t M68000::read_ea(const Ea& ea, Size size)
{
    const uint32_t mask = size_mask(size);
    switch (ea.kind) {
    case Ea::Kind::DataReg:   return m_d[ea.reg] & mask;
    case Ea::Kind::AddrReg:   return m_a[ea.reg] & mask;
    case Ea::Kind::Immediate: return ea.value & mask;
    case Ea::Kind::Memory:
        if (!check_alignment(ea, size)) {
            return 0;
        }
        switch (size) {
        case Size::Byte: return m_bus.read_byte(ea.address);
        case Size::Word: return m_bus.read_word(ea.address);
        case Size::Long: return m_bus.read_long(ea.address);
        }
        break;
    case Ea::Kind::Invalid:
        break;
    }
    return 0;
}

void M68000::write_ea(const Ea& ea, Size size, uint32_t value)
{
    const uint32_t mask = size_mask(size);
    switch (ea.kind) {
    case Ea::Kind::DataReg:
        m_d[ea.reg] = (m_d[ea.reg] & ~mask) | (value & mask);
        return;
    case Ea::Kind::AddrReg:
        m_a[ea.reg] = value; // address registers are always written whole
        return;
    case Ea::Kind::Memory:
        if (!check_alignment(ea, size)) {
            return;
        }
        switch (size) {
        case Size::Byte: m_bus.write_byte(ea.address, static_cast<uint8_t>(value)); return;
        case Size::Word: m_bus.write_word(ea.address, static_cast<uint16_t>(value)); return;
        case Size::Long: m_bus.write_long(ea.address, value); return;
        }
        return;
    case Ea::Kind::Immediate:
    case Ea::Kind::Invalid:
        halt("invalid destination addressing mode", 0, false);
        return;
    }
}

void M68000::set_nz(uint32_t value, Size size)
{
    m_sr = static_cast<uint16_t>(m_sr & ~(k_sr_negative | k_sr_zero | k_sr_overflow | k_sr_carry));
    if ((value & size_mask(size)) == 0) {
        m_sr |= k_sr_zero;
    }
    if ((value & (1u << (size_bytes(size) * 8 - 1))) != 0) {
        m_sr |= k_sr_negative;
    }
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

// Condition codes 2-15 (0 = true and 1 = false/BSR are handled by callers).
bool M68000::condition_true(uint32_t condition) const
{
    const bool c = (m_sr & k_sr_carry) != 0;
    const bool v = (m_sr & k_sr_overflow) != 0;
    const bool z = (m_sr & k_sr_zero) != 0;
    const bool n = (m_sr & k_sr_negative) != 0;
    switch (condition) {
    case 0x2: return !c && !z;          // HI
    case 0x3: return c || z;            // LS
    case 0x4: return !c;                // CC
    case 0x5: return c;                 // CS
    case 0x6: return !z;                // NE
    case 0x7: return z;                 // EQ
    case 0x8: return !v;                // VC
    case 0x9: return v;                 // VS
    case 0xA: return !n;                // PL
    case 0xB: return n;                 // MI
    case 0xC: return n == v;            // GE
    case 0xD: return n != v;            // LT
    case 0xE: return !z && n == v;      // GT
    case 0xF: return z || n != v;       // LE
    default:  return condition == 0;
    }
}

uint32_t M68000::execute_instruction(uint16_t opcode)
{
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;

    if (opcode == 0x4E71) { // NOP
        return k_cycles_nop;
    }
    if (opcode == 0x4E72) { // STOP #imm
        const uint16_t new_sr = fetch_word();
        if (!supervisor()) {
            halt("privilege violation: STOP in user mode (exception not emulated)", opcode);
        } else {
            set_sr(new_sr);
            m_stopped = true;
        }
        return k_cycles_stop;
    }
    if (opcode == 0x4E73) { // RTE
        if (!supervisor()) {
            halt("privilege violation: RTE in user mode (exception not emulated)", opcode);
        } else {
            const uint16_t restored_sr = pop_word();
            m_pc = pop_long();
            set_sr(restored_sr);
        }
        return k_cycles_rte;
    }
    if ((opcode & 0xF000) == 0x6000) { // BRA / BSR / Bcc: relative to the address after the opcode
        const uint32_t base = m_pc;
        const uint32_t condition = (opcode >> 8) & 0xF;
        const bool word_displacement = (opcode & 0xFF) == 0;
        int32_t displacement = static_cast<int8_t>(opcode & 0xFF);
        if (word_displacement) {
            displacement = static_cast<int16_t>(fetch_word());
        }
        if (condition == 1) { // BSR: push the address of the next instruction
            push_long(m_pc);
            m_pc = base + static_cast<uint32_t>(displacement);
            return 18;
        }
        if (condition == 0 || condition_true(condition)) {
            m_pc = base + static_cast<uint32_t>(displacement);
            return k_cycles_bra;
        }
        return word_displacement ? 12 : 8;
    }
    if (opcode == 0x4E75) { // RTS
        m_pc = pop_long();
        return 16;
    }
    if ((opcode & 0xFF80) == 0x4E80) { // JSR (0x4E80) / JMP (0x4EC0) <ea>
        const bool jsr = (opcode & 0x40) == 0;
        const bool control_mode = ea_mode == 2 || ea_mode == 5 || ea_mode == 6 || (ea_mode == 7 && ea_reg <= 3);
        if (!control_mode) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const Ea target = decode_ea(ea_mode, ea_reg, Size::Long);
        if (jsr) {
            push_long(m_pc);
        }
        m_pc = target.address;
        // JMP: (An) 8, d16 10, index 14, abs.W 10, abs.L 12; JSR = JMP + 8.
        static constexpr uint32_t k_jmp_mode[7] = {0, 0, 8, 0, 0, 10, 14};
        static constexpr uint32_t k_jmp_mode7[4] = {10, 12, 10, 14};
        const uint32_t jmp_cycles = ea_mode == 7 ? k_jmp_mode7[ea_reg] : k_jmp_mode[ea_mode];
        return jsr ? jmp_cycles + 8 : jmp_cycles;
    }
    if ((opcode & 0xF100) == 0x7000) { // MOVEQ #imm8, Dn
        const auto value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFF)));
        m_d[(opcode >> 9) & 7] = value;
        set_nz(value, Size::Long);
        return k_cycles_moveq;
    }
    if ((opcode & 0xF1C0) == 0x41C0) { // LEA <ea>, An: load the effective address itself
        const bool control_mode = ea_mode == 2 || ea_mode == 5 || ea_mode == 6 || (ea_mode == 7 && ea_reg <= 3);
        if (!control_mode) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const Ea source = decode_ea(ea_mode, ea_reg, Size::Long);
        m_a[(opcode >> 9) & 7] = source.address;
        // (An) 4, d16 8, index 12, abs.W 8, abs.L 12, d16(PC) 8, d8(PC,Xn) 12.
        static constexpr uint32_t k_lea_mode[7] = {0, 0, 4, 0, 0, 8, 12};
        static constexpr uint32_t k_lea_mode7[4] = {8, 12, 8, 12};
        return ea_mode == 7 ? k_lea_mode7[ea_reg] : k_lea_mode[ea_mode];
    }
    if ((opcode & 0xF0F8) == 0x50C8) { // DBcc Dn, disp16
        const uint32_t base = m_pc; // displacement is relative to the extension word
        const auto displacement = static_cast<int16_t>(fetch_word());
        const uint32_t condition = (opcode >> 8) & 0xF;
        if (condition == 0 || (condition != 1 && condition_true(condition))) {
            return 12; // condition true: no decrement, fall through
        }
        uint32_t& dn = m_d[opcode & 7];
        const auto counter = static_cast<uint16_t>((dn & 0xFFFF) - 1);
        dn = (dn & 0xFFFF0000u) | counter;
        if (counter == 0xFFFF) {
            return 14; // counter expired: fall through
        }
        m_pc = base + static_cast<uint32_t>(static_cast<int32_t>(displacement));
        return 10;
    }
    if ((opcode & 0xF100) == 0x0000 && (opcode & 0x0E00) != 0x0800) { // ORI ANDI SUBI ADDI EORI CMPI
        return execute_immediate_alu(opcode);
    }
    if ((opcode & 0xF000) == 0x8000 || (opcode & 0xF000) == 0xC000) { // OR / AND and their group
        return execute_or_and(opcode);
    }
    if ((opcode & 0xF000) == 0xB000) { // CMP / CMPA / CMPM / EOR
        return execute_compare_eor(opcode);
    }
    if ((opcode & 0xF000) == 0xD000 || (opcode & 0xF000) == 0x9000) { // ADD / ADDA / ADDX, SUB / SUBA / SUBX
        return execute_add_sub(opcode);
    }
    if ((opcode & 0xFF00) == 0x4A00 && (opcode & 0xC0) != 0xC0) { // TST.B/W/L <ea>: N Z, V C cleared
        const Size size = (opcode & 0xC0) == 0x00 ? Size::Byte : ((opcode & 0xC0) == 0x40 ? Size::Word : Size::Long);
        // Data alterable modes only on the 68000 (no An, PC-relative or #imm).
        if (ea_mode == 1 || (ea_mode == 7 && ea_reg > 1)) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t value = read_ea(decode_ea(ea_mode, ea_reg, size), size);
        if (m_halted) {
            return k_idle_cycles;
        }
        set_nz(value, size);
        return 4 + ea_cycles(ea_mode, ea_reg, size == Size::Long);
    }
    if ((opcode & 0xFFF8) == 0x4840) { // SWAP Dn: exchange the 16-bit halves
        uint32_t& dn = m_d[opcode & 7];
        dn = (dn >> 16) | (dn << 16);
        set_nz(dn, Size::Long);
        return 4;
    }
    if ((opcode & 0xF000) == 0xE000) { // ASx / LSx / ROXx / ROx, register or memory
        return execute_shift_rotate(opcode);
    }
    if ((opcode & 0xF000) == 0x5000 && (opcode & 0xC0) != 0xC0) { // ADDQ / SUBQ #1-8, <ea>
        return execute_add_sub_quick(opcode);
    }
    if ((opcode & 0xFFC0) == 0x46C0) { // MOVE <ea>, SR (privileged): 12 + ea
        if (!supervisor()) {
            halt("privilege violation: MOVE to SR in user mode (exception not emulated)", opcode);
            return k_idle_cycles;
        }
        const Ea source = decode_ea(ea_mode, ea_reg, Size::Word);
        if (source.kind == Ea::Kind::Invalid || source.kind == Ea::Kind::AddrReg) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t value = read_ea(source, Size::Word);
        set_sr(static_cast<uint16_t>(value));
        return 12 + ea_cycles(ea_mode, ea_reg, false);
    }
    if ((opcode & 0xFF00) == 0x4200 && (opcode & 0xC0) != 0xC0) { // CLR.B/W/L <ea>
        const Size size = (opcode & 0xC0) == 0x00 ? Size::Byte : ((opcode & 0xC0) == 0x40 ? Size::Word : Size::Long);
        const Ea target = decode_ea(ea_mode, ea_reg, size);
        if (target.kind == Ea::Kind::Invalid || target.kind == Ea::Kind::AddrReg || target.kind == Ea::Kind::Immediate) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        write_ea(target, size, 0);
        m_sr = static_cast<uint16_t>((m_sr & ~(k_sr_negative | k_sr_overflow | k_sr_carry)) | k_sr_zero);
        if (target.kind == Ea::Kind::DataReg) {
            return size == Size::Long ? 6 : 4;
        }
        return (size == Size::Long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, size == Size::Long);
    }
    // BTST / BCHG / BCLR / BSET: static (0000 1000 tt ea, bit number in an
    // extension word) or dynamic (0000 rrr1 tt ea, bit number in Dr; mode 1
    // there is MOVEP). Z = the tested bit was 0. On a data register the bit
    // number is modulo 32 (long); on memory it is modulo 8 (byte).
    const bool static_bit_op = (opcode & 0xFF00) == 0x0800;
    const bool dynamic_bit_op = (opcode & 0xF100) == 0x0100 && ea_mode != 1;
    if (static_bit_op || dynamic_bit_op) {
        return execute_bit_operation(opcode, static_bit_op);
    }
    if ((opcode & 0xC000) == 0 && (opcode & 0x3000) != 0) { // MOVE / MOVEA
        const uint32_t size_bits = (opcode >> 12) & 3;
        const Size size = size_bits == 1 ? Size::Byte : (size_bits == 3 ? Size::Word : Size::Long);
        const uint32_t dest_reg = (opcode >> 9) & 7;
        const uint32_t dest_mode = (opcode >> 6) & 7;
        const bool is_long = size == Size::Long;

        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid || (size == Size::Byte && source.kind == Ea::Kind::AddrReg)) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t value = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        if (dest_mode == 1) { // MOVEA: word sources are sign-extended; flags unchanged
            if (size == Size::Byte) {
                halt("invalid MOVEA size", opcode);
                return k_idle_cycles;
            }
            m_a[dest_reg] = size == Size::Word ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)))
                                               : value;
            return 4 + ea_cycles(ea_mode, ea_reg, is_long);
        }
        const Ea dest = decode_ea(dest_mode, dest_reg, size);
        if (dest.kind == Ea::Kind::Invalid || dest.kind == Ea::Kind::Immediate || dest.kind == Ea::Kind::AddrReg
            || (dest_mode == 7 && dest_reg > 1)) {
            halt("invalid destination addressing mode", opcode);
            return k_idle_cycles;
        }
        write_ea(dest, size, value);
        set_nz(value, size);
        // Destination -(An) costs the same as (An) for MOVE.
        const uint32_t dest_cycles = dest_mode == 4 ? ea_cycles(2, 0, is_long) : ea_cycles(dest_mode, dest_reg, is_long);
        return 4 + ea_cycles(ea_mode, ea_reg, is_long) + dest_cycles;
    }

    halt("unimplemented opcode", opcode);
    return k_idle_cycles;
}

// dst + src or dst - src at `size`: sets N Z V C (and X = C when
// `update_x`; CMP leaves X alone) and returns the result.
uint32_t M68000::add_sub_flags(uint32_t dst, uint32_t src, bool subtract, Size size, bool update_x)
{
    const uint32_t mask = size_mask(size);
    const uint32_t sign = 1u << (size_bytes(size) * 8 - 1);
    const uint64_t d = dst & mask;
    const uint64_t s = src & mask;
    const uint64_t wide = subtract ? d - s : d + s;
    const uint32_t result = static_cast<uint32_t>(wide) & mask;
    const bool carry = (wide & ~static_cast<uint64_t>(mask)) != 0; // carry out, or borrow
    const auto d32 = static_cast<uint32_t>(d);
    const auto s32 = static_cast<uint32_t>(s);
    // Overflow: operands of the same sign (ADD) or of opposite signs (SUB)
    // giving a result whose sign differs from the destination's.
    const uint32_t overflow = subtract ? (d32 ^ s32) & (d32 ^ result) : ~(d32 ^ s32) & (d32 ^ result);

    set_nz(result, size);
    m_sr = static_cast<uint16_t>(m_sr | (carry ? k_sr_carry : 0) | ((overflow & sign) != 0 ? k_sr_overflow : 0));
    if (update_x) {
        m_sr = static_cast<uint16_t>((m_sr & ~k_sr_extend) | (carry ? k_sr_extend : 0));
    }
    return result;
}

// ADD / SUB family: 1101 (ADD) or 1001 (SUB) rrr ooo ea.
//   ooo 000-010  <ea> op Dn -> Dn (B/W/L)     4 + ea (B/W); 6 + ea (L), 8 for Dn/An/#imm
//   ooo 100-110  Dn op <ea> -> <ea> (memory)  8 + ea (B/W); 12 + ea (L)
//                with ea mode 0/1: ADDX / SUBX Dy,Dx or -(Ay),-(Ax): X is
//                the carry in, Z is only ever cleared. 4/8 (Dn), 18/30 (memory)
//   ooo 011/111  ADDA / SUBA.W/L <ea>, An: word source sign-extended, all
//                32 bits written, flags untouched. 8 + ea (W); 6 + ea (L), 8 for Dn/An/#imm
uint32_t M68000::execute_add_sub(uint16_t opcode)
{
    const bool subtract = (opcode & 0xF000) == 0x9000;
    const uint32_t reg = (opcode >> 9) & 7;
    const uint32_t opmode = (opcode >> 6) & 7;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;
    const bool register_or_immediate = ea_mode <= 1 || (ea_mode == 7 && ea_reg == 4);

    if ((opmode & 3) == 3) { // ADDA / SUBA
        const bool is_long = opmode == 7;
        const Size size = is_long ? Size::Long : Size::Word;
        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        uint32_t value = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        if (!is_long) {
            value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
        }
        m_a[reg] = subtract ? m_a[reg] - value : m_a[reg] + value;
        if (is_long) {
            return (register_or_immediate ? 8 : 6) + ea_cycles(ea_mode, ea_reg, true);
        }
        return 8 + ea_cycles(ea_mode, ea_reg, false);
    }

    const Size size = (opmode & 3) == 0 ? Size::Byte : ((opmode & 3) == 1 ? Size::Word : Size::Long);
    const bool is_long = size == Size::Long;
    const bool to_register = (opmode & 4) == 0;

    if (!to_register && ea_mode <= 1) { // ADDX / SUBX
        const bool memory = ea_mode == 1;
        Ea source;
        Ea dest;
        if (memory) {
            source = decode_ea(4, ea_reg, size); // -(Ay) first, then -(Ax)
            dest = decode_ea(4, reg, size);
        } else {
            source = decode_ea(0, ea_reg, size);
            dest = decode_ea(0, reg, size);
        }
        const uint32_t src = read_ea(source, size);
        const uint32_t dst = read_ea(dest, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        const uint32_t mask = size_mask(size);
        const uint32_t sign = 1u << (size_bytes(size) * 8 - 1);
        const uint64_t x = (m_sr & k_sr_extend) != 0 ? 1 : 0;
        const uint64_t wide = subtract ? uint64_t{dst} - src - x : uint64_t{dst} + src + x;
        const uint32_t result = static_cast<uint32_t>(wide) & mask;
        write_ea(dest, size, result);

        const bool carry = (wide & ~static_cast<uint64_t>(mask)) != 0;
        const uint32_t overflow = subtract ? (dst ^ src) & (dst ^ result) : ~(dst ^ src) & (dst ^ result);
        const bool keep_zero = (m_sr & k_sr_zero) != 0 && result == 0; // Z cleared if non-zero, else unchanged
        m_sr = static_cast<uint16_t>(m_sr & ~(k_sr_extend | k_sr_negative | k_sr_zero | k_sr_overflow | k_sr_carry));
        m_sr = static_cast<uint16_t>(m_sr | (carry ? (k_sr_carry | k_sr_extend) : 0)
                                     | ((result & sign) != 0 ? k_sr_negative : 0) | (keep_zero ? k_sr_zero : 0)
                                     | ((overflow & sign) != 0 ? k_sr_overflow : 0));
        if (memory) {
            return is_long ? 30 : 18;
        }
        return is_long ? 8 : 4;
    }

    if (to_register) {
        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid || (size == Size::Byte && source.kind == Ea::Kind::AddrReg)) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t src = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        Ea dest;
        dest.kind = Ea::Kind::DataReg;
        dest.reg = reg;
        write_ea(dest, size, add_sub_flags(m_d[reg], src, subtract, size, true));
        if (is_long) {
            return (register_or_immediate ? 8 : 6) + ea_cycles(ea_mode, ea_reg, true);
        }
        return 4 + ea_cycles(ea_mode, ea_reg, false);
    }

    // Dn op <ea> -> <ea>: memory-alterable destinations only.
    if (ea_mode == 7 && ea_reg > 1) {
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }
    const Ea dest = decode_ea(ea_mode, ea_reg, size);
    const uint32_t dst = read_ea(dest, size);
    if (m_halted) {
        return k_idle_cycles;
    }
    write_ea(dest, size, add_sub_flags(dst, m_d[reg], subtract, size, true));
    return (is_long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, is_long);
}

// OR (1000) / AND (1100) rrr ooo ea, and the instructions sharing them:
//   ooo 000-010  OR / AND <ea>, Dn         4 + ea (B/W), 6 + ea (L; 8 for Dn / #imm)
//   ooo 100-110  OR / AND Dn, <ea> (memory) 8 + ea (B/W), 12 + ea (L)
//                with ea mode 0 / 1 and size B: SBCD / ABCD Dy,Dx or -(Ay),-(Ax)
//                (6 / 18); AND with opmode 101 / 110 and mode 0 / 1: EXG (6)
//   ooo 011 / 111  OR: DIVU / DIVS <ea>, Dn; AND: MULU / MULS <ea>, Dn
// OR / AND: N Z, V C cleared, X kept. MULU / MULS: 16 x 16 -> 32, N Z, V C
// cleared; 38 + 2n + ea (n: ones in the source for MULU, 01 / 10 bit pairs
// for MULS). DIVU / DIVS: 32 / 16, quotient in the low word, remainder in
// the high word; on overflow V is set and Dn is unchanged; charged the
// manual's worst case, 140 / 158 + ea (the exact time depends on the
// operands). Division by zero traps on the 68000: not emulated, halts.
// VR's sound program at 0xD50 uses OR.B D0, D0.
uint32_t M68000::execute_or_and(uint16_t opcode)
{
    const bool is_and = (opcode & 0xF000) == 0xC000;
    const uint32_t reg = (opcode >> 9) & 7;
    const uint32_t opmode = (opcode >> 6) & 7;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;

    if (is_and && ea_mode <= 1 && (opmode == 5 || opmode == 6)) { // EXG
        if (opmode == 5) {
            uint32_t& a = ea_mode == 0 ? m_d[reg] : m_a[reg];
            uint32_t& b = ea_mode == 0 ? m_d[ea_reg] : m_a[ea_reg];
            std::swap(a, b);
        } else if (ea_mode == 1) {
            std::swap(m_d[reg], m_a[ea_reg]);
        } else {
            halt("unimplemented opcode", opcode);
            return k_idle_cycles;
        }
        return 6;
    }

    if (opmode == 4 && ea_mode <= 1) { // SBCD (OR group) / ABCD (AND group)
        Ea source;
        Ea dest;
        if (ea_mode == 1) {
            source = decode_ea(4, ea_reg, Size::Byte);
            dest = decode_ea(4, reg, Size::Byte);
        } else {
            source = decode_ea(0, ea_reg, Size::Byte);
            dest = decode_ea(0, reg, Size::Byte);
        }
        const uint32_t src = read_ea(source, Size::Byte);
        const uint32_t dst = read_ea(dest, Size::Byte);
        const uint32_t x = (m_sr & k_sr_extend) != 0 ? 1 : 0;
        int32_t result = 0;
        bool carry = false;
        if (is_and) { // ABCD: dst + src + X in BCD
            int32_t low = static_cast<int32_t>((dst & 0x0F) + (src & 0x0F) + x);
            int32_t high = static_cast<int32_t>((dst & 0xF0) + (src & 0xF0));
            if (low > 9) {
                low += 6;
            }
            result = high + low;
            if (result > 0x99) {
                result += 0x60;
                carry = true;
            }
        } else { // SBCD: dst - src - X in BCD
            int32_t low = static_cast<int32_t>(dst & 0x0F) - static_cast<int32_t>(src & 0x0F) - static_cast<int32_t>(x);
            int32_t high = static_cast<int32_t>(dst & 0xF0) - static_cast<int32_t>(src & 0xF0);
            if (low < 0) {
                low -= 6;
            }
            result = high + low;
            if (result < 0) {
                result -= 0x60;
                carry = true;
            }
        }
        const auto value = static_cast<uint32_t>(result) & 0xFF;
        write_ea(dest, Size::Byte, value);
        const bool keep_zero = (m_sr & k_sr_zero) != 0 && value == 0; // Z only cleared
        m_sr = static_cast<uint16_t>(m_sr & ~(k_sr_extend | k_sr_negative | k_sr_zero | k_sr_carry));
        m_sr = static_cast<uint16_t>(m_sr | (carry ? (k_sr_carry | k_sr_extend) : 0) | (keep_zero ? k_sr_zero : 0)
                                     | ((value & 0x80) != 0 ? k_sr_negative : 0)); // N, V undefined
        return ea_mode == 1 ? 18 : 6;
    }

    if ((opmode & 3) == 3) { // DIVU / DIVS (OR group), MULU / MULS (AND group)
        const bool is_signed = opmode == 7;
        const Ea source = decode_ea(ea_mode, ea_reg, Size::Word);
        if (source.kind == Ea::Kind::Invalid || source.kind == Ea::Kind::AddrReg) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t src = read_ea(source, Size::Word);
        if (m_halted) {
            return k_idle_cycles;
        }
        const uint32_t ea_time = ea_cycles(ea_mode, ea_reg, false);
        m_sr = static_cast<uint16_t>(m_sr & ~(k_sr_overflow | k_sr_carry));
        if (is_and) {
            uint32_t product = 0;
            uint32_t n = 0;
            if (is_signed) {
                product = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(m_d[reg]))
                                                * static_cast<int32_t>(static_cast<int16_t>(src)));
                const uint32_t pairs = (src << 1) ^ src; // 01 / 10 transitions, bit -1 = 0
                n = static_cast<uint32_t>(std::popcount(pairs & 0xFFFFu));
            } else {
                product = (m_d[reg] & 0xFFFF) * src;
                n = static_cast<uint32_t>(std::popcount(src));
            }
            m_d[reg] = product;
            set_nz(product, Size::Long);
            return 38 + 2 * n + ea_time;
        }
        if (src == 0) {
            halt("division by zero (trap not emulated)", opcode);
            return k_idle_cycles;
        }
        const uint32_t dividend = m_d[reg];
        if (is_signed) {
            const int64_t quotient = static_cast<int64_t>(static_cast<int32_t>(dividend)) / static_cast<int16_t>(src);
            const int64_t remainder = static_cast<int64_t>(static_cast<int32_t>(dividend)) % static_cast<int16_t>(src);
            if (quotient < -32768 || quotient > 32767) {
                m_sr |= k_sr_overflow;
            } else {
                m_d[reg] = (static_cast<uint32_t>(remainder) << 16) | (static_cast<uint32_t>(quotient) & 0xFFFF);
                set_nz(static_cast<uint32_t>(quotient), Size::Word);
            }
            return 158 + ea_time;
        }
        const uint32_t quotient = dividend / src;
        const uint32_t remainder = dividend % src;
        if (quotient > 0xFFFF) {
            m_sr |= k_sr_overflow;
        } else {
            m_d[reg] = (remainder << 16) | quotient;
            set_nz(quotient, Size::Word);
        }
        return 140 + ea_time;
    }

    const Size size = (opmode & 3) == 0 ? Size::Byte : ((opmode & 3) == 1 ? Size::Word : Size::Long);
    const bool is_long = size == Size::Long;
    auto apply = [is_and](uint32_t a, uint32_t b) { return is_and ? (a & b) : (a | b); };

    if ((opmode & 4) == 0) { // <ea>, Dn
        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid || source.kind == Ea::Kind::AddrReg) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t value = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        Ea dest;
        dest.kind = Ea::Kind::DataReg;
        dest.reg = reg;
        const uint32_t result = apply(m_d[reg], value) & size_mask(size);
        write_ea(dest, size, result);
        set_nz(result, size);
        const bool register_or_immediate = ea_mode == 0 || (ea_mode == 7 && ea_reg == 4);
        if (is_long) {
            return (register_or_immediate ? 8 : 6) + ea_cycles(ea_mode, ea_reg, true);
        }
        return 4 + ea_cycles(ea_mode, ea_reg, false);
    }

    // Dn, <ea>: memory-alterable destinations.
    if (ea_mode <= 1 || (ea_mode == 7 && ea_reg > 1)) {
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }
    const Ea dest = decode_ea(ea_mode, ea_reg, size);
    const uint32_t value = read_ea(dest, size);
    if (m_halted) {
        return k_idle_cycles;
    }
    const uint32_t result = apply(value, m_d[reg]) & size_mask(size);
    write_ea(dest, size, result);
    set_nz(result, size);
    return (is_long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, is_long);
}

// CMP / CMPA / CMPM / EOR: 1011 rrr ooo ea.
//   ooo 000-010  CMP <ea>, Dn: N Z V C from Dn - <ea>, X kept.   4 + ea (B/W), 6 + ea (L)
//   ooo 011/111  CMPA.W/L <ea>, An: 32-bit An - <ea> (a word source is
//                sign-extended), N Z V C, X kept.                 6 + ea
//   ooo 100-110  EOR Dn, <ea> (data alterable): N Z, V C cleared. Dn 4/8,
//                memory 8/12 + ea; with ea mode 1: CMPM (Ay)+, (Ax)+ 12/20
// VR's sound program, UART handler at 0x1B6: CMPA.L #$F01300, A6 wraps
// its 256-byte command queue.
uint32_t M68000::execute_compare_eor(uint16_t opcode)
{
    const uint32_t reg = (opcode >> 9) & 7;
    const uint32_t opmode = (opcode >> 6) & 7;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;

    if ((opmode & 3) == 3) { // CMPA
        const bool is_long = opmode == 7;
        const Size size = is_long ? Size::Long : Size::Word;
        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        uint32_t value = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        if (!is_long) {
            value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
        }
        add_sub_flags(m_a[reg], value, true, Size::Long, false); // X kept
        return 6 + ea_cycles(ea_mode, ea_reg, is_long);
    }

    const Size size = (opmode & 3) == 0 ? Size::Byte : ((opmode & 3) == 1 ? Size::Word : Size::Long);
    const bool is_long = size == Size::Long;

    if ((opmode & 4) == 0) { // CMP <ea>, Dn
        const Ea source = decode_ea(ea_mode, ea_reg, size);
        if (source.kind == Ea::Kind::Invalid || (size == Size::Byte && source.kind == Ea::Kind::AddrReg)) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        const uint32_t value = read_ea(source, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        add_sub_flags(m_d[reg], value, true, size, false);
        return (is_long ? 6 : 4) + ea_cycles(ea_mode, ea_reg, is_long);
    }

    if (ea_mode == 1) { // CMPM (Ay)+, (Ax)+
        const Ea source = decode_ea(3, ea_reg, size);
        const Ea dest = decode_ea(3, reg, size);
        const uint32_t src = read_ea(source, size);
        const uint32_t dst = read_ea(dest, size);
        if (m_halted) {
            return k_idle_cycles;
        }
        add_sub_flags(dst, src, true, size, false);
        return is_long ? 20 : 12;
    }

    // EOR Dn, <ea>: data-alterable destinations.
    if (ea_mode == 7 && ea_reg > 1) {
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }
    const Ea dest = decode_ea(ea_mode, ea_reg, size);
    const uint32_t value = read_ea(dest, size);
    if (m_halted) {
        return k_idle_cycles;
    }
    const uint32_t result = (value ^ m_d[reg]) & size_mask(size);
    write_ea(dest, size, result);
    set_nz(result, size);
    if (dest.kind == Ea::Kind::DataReg) {
        return is_long ? 8 : 4;
    }
    return (is_long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, is_long);
}

// ORI / ANDI / SUBI / ADDI / EORI / CMPI #imm, <ea>: 0000 ooo0 ss ea, the
// immediate (one word for byte/word, two for long) before the EA's
// extension words. Data-alterable destinations only (CMPI: data modes).
// ORI / ANDI / EORI with ea = #imm (0x3C byte / 0x7C word) target CCR / SR
// instead; the SR forms are privileged. 20 cycles for those.
// Logic: N Z, V C cleared, X kept. ADDI / SUBI: X N Z V C. CMPI: N Z V C.
uint32_t M68000::execute_immediate_alu(uint16_t opcode)
{
    enum : uint32_t { k_ori = 0, k_andi = 1, k_subi = 2, k_addi = 3, k_eori = 5, k_cmpi = 6 };
    const uint32_t op = (opcode >> 9) & 7;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;
    const uint32_t size_bits = (opcode >> 6) & 3;
    const bool logic = op == k_ori || op == k_andi || op == k_eori;
    if (op == 4 || op == 7 || size_bits == 3) {
        halt("unimplemented opcode", opcode);
        return k_idle_cycles;
    }
    const Size size = size_bits == 0 ? Size::Byte : (size_bits == 1 ? Size::Word : Size::Long);
    const uint32_t imm = size == Size::Long ? fetch_long() : (size == Size::Byte ? (fetch_word() & 0xFFu) : fetch_word());

    auto apply_logic = [op](uint32_t a, uint32_t b) {
        return op == k_ori ? (a | b) : (op == k_andi ? (a & b) : (a ^ b));
    };
    if (logic && ea_mode == 7 && ea_reg == 4 && size != Size::Long) { // to CCR / SR
        if (size == Size::Word && !supervisor()) {
            halt("privilege violation: logic immediate to SR in user mode (exception not emulated)", opcode);
            return k_idle_cycles;
        }
        if (size == Size::Byte) {
            const uint32_t ccr = apply_logic(m_sr & 0x1Fu, imm) & 0x1Fu;
            m_sr = static_cast<uint16_t>((m_sr & 0xFF00u) | ccr);
        } else {
            set_sr(static_cast<uint16_t>(apply_logic(m_sr, imm)));
        }
        return 20;
    }

    // Data-alterable destinations; CMPI may also read PC-relative operands.
    const bool pc_relative = ea_mode == 7 && (ea_reg == 2 || ea_reg == 3);
    if (ea_mode == 1 || (ea_mode == 7 && ea_reg > 3) || (pc_relative && op != k_cmpi)) {
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }
    const Ea target = decode_ea(ea_mode, ea_reg, size);
    const uint32_t value = read_ea(target, size);
    if (m_halted) {
        return k_idle_cycles;
    }

    const bool is_long = size == Size::Long;
    const bool on_register = target.kind == Ea::Kind::DataReg;
    if (op == k_cmpi) {
        add_sub_flags(value, imm, true, size, false);
        if (on_register) {
            return is_long ? 14 : 8;
        }
        return (is_long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, is_long);
    }
    uint32_t result = 0;
    if (logic) {
        result = apply_logic(value, imm) & size_mask(size);
        set_nz(result, size); // also clears V and C
    } else {
        result = add_sub_flags(value, imm, op == k_subi, size, true);
    }
    write_ea(target, size, result);
    if (on_register) {
        return is_long ? (op == k_andi ? 14 : 16) : 8;
    }
    return (is_long ? 20 : 12) + ea_cycles(ea_mode, ea_reg, is_long);
}

// Shifts and rotates: ASL/ASR (00), LSL/LSR (01), ROXL/ROXR (10), ROL/ROR (11).
//   register: 1110 ccc d ss i tt rrr  count = ccc (1-8, 0 means 8) when i = 0,
//                                     else D[ccc] modulo 64
//   memory:   1110 0tt d 11 ea        one bit, word size
// d = 1 shifts left. C (and X, except for ROL/ROR) gets the last bit shifted
// out. ASL sets V if the sign bit changes at any point; the others clear V.
// A count of 0 clears C (ROXL/ROXR: C = X) and leaves X unchanged.
// Timing: Dn 6 + 2n (byte/word), 8 + 2n (long); memory 8 + ea.
uint32_t M68000::execute_shift_rotate(uint16_t opcode)
{
    const bool memory_form = (opcode & 0xC0) == 0xC0;
    const bool left = (opcode & 0x0100) != 0;
    const uint32_t type = memory_form ? (opcode >> 9) & 3 : (opcode >> 3) & 3;
    enum : uint32_t { k_arithmetic = 0, k_logical = 1, k_rotate_extend = 2, k_rotate = 3 };

    Size size = Size::Word;
    uint32_t count = 1;
    Ea target;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;
    if (memory_form) {
        // Memory alterable modes only: (An) to abs.L.
        if ((opcode & 0x0800) != 0 || ea_mode < 2 || (ea_mode == 7 && ea_reg > 1)) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        target = decode_ea(ea_mode, ea_reg, size);
    } else {
        const uint32_t size_bits = (opcode >> 6) & 3;
        size = size_bits == 0 ? Size::Byte : (size_bits == 1 ? Size::Word : Size::Long);
        const uint32_t field = (opcode >> 9) & 7;
        count = (opcode & 0x20) != 0 ? m_d[field] & 63 : (field == 0 ? 8 : field);
        target.kind = Ea::Kind::DataReg;
        target.reg = opcode & 7;
    }

    const uint32_t mask = size_mask(size);
    const uint32_t msb = 1u << (size_bytes(size) * 8 - 1);
    uint32_t value = read_ea(target, size);
    if (m_halted) {
        return k_idle_cycles;
    }
    bool x = (m_sr & k_sr_extend) != 0;
    bool carry = type == k_rotate_extend ? x : false; // result for a count of 0
    bool overflow = false;
    for (uint32_t i = 0; i < count; ++i) {
        const bool out = left ? (value & msb) != 0 : (value & 1) != 0;
        if (left) {
            const uint32_t in_bit = type == k_rotate_extend ? (x ? 1u : 0u) : (type == k_rotate && out ? 1u : 0u);
            value = ((value << 1) | in_bit) & mask;
            if (type == k_arithmetic && ((value & msb) != 0) != out) {
                overflow = true; // the sign bit changed
            }
        } else {
            uint32_t in_bit = 0;
            switch (type) {
            case k_arithmetic:    in_bit = value & msb; break; // sign fill
            case k_rotate_extend: in_bit = x ? msb : 0; break;
            case k_rotate:        in_bit = out ? msb : 0; break;
            default:              break;
            }
            value = (value >> 1) | in_bit;
        }
        carry = out;
        if (type != k_rotate) {
            x = out;
        }
    }
    write_ea(target, size, value);

    set_nz(value, size);
    m_sr = static_cast<uint16_t>(m_sr | (carry ? k_sr_carry : 0) | (overflow ? k_sr_overflow : 0));
    m_sr = static_cast<uint16_t>((m_sr & ~k_sr_extend) | (x ? k_sr_extend : 0));

    if (memory_form) {
        return 8 + ea_cycles(ea_mode, ea_reg, false);
    }
    return (size == Size::Long ? 8 : 6) + 2 * count;
}

// ADDQ / SUBQ #data, <ea>: 0101 ddd s ss ea, data 1-8 (0 encodes 8), s = 1
// for SUBQ. Sets X N Z V C like ADD / SUB. On an address register the whole
// 32 bits change, flags are untouched, and byte size is not allowed.
uint32_t M68000::execute_add_sub_quick(uint16_t opcode)
{
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;
    const bool subtract = (opcode & 0x0100) != 0;
    const uint32_t data = ((opcode >> 9) & 7) == 0 ? 8u : (opcode >> 9) & 7;
    const uint32_t size_bits = (opcode >> 6) & 3;
    const Size size = size_bits == 0 ? Size::Byte : (size_bits == 1 ? Size::Word : Size::Long);

    if (ea_mode == 1) {
        if (size == Size::Byte) {
            halt("invalid addressing mode", opcode);
            return k_idle_cycles;
        }
        m_a[ea_reg] = subtract ? m_a[ea_reg] - data : m_a[ea_reg] + data;
        return 8;
    }
    if (ea_mode == 7 && ea_reg > 1) { // only abs.W / abs.L are alterable in mode 7
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }

    const Ea target = decode_ea(ea_mode, ea_reg, size);
    const uint32_t value = read_ea(target, size);
    if (m_halted) {
        return k_idle_cycles;
    }
    write_ea(target, size, add_sub_flags(value, data, subtract, size, true));

    if (target.kind == Ea::Kind::DataReg) {
        return size == Size::Long ? 8 : 4;
    }
    return (size == Size::Long ? 12 : 8) + ea_cycles(ea_mode, ea_reg, size == Size::Long);
}

uint32_t M68000::execute_bit_operation(uint16_t opcode, bool static_form)
{
    enum : uint32_t { k_btst = 0, k_bchg = 1, k_bclr = 2, k_bset = 3 };
    const uint32_t kind = (opcode >> 6) & 3;
    const uint32_t ea_mode = (opcode >> 3) & 7;
    const uint32_t ea_reg = opcode & 7;

    // The bit-number extension word comes before the EA's own extension words.
    const uint32_t bit_number = static_form ? (fetch_word() & 0xFFu) : m_d[(opcode >> 9) & 7];

    // BTST may read any data mode (dynamic BTST even #imm and PC-relative);
    // the others write back, so they need a data-alterable destination.
    const bool pc_relative_or_immediate = ea_mode == 7 && ea_reg >= 2;
    const bool immediate_allowed = kind == k_btst && !static_form;
    if (ea_mode == 1 || (ea_mode == 7 && ea_reg > 4) || (ea_mode == 7 && ea_reg == 4 && !immediate_allowed)
        || (pc_relative_or_immediate && kind != k_btst)) {
        halt("invalid addressing mode", opcode);
        return k_idle_cycles;
    }

    const bool on_register = ea_mode == 0;
    const Size size = on_register ? Size::Long : Size::Byte;
    const Ea target = decode_ea(ea_mode, ea_reg, size);
    const uint32_t mask = 1u << (bit_number & (on_register ? 31u : 7u));
    const uint32_t value = read_ea(target, size);
    if (m_halted) {
        return k_idle_cycles;
    }

    m_sr = static_cast<uint16_t>((m_sr & ~k_sr_zero) | ((value & mask) == 0 ? k_sr_zero : 0));
    switch (kind) {
    case k_bchg: write_ea(target, size, value ^ mask); break;
    case k_bclr: write_ea(target, size, value & ~mask); break;
    case k_bset: write_ea(target, size, value | mask); break;
    default:     break;
    }

    // MC68000 manual, worst case for data registers (bit numbers 16-31).
    if (on_register) {
        static constexpr uint32_t k_dynamic[4] = {6, 8, 10, 8};
        static constexpr uint32_t k_static[4] = {10, 12, 14, 12};
        return static_form ? k_static[kind] : k_dynamic[kind];
    }
    const uint32_t base = kind == k_btst ? 4 : 8;
    return base + (static_form ? 4 : 0) + ea_cycles(ea_mode, ea_reg, false);
}

} // namespace model1
