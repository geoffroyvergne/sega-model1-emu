#include "core/z80.hpp"

#include "core/log.hpp"

#include <iostream>
#include <utility>

namespace model1 {

namespace {

constexpr bool parity_even(uint8_t value)
{
    value ^= static_cast<uint8_t>(value >> 4);
    value ^= static_cast<uint8_t>(value >> 2);
    value ^= static_cast<uint8_t>(value >> 1);
    return (value & 1) == 0;
}

// Extra T-states for an (IX + d) / (IY + d) operand over the (HL) form,
// on top of the 4 T-states of the prefix itself.
constexpr uint32_t k_index_displacement_cycles = 8;

} // namespace

Z80::Z80(Z80Bus& bus)
    : m_bus(bus)
{
}

void Z80::reset()
{
    m_pc = 0;
    m_i = 0;
    m_r = 0;
    m_iff1 = m_iff2 = false;
    m_im = 0;
    m_halted = false;
    m_ei_delay = false;
    m_nmi_pending = false;
    m_sp = 0xFFFF;
    m_a = m_f = 0xFF;
    m_cycles = 0;
    m_instructions = 0;
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

uint8_t Z80::fetch_opcode()
{
    m_r = static_cast<uint8_t>((m_r & 0x80) | ((m_r + 1) & 0x7F));
    return read(m_pc++);
}

uint8_t Z80::fetch_byte()
{
    return read(m_pc++);
}

uint16_t Z80::fetch_word()
{
    const uint8_t low = fetch_byte();
    return static_cast<uint16_t>(low | (fetch_byte() << 8));
}

uint16_t Z80::read_word(uint16_t address)
{
    return static_cast<uint16_t>(read(address) | (read(static_cast<uint16_t>(address + 1)) << 8));
}

void Z80::write_word(uint16_t address, uint16_t value)
{
    write(address, static_cast<uint8_t>(value));
    write(static_cast<uint16_t>(address + 1), static_cast<uint8_t>(value >> 8));
}

void Z80::push(uint16_t value)
{
    m_sp = static_cast<uint16_t>(m_sp - 2);
    write_word(m_sp, value);
}

uint16_t Z80::pop()
{
    const uint16_t value = read_word(m_sp);
    m_sp = static_cast<uint16_t>(m_sp + 2);
    return value;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint16_t Z80::index_reg(Index index) const
{
    switch (index) {
    case Index::IX: return m_ix;
    case Index::IY: return m_iy;
    case Index::HL: break;
    }
    return hl();
}

void Z80::set_index_reg(Index index, uint16_t value)
{
    switch (index) {
    case Index::IX: m_ix = value; return;
    case Index::IY: m_iy = value; return;
    case Index::HL: set_hl(value); return;
    }
}

uint8_t Z80::reg8(int r, Index index) const
{
    switch (r) {
    case 0: return m_b;
    case 1: return m_c;
    case 2: return m_d;
    case 3: return m_e;
    case 4: return static_cast<uint8_t>(index_reg(index) >> 8);
    case 5: return static_cast<uint8_t>(index_reg(index));
    case 7: return m_a;
    default: return 0; // 6 = (HL): handled by callers
    }
}

void Z80::set_reg8(int r, uint8_t value, Index index)
{
    switch (r) {
    case 0: m_b = value; return;
    case 1: m_c = value; return;
    case 2: m_d = value; return;
    case 3: m_e = value; return;
    case 4: set_index_reg(index, static_cast<uint16_t>((index_reg(index) & 0x00FF) | (value << 8))); return;
    case 5: set_index_reg(index, static_cast<uint16_t>((index_reg(index) & 0xFF00) | value)); return;
    case 7: m_a = value; return;
    default: return;
    }
}

uint16_t Z80::rp(int p, Index index) const
{
    switch (p) {
    case 0: return bc();
    case 1: return de();
    case 2: return index_reg(index);
    default: return m_sp;
    }
}

void Z80::set_rp(int p, uint16_t value, Index index)
{
    switch (p) {
    case 0: set_bc(value); return;
    case 1: set_de(value); return;
    case 2: set_index_reg(index, value); return;
    default: m_sp = value; return;
    }
}

uint16_t Z80::memory_operand(Index index)
{
    if (index == Index::HL) {
        return hl();
    }
    const auto displacement = static_cast<int8_t>(fetch_byte());
    return static_cast<uint16_t>(index_reg(index) + displacement);
}

bool Z80::condition(int cc) const
{
    switch (cc) {
    case 0: return (m_f & k_flag_z) == 0;  // NZ
    case 1: return (m_f & k_flag_z) != 0;  // Z
    case 2: return (m_f & k_flag_c) == 0;  // NC
    case 3: return (m_f & k_flag_c) != 0;  // C
    case 4: return (m_f & k_flag_pv) == 0; // PO
    case 5: return (m_f & k_flag_pv) != 0; // PE
    case 6: return (m_f & k_flag_s) == 0;  // P
    default: return (m_f & k_flag_s) != 0; // M
    }
}

// ---------------------------------------------------------------------------
// ALU
// ---------------------------------------------------------------------------

void Z80::set_szp(uint8_t value)
{
    m_f = static_cast<uint8_t>((value & (k_flag_s | k_flag_y | k_flag_x)) | (value == 0 ? k_flag_z : 0)
                               | (parity_even(value) ? k_flag_pv : 0));
}

void Z80::alu(int op, uint8_t value)
{
    const uint8_t a = m_a;
    switch (op) {
    case 0:   // ADD
    case 1: { // ADC
        const uint32_t carry = op == 1 && (m_f & k_flag_c) != 0 ? 1 : 0;
        const uint32_t sum = a + value + carry;
        const auto r = static_cast<uint8_t>(sum);
        m_a = r;
        m_f = static_cast<uint8_t>((r & (k_flag_s | k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                                   | ((a ^ value ^ r) & k_flag_h)
                                   | (((a ^ ~value) & (a ^ r) & 0x80) != 0 ? k_flag_pv : 0)
                                   | (sum > 0xFF ? k_flag_c : 0));
        return;
    }
    case 2:   // SUB
    case 3:   // SBC
    case 7: { // CP
        const uint32_t carry = op == 3 && (m_f & k_flag_c) != 0 ? 1 : 0;
        const uint32_t diff = static_cast<uint32_t>(a) - value - carry;
        const auto r = static_cast<uint8_t>(diff);
        // CP takes bits 5 / 3 from the operand, the others from the result.
        const uint8_t xy = op == 7 ? value : r;
        m_f = static_cast<uint8_t>((r & k_flag_s) | (xy & (k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                                   | ((a ^ value ^ r) & k_flag_h)
                                   | (((a ^ value) & (a ^ r) & 0x80) != 0 ? k_flag_pv : 0) | k_flag_n
                                   | ((diff & 0x100) != 0 ? k_flag_c : 0));
        if (op != 7) {
            m_a = r;
        }
        return;
    }
    case 4: // AND
        m_a = static_cast<uint8_t>(a & value);
        set_szp(m_a);
        m_f |= k_flag_h;
        return;
    case 5: // XOR
        m_a = static_cast<uint8_t>(a ^ value);
        set_szp(m_a);
        return;
    default: // OR
        m_a = static_cast<uint8_t>(a | value);
        set_szp(m_a);
        return;
    }
}

uint8_t Z80::inc8(uint8_t value)
{
    const auto r = static_cast<uint8_t>(value + 1);
    m_f = static_cast<uint8_t>((m_f & k_flag_c) | (r & (k_flag_s | k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                               | ((r & 0x0F) == 0 ? k_flag_h : 0) | (r == 0x80 ? k_flag_pv : 0));
    return r;
}

uint8_t Z80::dec8(uint8_t value)
{
    const auto r = static_cast<uint8_t>(value - 1);
    m_f = static_cast<uint8_t>((m_f & k_flag_c) | (r & (k_flag_s | k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                               | ((r & 0x0F) == 0x0F ? k_flag_h : 0) | (r == 0x7F ? k_flag_pv : 0) | k_flag_n);
    return r;
}

uint8_t Z80::rotate_shift(int op, uint8_t value)
{
    const bool carry_in = (m_f & k_flag_c) != 0;
    bool carry = false;
    uint8_t r = 0;
    switch (op) {
    case 0: carry = (value & 0x80) != 0; r = static_cast<uint8_t>((value << 1) | (carry ? 1 : 0)); break;        // RLC
    case 1: carry = (value & 0x01) != 0; r = static_cast<uint8_t>((value >> 1) | (carry ? 0x80 : 0)); break;     // RRC
    case 2: carry = (value & 0x80) != 0; r = static_cast<uint8_t>((value << 1) | (carry_in ? 1 : 0)); break;     // RL
    case 3: carry = (value & 0x01) != 0; r = static_cast<uint8_t>((value >> 1) | (carry_in ? 0x80 : 0)); break;  // RR
    case 4: carry = (value & 0x80) != 0; r = static_cast<uint8_t>(value << 1); break;                            // SLA
    case 5: carry = (value & 0x01) != 0; r = static_cast<uint8_t>((value >> 1) | (value & 0x80)); break;         // SRA
    case 6: carry = (value & 0x80) != 0; r = static_cast<uint8_t>((value << 1) | 1); break;                      // SLL (undocumented)
    default: carry = (value & 0x01) != 0; r = static_cast<uint8_t>(value >> 1); break;                           // SRL
    }
    set_szp(r);
    if (carry) {
        m_f |= k_flag_c;
    }
    return r;
}

void Z80::bit_test(int bit, uint8_t value)
{
    const bool set = (value & (1u << bit)) != 0;
    m_f = static_cast<uint8_t>((m_f & k_flag_c) | k_flag_h | (value & (k_flag_y | k_flag_x))
                               | (set ? 0 : (k_flag_z | k_flag_pv)) | (bit == 7 && set ? k_flag_s : 0));
}

uint16_t Z80::add16(uint16_t a, uint16_t b)
{
    const uint32_t sum = static_cast<uint32_t>(a) + b;
    const auto r = static_cast<uint16_t>(sum);
    m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | ((r >> 8) & (k_flag_y | k_flag_x))
                               | (((a ^ b ^ r) >> 8) & k_flag_h) | (sum > 0xFFFF ? k_flag_c : 0));
    return r;
}

uint16_t Z80::adc16(uint16_t a, uint16_t b)
{
    const uint32_t carry = (m_f & k_flag_c) != 0 ? 1 : 0;
    const uint32_t sum = static_cast<uint32_t>(a) + b + carry;
    const auto r = static_cast<uint16_t>(sum);
    m_f = static_cast<uint8_t>(((r >> 8) & (k_flag_s | k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                               | (((a ^ b ^ r) >> 8) & k_flag_h)
                               | (((a ^ ~b) & (a ^ r) & 0x8000) != 0 ? k_flag_pv : 0) | (sum > 0xFFFF ? k_flag_c : 0));
    return r;
}

uint16_t Z80::sbc16(uint16_t a, uint16_t b)
{
    const uint32_t carry = (m_f & k_flag_c) != 0 ? 1 : 0;
    const uint32_t diff = static_cast<uint32_t>(a) - b - carry;
    const auto r = static_cast<uint16_t>(diff);
    m_f = static_cast<uint8_t>(((r >> 8) & (k_flag_s | k_flag_y | k_flag_x)) | (r == 0 ? k_flag_z : 0)
                               | (((a ^ b ^ r) >> 8) & k_flag_h)
                               | (((a ^ b) & (a ^ r) & 0x8000) != 0 ? k_flag_pv : 0) | k_flag_n
                               | ((diff & 0x10000) != 0 ? k_flag_c : 0));
    return r;
}

void Z80::daa()
{
    const uint8_t a = m_a;
    const bool n = (m_f & k_flag_n) != 0;
    uint8_t correction = 0;
    bool carry = (m_f & k_flag_c) != 0;
    if ((m_f & k_flag_h) != 0 || (a & 0x0F) > 9) {
        correction |= 0x06;
    }
    if (carry || a > 0x99) {
        correction |= 0x60;
        carry = true;
    }
    const auto r = static_cast<uint8_t>(n ? a - correction : a + correction);
    const bool half = n ? ((m_f & k_flag_h) != 0 && (a & 0x0F) < 6) : (a & 0x0F) > 9;
    m_a = r;
    set_szp(r);
    m_f = static_cast<uint8_t>(m_f | (half ? k_flag_h : 0) | (n ? k_flag_n : 0) | (carry ? k_flag_c : 0));
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

uint32_t Z80::step()
{
    uint32_t cycles = 0;
    if (m_nmi_pending || (m_int_line && m_iff1 && !m_ei_delay)) {
        cycles = take_interrupt();
    } else if (m_halted) {
        m_r = static_cast<uint8_t>((m_r & 0x80) | ((m_r + 1) & 0x7F)); // HALT keeps executing NOPs
        cycles = 4;
    } else {
        m_ei_delay = false;
        const uint8_t opcode = fetch_opcode();
        cycles = execute_main(opcode, Index::HL);
        ++m_instructions;
    }
    m_cycles += cycles;
    return cycles;
}

uint32_t Z80::take_interrupt()
{
    m_halted = false;
    m_r = static_cast<uint8_t>((m_r & 0x80) | ((m_r + 1) & 0x7F));
    if (m_nmi_pending) {
        m_nmi_pending = false;
        m_iff1 = false; // IFF2 keeps the previous IFF1 for RETN
        push(m_pc);
        m_pc = 0x0066;
        return 11;
    }
    m_iff1 = m_iff2 = false;
    push(m_pc);
    if (m_im == 2) {
        m_pc = read_word(static_cast<uint16_t>((m_i << 8) | m_int_vector));
        return 19;
    }
    // Modes 0 and 1: RST 38h (mode 0 assumes 0xFF on the data bus).
    m_pc = 0x0038;
    return 13;
}

uint32_t Z80::execute_main(uint8_t opcode, Index index)
{
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const int p = y >> 1;
    const int q = y & 1;
    // A prefix (DD / FD) costs an extra opcode fetch; memory_operand adds
    // the displacement cost for (IX + d) forms.
    const uint32_t prefix = index == Index::HL ? 0 : 4;
    const uint32_t indexed_memory = index == Index::HL ? 0 : k_index_displacement_cycles;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4 + prefix; // NOP
            case 1: { // EX AF, AF'
                std::swap(m_a, m_a2);
                std::swap(m_f, m_f2);
                return 4 + prefix;
            }
            case 2: { // DJNZ d
                const auto d = static_cast<int8_t>(fetch_byte());
                --m_b;
                if (m_b != 0) {
                    m_pc = static_cast<uint16_t>(m_pc + d);
                    return 13 + prefix;
                }
                return 8 + prefix;
            }
            case 3: { // JR d
                const auto d = static_cast<int8_t>(fetch_byte());
                m_pc = static_cast<uint16_t>(m_pc + d);
                return 12 + prefix;
            }
            default: { // JR cc, d (NZ Z NC C)
                const auto d = static_cast<int8_t>(fetch_byte());
                if (condition(y - 4)) {
                    m_pc = static_cast<uint16_t>(m_pc + d);
                    return 12 + prefix;
                }
                return 7 + prefix;
            }
            }
        case 1:
            if (q == 0) { // LD rp, nn
                set_rp(p, fetch_word(), index);
                return 10 + prefix;
            }
            set_index_reg(index, add16(index_reg(index), rp(p, index))); // ADD HL, rp
            return 11 + prefix;
        case 2:
            switch (y) {
            case 0: write(bc(), m_a); return 7 + prefix;                       // LD (BC), A
            case 1: m_a = read(bc()); return 7 + prefix;                       // LD A, (BC)
            case 2: write(de(), m_a); return 7 + prefix;                       // LD (DE), A
            case 3: m_a = read(de()); return 7 + prefix;                       // LD A, (DE)
            case 4: write_word(fetch_word(), index_reg(index)); return 16 + prefix; // LD (nn), HL
            case 5: set_index_reg(index, read_word(fetch_word())); return 16 + prefix; // LD HL, (nn)
            case 6: write(fetch_word(), m_a); return 13 + prefix;              // LD (nn), A
            default: m_a = read(fetch_word()); return 13 + prefix;             // LD A, (nn)
            }
        case 3: // INC / DEC rp
            set_rp(p, static_cast<uint16_t>(rp(p, index) + (q == 0 ? 1 : -1)), index);
            return 6 + prefix;
        case 4:
        case 5: { // INC r / DEC r
            if (y == 6) {
                const uint16_t address = memory_operand(index);
                const uint8_t value = read(address);
                write(address, z == 4 ? inc8(value) : dec8(value));
                return 11 + prefix + indexed_memory;
            }
            const uint8_t value = reg8(y, index);
            set_reg8(y, z == 4 ? inc8(value) : dec8(value), index);
            return 4 + prefix;
        }
        case 6: // LD r, n
            if (y == 6) {
                const uint16_t address = memory_operand(index);
                write(address, fetch_byte());
                return 10 + prefix + (index == Index::HL ? 0 : 5); // LD (IX+d), n: 19
            }
            set_reg8(y, fetch_byte(), index);
            return 7 + prefix;
        default:
            switch (y) {
            case 0: { // RLCA
                const bool c = (m_a & 0x80) != 0;
                m_a = static_cast<uint8_t>((m_a << 1) | (c ? 1 : 0));
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x)) | (c ? k_flag_c : 0));
                return 4 + prefix;
            }
            case 1: { // RRCA
                const bool c = (m_a & 0x01) != 0;
                m_a = static_cast<uint8_t>((m_a >> 1) | (c ? 0x80 : 0));
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x)) | (c ? k_flag_c : 0));
                return 4 + prefix;
            }
            case 2: { // RLA
                const bool c = (m_a & 0x80) != 0;
                m_a = static_cast<uint8_t>((m_a << 1) | ((m_f & k_flag_c) != 0 ? 1 : 0));
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x)) | (c ? k_flag_c : 0));
                return 4 + prefix;
            }
            case 3: { // RRA
                const bool c = (m_a & 0x01) != 0;
                m_a = static_cast<uint8_t>((m_a >> 1) | ((m_f & k_flag_c) != 0 ? 0x80 : 0));
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x)) | (c ? k_flag_c : 0));
                return 4 + prefix;
            }
            case 4: daa(); return 4 + prefix;
            case 5: // CPL
                m_a = static_cast<uint8_t>(~m_a);
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv | k_flag_c)) | (m_a & (k_flag_y | k_flag_x)) | k_flag_h | k_flag_n);
                return 4 + prefix;
            case 6: // SCF
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x)) | k_flag_c);
                return 4 + prefix;
            default: // CCF
                m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_pv)) | (m_a & (k_flag_y | k_flag_x))
                                           | ((m_f & k_flag_c) != 0 ? k_flag_h : k_flag_c));
                return 4 + prefix;
            }
        }
    case 1:
        if (y == 6 && z == 6) { // HALT
            m_halted = true;
            return 4 + prefix;
        }
        if (y == 6) { // LD (HL), r - with an index prefix, r is the plain H / L
            const uint16_t address = memory_operand(index);
            write(address, reg8(z, Index::HL));
            return 7 + prefix + indexed_memory;
        }
        if (z == 6) { // LD r, (HL)
            const uint16_t address = memory_operand(index);
            set_reg8(y, read(address), Index::HL);
            return 7 + prefix + indexed_memory;
        }
        set_reg8(y, reg8(z, index), index); // LD r, r'
        return 4 + prefix;
    case 2: // ALU A, r
        if (z == 6) {
            alu(y, read(memory_operand(index)));
            return 7 + prefix + indexed_memory;
        }
        alu(y, reg8(z, index));
        return 4 + prefix;
    default:
        break;
    }

    // x == 3
    switch (z) {
    case 0: // RET cc
        if (condition(y)) {
            m_pc = pop();
            return 11 + prefix;
        }
        return 5 + prefix;
    case 1:
        if (q == 0) { // POP rp2 (AF for p = 3)
            const uint16_t value = pop();
            if (p == 3) {
                m_a = static_cast<uint8_t>(value >> 8);
                m_f = static_cast<uint8_t>(value);
            } else {
                set_rp(p, value, index);
            }
            return 10 + prefix;
        }
        switch (p) {
        case 0: m_pc = pop(); return 10 + prefix; // RET
        case 1: // EXX
            std::swap(m_b, m_b2); std::swap(m_c, m_c2);
            std::swap(m_d, m_d2); std::swap(m_e, m_e2);
            std::swap(m_h, m_h2); std::swap(m_l, m_l2);
            return 4 + prefix;
        case 2: m_pc = index_reg(index); return 4 + prefix; // JP (HL)
        default: m_sp = index_reg(index); return 6 + prefix; // LD SP, HL
        }
    case 2: { // JP cc, nn
        const uint16_t target = fetch_word();
        if (condition(y)) {
            m_pc = target;
        }
        return 10 + prefix;
    }
    case 3:
        switch (y) {
        case 0: m_pc = fetch_word(); return 10 + prefix; // JP nn
        case 1: // CB prefix
            return index == Index::HL ? execute_cb() : prefix + execute_index_cb(index);
        case 2: { // OUT (n), A
            const uint8_t n = fetch_byte();
            m_bus.out(static_cast<uint16_t>((m_a << 8) | n), m_a);
            return 11 + prefix;
        }
        case 3: { // IN A, (n)
            const uint8_t n = fetch_byte();
            m_a = m_bus.in(static_cast<uint16_t>((m_a << 8) | n));
            return 11 + prefix;
        }
        case 4: { // EX (SP), HL
            const uint16_t value = read_word(m_sp);
            write_word(m_sp, index_reg(index));
            set_index_reg(index, value);
            return 19 + prefix;
        }
        case 5: { // EX DE, HL (never affected by a prefix)
            const uint16_t value = de();
            set_de(hl());
            set_hl(value);
            return 4 + prefix;
        }
        case 6: // DI
            m_iff1 = m_iff2 = false;
            return 4 + prefix;
        default: // EI: interrupts are accepted after the next instruction
            m_iff1 = m_iff2 = true;
            m_ei_delay = true;
            return 4 + prefix;
        }
    case 4: { // CALL cc, nn
        const uint16_t target = fetch_word();
        if (condition(y)) {
            push(m_pc);
            m_pc = target;
            return 17 + prefix;
        }
        return 10 + prefix;
    }
    case 5:
        if (q == 0) { // PUSH rp2
            push(p == 3 ? static_cast<uint16_t>((m_a << 8) | m_f) : rp(p, index));
            return 11 + prefix;
        }
        switch (p) {
        case 0: { // CALL nn
            const uint16_t target = fetch_word();
            push(m_pc);
            m_pc = target;
            return 17 + prefix;
        }
        case 1: // DD prefix (a repeated prefix replaces the previous one)
            return prefix + execute_main(fetch_opcode(), Index::IX);
        case 2: // ED prefix
            return prefix + execute_ed();
        default: // FD prefix
            return prefix + execute_main(fetch_opcode(), Index::IY);
        }
    case 6: // ALU A, n
        alu(y, fetch_byte());
        return 7 + prefix;
    default: // RST
        push(m_pc);
        m_pc = static_cast<uint16_t>(y * 8);
        return 11 + prefix;
    }
}

uint32_t Z80::execute_cb()
{
    const uint8_t opcode = fetch_opcode();
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const bool memory = z == 6;
    const uint8_t value = memory ? read(hl()) : reg8(z, Index::HL);

    uint8_t result = value;
    switch (x) {
    case 0: result = rotate_shift(y, value); break;
    case 1:
        bit_test(y, value);
        return memory ? 12 : 8;
    case 2: result = static_cast<uint8_t>(value & ~(1u << y)); break; // RES
    default: result = static_cast<uint8_t>(value | (1u << y)); break; // SET
    }
    if (memory) {
        write(hl(), result);
        return 15;
    }
    set_reg8(z, result, Index::HL);
    return 8;
}

// DDCB d op / FDCB d op: the displacement comes before the final opcode.
// The result also goes to register z (undocumented) unless z = 6.
uint32_t Z80::execute_index_cb(Index index)
{
    const auto displacement = static_cast<int8_t>(fetch_byte());
    const uint8_t opcode = fetch_byte(); // not an M1 cycle: R is not incremented
    const auto address = static_cast<uint16_t>(index_reg(index) + displacement);
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const uint8_t value = read(address);

    uint8_t result = value;
    switch (x) {
    case 0: result = rotate_shift(y, value); break;
    case 1:
        bit_test(y, value);
        return 20 - 4; // 20 including the DD / FD prefix added by the caller
    case 2: result = static_cast<uint8_t>(value & ~(1u << y)); break;
    default: result = static_cast<uint8_t>(value | (1u << y)); break;
    }
    write(address, result);
    if (z != 6) {
        set_reg8(z, result, Index::HL);
    }
    return 23 - 4;
}

uint32_t Z80::execute_ed()
{
    const uint8_t opcode = fetch_opcode();
    const int x = opcode >> 6;
    const int y = (opcode >> 3) & 7;
    const int z = opcode & 7;
    const int p = y >> 1;
    const int q = y & 1;

    if (x == 1) {
        switch (z) {
        case 0: { // IN r, (C) (y = 6: flags only)
            const uint8_t value = m_bus.in(bc());
            const uint8_t carry = m_f & k_flag_c;
            set_szp(value);
            m_f |= carry;
            if (y != 6) {
                set_reg8(y, value, Index::HL);
            }
            return 12;
        }
        case 1: // OUT (C), r (y = 6: outputs 0)
            m_bus.out(bc(), y == 6 ? 0 : reg8(y, Index::HL));
            return 12;
        case 2: // SBC HL, rp / ADC HL, rp
            set_hl(q == 0 ? sbc16(hl(), rp(p, Index::HL)) : adc16(hl(), rp(p, Index::HL)));
            return 15;
        case 3: { // LD (nn), rp / LD rp, (nn)
            const uint16_t address = fetch_word();
            if (q == 0) {
                write_word(address, rp(p, Index::HL));
            } else {
                set_rp(p, read_word(address), Index::HL);
            }
            return 20;
        }
        case 4: { // NEG
            const uint8_t value = m_a;
            m_a = 0;
            alu(2, value);
            return 8;
        }
        case 5: // RETN / RETI
            m_pc = pop();
            m_iff1 = m_iff2;
            return 14;
        case 6: // IM 0 / 1 / 2
            m_im = (y & 3) == 2 ? 1 : ((y & 3) == 3 ? 2 : 0);
            return 8;
        default:
            switch (y) {
            case 0: m_i = m_a; return 9;  // LD I, A
            case 1: m_r = m_a; return 9;  // LD R, A
            case 2:                       // LD A, I
            case 3: {                     // LD A, R
                m_a = y == 2 ? m_i : m_r;
                const uint8_t carry = m_f & k_flag_c;
                m_f = static_cast<uint8_t>((m_a & (k_flag_s | k_flag_y | k_flag_x)) | (m_a == 0 ? k_flag_z : 0)
                                           | (m_iff2 ? k_flag_pv : 0) | carry);
                return 9;
            }
            case 4: { // RRD
                const uint8_t value = read(hl());
                write(hl(), static_cast<uint8_t>((m_a << 4) | (value >> 4)));
                m_a = static_cast<uint8_t>((m_a & 0xF0) | (value & 0x0F));
                const uint8_t carry = m_f & k_flag_c;
                set_szp(m_a);
                m_f |= carry;
                return 18;
            }
            case 5: { // RLD
                const uint8_t value = read(hl());
                write(hl(), static_cast<uint8_t>((value << 4) | (m_a & 0x0F)));
                m_a = static_cast<uint8_t>((m_a & 0xF0) | (value >> 4));
                const uint8_t carry = m_f & k_flag_c;
                set_szp(m_a);
                m_f |= carry;
                return 18;
            }
            default:
                return 8; // ED 77 / ED 7F: NOP
            }
        }
    }
    if (x == 2 && y >= 4 && z <= 3) {
        const bool increment = (y & 1) == 0;
        const bool repeat = y >= 6;
        switch (z) {
        case 0: return block_load(increment, repeat);
        case 1: return block_compare(increment, repeat);
        case 2: return block_in(increment, repeat);
        default: return block_out(increment, repeat);
        }
    }
    if (!m_logged_undefined_ed) {
        m_logged_undefined_ed = true;
        std::cerr << "[Z80] WARNING: undefined opcode ED " << Hex{opcode, 2} << " at PC="
                  << Hex{static_cast<uint32_t>(m_pc - 2), 4} << ", executed as NOP\n";
    }
    return 8;
}

uint32_t Z80::block_load(bool increment, bool repeat)
{
    const uint8_t value = read(hl());
    write(de(), value);
    const int step = increment ? 1 : -1;
    set_hl(static_cast<uint16_t>(hl() + step));
    set_de(static_cast<uint16_t>(de() + step));
    set_bc(static_cast<uint16_t>(bc() - 1));
    const auto n = static_cast<uint8_t>(value + m_a);
    m_f = static_cast<uint8_t>((m_f & (k_flag_s | k_flag_z | k_flag_c)) | (n & k_flag_x) | ((n << 4) & k_flag_y)
                               | (bc() != 0 ? k_flag_pv : 0));
    if (repeat && bc() != 0) {
        m_pc = static_cast<uint16_t>(m_pc - 2);
        return 21;
    }
    return 16;
}

uint32_t Z80::block_compare(bool increment, bool repeat)
{
    const uint8_t value = read(hl());
    const auto r = static_cast<uint8_t>(m_a - value);
    set_hl(static_cast<uint16_t>(hl() + (increment ? 1 : -1)));
    set_bc(static_cast<uint16_t>(bc() - 1));
    const bool half = ((m_a ^ value ^ r) & k_flag_h) != 0;
    const auto n = static_cast<uint8_t>(r - (half ? 1 : 0));
    m_f = static_cast<uint8_t>((m_f & k_flag_c) | (r & k_flag_s) | (r == 0 ? k_flag_z : 0) | (half ? k_flag_h : 0)
                               | (n & k_flag_x) | ((n << 4) & k_flag_y) | (bc() != 0 ? k_flag_pv : 0) | k_flag_n);
    if (repeat && bc() != 0 && r != 0) {
        m_pc = static_cast<uint16_t>(m_pc - 2);
        return 21;
    }
    return 16;
}

uint32_t Z80::block_in(bool increment, bool repeat)
{
    const uint8_t value = m_bus.in(bc());
    write(hl(), value);
    set_hl(static_cast<uint16_t>(hl() + (increment ? 1 : -1)));
    --m_b;
    m_f = static_cast<uint8_t>((m_b & (k_flag_s | k_flag_y | k_flag_x)) | (m_b == 0 ? k_flag_z : 0) | k_flag_n);
    if (repeat && m_b != 0) {
        m_pc = static_cast<uint16_t>(m_pc - 2);
        return 21;
    }
    return 16;
}

uint32_t Z80::block_out(bool increment, bool repeat)
{
    const uint8_t value = read(hl());
    --m_b;
    m_bus.out(bc(), value);
    set_hl(static_cast<uint16_t>(hl() + (increment ? 1 : -1)));
    m_f = static_cast<uint8_t>((m_b & (k_flag_s | k_flag_y | k_flag_x)) | (m_b == 0 ? k_flag_z : 0) | k_flag_n);
    if (repeat && m_b != 0) {
        m_pc = static_cast<uint16_t>(m_pc - 2);
        return 21;
    }
    return 16;
}

} // namespace model1
