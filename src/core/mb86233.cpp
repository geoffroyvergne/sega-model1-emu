#include "core/mb86233.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>

namespace model1 {

namespace {

float u2f(uint32_t v) { return std::bit_cast<float>(v); }
uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }

// Sign-extends the low `bits` bits of `value`.
uint32_t sext(uint32_t value, int bits)
{
    const int shift = 32 - bits;
    return static_cast<uint32_t>(static_cast<int32_t>(value << shift) >> shift);
}

// Float to 32-bit integer, saturating (out of range is undefined in C++).
uint32_t to_int(float value)
{
    if (!(value >= -2147483648.0f)) { // also NaN
        return 0x80000000u;
    }
    if (value >= 2147483648.0f) {
        return 0x7FFFFFFFu;
    }
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}

enum class Outcome { Normal, Repeat, Stall };

constexpr uint32_t k_alu_flags = 0x00000002 | 0x00000008 | 0x00000020 | 0x00000080 | 0x00000800; // ZRD SGD CPD OVD DVZD

} // namespace

Mb86233::Mb86233(Mb86233Bus& bus)
    : m_bus(bus)
{
}

void Mb86233::reset()
{
    m_pc = m_ppc = 0;
    m_st = k_zrc | k_zrd | 0x08000000 | 0x10000000 | 0x20000000 | k_zc0 | k_zc1; // ZRC ZRD ZX0-2 ZC0 ZC1
    m_sp = 0;
    m_a = m_b = m_d = m_p = 0;
    m_r = m_rpc = m_c0 = m_c1 = 1;
    m_b0 = m_b1 = m_x0 = m_x1 = m_i0 = m_i1 = 0;
    m_sft = m_vsm = 0;
    m_vsmr = 7;
    m_mask = 0;
    m_m = 1;
    m_alu_stmask = m_alu_stset = m_alu_r1 = m_alu_r2 = 0;
    m_pcs.fill(0);
    m_stall = false;
    m_instructions = 0;
    m_stalls = 0;
}

void Mb86233::log_once(const char* what, uint32_t value)
{
    if (m_logged < 16) {
        ++m_logged;
        std::cerr << "[TGP DSP] WARNING: " << what << ' ' << Hex{value} << " at PC=" << Hex{m_ppc, 4}
                  << (m_logged == 16 ? " (further warnings suppressed)" : "") << '\n';
    }
}

// ---------------------------------------------------------------------------
// Float field helpers (exponent / mantissa registers)
// ---------------------------------------------------------------------------

uint32_t Mb86233::set_exp(uint32_t value, uint32_t exp)
{
    return (value & 0x807FFFFFu) | ((exp & 0xFF) << 23);
}

uint32_t Mb86233::set_mant(uint32_t value, uint32_t mant)
{
    return (value & 0x7F800000u) | ((mant & 0x00800000u) << 8) | (mant & 0x007FFFFFu);
}

uint32_t Mb86233::get_exp(uint32_t value)
{
    return (value >> 23) & 0xFF;
}

uint32_t Mb86233::get_mant(uint32_t value)
{
    return (value & 0x80000000u) != 0 ? value | 0x7F800000u : value & 0x807FFFFFu;
}

void Mb86233::pcs_push()
{
    for (std::size_t i = 3; i != 0; --i) {
        m_pcs[i] = m_pcs[i - 1];
    }
    m_pcs[0] = m_pc;
}

void Mb86233::pcs_pop()
{
    m_pc = m_pcs[0];
    for (std::size_t i = 0; i != 3; ++i) {
        m_pcs[i] = m_pcs[i + 1];
    }
}

void Mb86233::stset_int(uint32_t value)
{
    m_alu_stset = value != 0 ? ((value & 0x80000000u) != 0 ? k_sgd : 0) : k_zrd;
}

void Mb86233::stset_float(uint32_t value)
{
    m_alu_stset = (value & 0x7FFFFFFFu) != 0 ? ((value & 0x80000000u) != 0 ? k_sgd : 0) : k_zrd;
}

// ---------------------------------------------------------------------------
// ALU: computed at the start of an instruction (alu_pre), written back after
// the transfer for integer operations (alu_post_int) and after the transfer
// for floating-point ones (alu_post_float), as in MAME.
// ---------------------------------------------------------------------------

void Mb86233::alu_pre(uint32_t alu)
{
    m_alu_stmask = k_alu_flags;
    switch (alu) {
    case 0x00: m_alu_stmask = 0; break;                                                           // none
    case 0x01: m_alu_r1 = m_d & m_a; stset_int(m_alu_r1); break;                                  // andd
    case 0x02: m_alu_r1 = m_d | m_a; stset_int(m_alu_r1); break;                                  // orad
    case 0x03: m_alu_r1 = m_d ^ m_a; stset_int(m_alu_r1); break;                                  // eord
    case 0x04: m_alu_r1 = ~m_d; stset_int(m_alu_r1); break;                                       // notd
    case 0x05: stset_float(f2u(u2f(m_d) - u2f(m_a))); break;                                      // fcpd
    case 0x06: m_alu_r1 = f2u(u2f(m_d) + u2f(m_a)); stset_float(m_alu_r1); break;                 // fadd
    case 0x07: m_alu_r1 = f2u(u2f(m_d) - u2f(m_a)); stset_float(m_alu_r1); break;                 // fsbd
    case 0x08: m_alu_stmask = 0; m_alu_stset = 0; m_alu_r1 = f2u(u2f(m_a) * u2f(m_b)); break;      // fml
    case 0x09:                                                                                    // fmsd
        m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
        m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
        stset_float(m_alu_r1);
        break;
    case 0x0A:                                                                                    // fmrd
        m_alu_r1 = f2u(u2f(m_d) - u2f(m_p));
        m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
        stset_float(m_alu_r1);
        break;
    case 0x0B: m_alu_r1 = m_d & 0x7FFFFFFFu; stset_float(m_alu_r1); break;                        // fabd
    case 0x0C: m_alu_r1 = f2u(u2f(m_d) + u2f(m_p)); stset_float(m_alu_r1); break;                 // fsmd
    case 0x0D: m_alu_r1 = m_p; m_alu_r2 = f2u(u2f(m_a) * u2f(m_b)); stset_float(m_alu_r1); break; // fspd
    case 0x0E: m_alu_r1 = f2u(static_cast<float>(static_cast<int32_t>(m_d))); stset_int(m_alu_r1); break; // cxfd
    case 0x0F: {                                                                                  // cfxd
        const float value = u2f(m_d);
        switch ((m_m >> 1) & 3) {
        case 0: m_alu_r1 = to_int(std::round(value)); break;
        case 1: m_alu_r1 = to_int(std::ceil(value)); break;
        case 2: m_alu_r1 = to_int(std::floor(value)); break;
        default: m_alu_r1 = to_int(value); break;
        }
        stset_int(m_alu_r1);
        break;
    }
    case 0x10: m_alu_r1 = f2u(u2f(m_d) / u2f(m_a)); stset_float(m_alu_r1); break;                 // fdvd
    case 0x11: m_alu_r1 = m_d != 0 ? m_d ^ 0x80000000u : 0; stset_float(m_alu_r1); break;        // fned
    case 0x13: m_alu_r1 = f2u(u2f(m_b) + u2f(m_a)); stset_float(m_alu_r1); break;                 // d = b + a
    case 0x14: m_alu_r1 = f2u(u2f(m_b) - u2f(m_a)); stset_float(m_alu_r1); break;                 // d = b - a
    case 0x16: m_alu_r1 = m_sft >= 32 ? 0 : m_d >> m_sft; stset_int(m_alu_r1); break;             // lsrd
    case 0x17: m_alu_r1 = m_sft >= 32 ? 0 : m_d << m_sft; stset_int(m_alu_r1); break;             // lsld
    case 0x18:                                                                                    // asrd
        m_alu_r1 = static_cast<uint32_t>(static_cast<int32_t>(m_d) >> std::min<uint32_t>(m_sft, 31));
        stset_int(m_alu_r1);
        break;
    case 0x19: m_alu_r1 = m_sft >= 32 ? 0 : m_d << m_sft; stset_int(m_alu_r1); break;             // asld
    case 0x1A: m_alu_r1 = m_d + m_a; stset_int(m_alu_r1); break;                                  // addd
    case 0x1B: m_alu_r1 = m_d - m_a; stset_int(m_alu_r1); break;                                  // subd
    default:
        m_alu_stmask = 0;
        log_once("unimplemented ALU operation", alu);
        break;
    }
}

void Mb86233::alu_post_int(uint32_t alu)
{
    switch (alu) {
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x0E: case 0x0F: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
        m_d = m_alu_r1;
        alu_update_st();
        break;
    default:
        break;
    }
}

uint32_t Mb86233::alu_post_float(uint32_t alu)
{
    switch (alu) {
    case 0x05: // flags only
        alu_update_st();
        return 1;
    case 0x06: case 0x07: case 0x0B: case 0x0C:
    case 0x10: case 0x11: case 0x13: case 0x14:
        m_d = m_alu_r1;
        alu_update_st();
        return 1;
    case 0x08:
        m_p = m_alu_r1;
        return 1;
    case 0x09: case 0x0A: case 0x0D:
        m_d = m_alu_r1;
        m_p = m_alu_r2;
        alu_update_st();
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Addressing: two address units (0 and 1), each with base B, index X,
// increment I; VSM masks the index for circular buffers.
// ---------------------------------------------------------------------------

uint16_t Mb86233::ea_pre_0(uint32_t r) const
{
    switch (r & 0x180) {
    case 0x000: return static_cast<uint16_t>(r & 0x7F);
    case 0x080:
    case 0x100: return static_cast<uint16_t>((r & 0x7F) + m_b0 + m_x0);
    default:
        switch (r & 0x60) {
        case 0x00: return static_cast<uint16_t>(m_b0 + m_x0);
        case 0x20: return m_x0;
        case 0x40: return static_cast<uint16_t>(m_b0 + (m_x0 & m_vsmr));
        default:   return static_cast<uint16_t>(m_x0 & m_vsmr);
        }
    }
}

void Mb86233::ea_post_0(uint32_t r)
{
    if ((r & 0x100) == 0) {
        return;
    }
    m_x0 = static_cast<uint16_t>(m_x0 + ((r & 0x080) == 0 ? m_i0 : sext(r, 5)));
}

uint16_t Mb86233::ea_pre_1(uint32_t r) const
{
    switch (r & 0x180) {
    case 0x000: return static_cast<uint16_t>(r & 0x7F);
    case 0x080:
    case 0x100: return static_cast<uint16_t>((r & 0x7F) + m_b1 + m_x1);
    default:
        switch (r & 0x60) {
        case 0x00: return static_cast<uint16_t>(m_b1 + m_x1);
        case 0x20: return m_x1;
        case 0x40: return static_cast<uint16_t>(m_b1 + (m_x1 & m_vsmr));
        default:   return static_cast<uint16_t>(m_x1 & m_vsmr);
        }
    }
}

void Mb86233::ea_post_1(uint32_t r)
{
    if ((r & 0x100) == 0) {
        return;
    }
    m_x1 = static_cast<uint16_t>(m_x1 + ((r & 0x080) == 0 ? m_i1 : sext(r, 5)));
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint32_t Mb86233::read_reg(uint32_t r)
{
    r &= 0x3F;
    if (r >= 0x20 && r < 0x30) {
        return m_bus.rf_read(static_cast<uint16_t>(r & 0x1F));
    }
    switch (r) {
    case 0x00: return m_b0;
    case 0x01: return m_b1;
    case 0x02: return m_x0;
    case 0x03: return m_x1;
    case 0x0C: return m_c0;
    case 0x0D: return m_c1;
    case 0x10: return m_a;
    case 0x11: return get_exp(m_a);
    case 0x12: return get_mant(m_a);
    case 0x13: return m_b;
    case 0x14: return get_exp(m_b);
    case 0x15: return get_mant(m_b);
    case 0x19: return m_d;
    case 0x1A: return get_exp(m_d);
    case 0x1B: return get_mant(m_d);
    case 0x1C: return m_p;
    case 0x1D: return get_exp(m_p);
    case 0x1E: return get_mant(m_p);
    case 0x1F: return m_sft;
    case 0x34: return m_rpc;
    default:
        log_once("unimplemented register read", r);
        return 0;
    }
}

void Mb86233::write_reg(uint32_t r, uint32_t value)
{
    r &= 0x3F;
    if (r >= 0x20 && r < 0x30) {
        m_bus.rf_write(static_cast<uint16_t>(r & 0x1F), value);
        return;
    }
    switch (r) {
    case 0x00: m_b0 = static_cast<uint16_t>(value); break;
    case 0x01: m_b1 = static_cast<uint16_t>(value); break;
    case 0x02: m_x0 = static_cast<uint16_t>(value); break;
    case 0x03: m_x1 = static_cast<uint16_t>(value); break;
    case 0x05: m_i0 = static_cast<uint16_t>(value); break;
    case 0x06: m_i1 = static_cast<uint16_t>(value); break;
    case 0x08: m_sp = static_cast<uint16_t>(value); break;
    case 0x0A:
        m_vsm = static_cast<uint8_t>(value & 7);
        m_vsmr = static_cast<uint16_t>((8u << m_vsm) - 1);
        break;
    case 0x0C:
        m_c0 = static_cast<uint8_t>(value);
        m_st = m_c0 == 1 ? (m_st | k_zc0) : (m_st & ~k_zc0);
        break;
    case 0x0D:
        m_c1 = static_cast<uint8_t>(value);
        m_st = m_c1 == 1 ? (m_st | k_zc1) : (m_st & ~k_zc1);
        break;
    case 0x0F: break;
    case 0x10: m_a = value; break;
    case 0x11: m_a = set_exp(m_a, value); break;
    case 0x12: m_a = set_mant(m_a, value); break;
    case 0x13: m_b = value; break;
    case 0x14: m_b = set_exp(m_b, value); break;
    case 0x15: m_b = set_mant(m_b, value); break;
    case 0x19: m_d = value; break;
    case 0x1A: m_d = set_exp(m_d, value); break;
    case 0x1B: m_d = set_mant(m_d, value); break;
    case 0x1C: m_p = value; break;
    case 0x1D: m_p = set_exp(m_p, value); break;
    case 0x1E: m_p = set_mant(m_p, value); break;
    case 0x1F: m_sft = static_cast<uint8_t>(value); break;
    case 0x34: m_rpc = static_cast<uint8_t>(value); break;
    case 0x3C: m_mask = static_cast<uint16_t>(value); break;
    default:
        log_once("unimplemented register write", r);
        break;
    }
}

void Mb86233::write_internal_1(uint32_t r, uint32_t value, bool bank)
{
    uint16_t ea = ea_pre_1(r);
    if (bank) {
        ea = static_cast<uint16_t>(ea + 0x200);
    }
    m_bus.data_write(ea, value);
    ea_post_1(r);
}

void Mb86233::write_io_1(uint32_t r, uint32_t value)
{
    m_bus.io_write(ea_pre_1(r), value);
    ea_post_1(r);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

uint32_t Mb86233::step()
{
    m_ppc = m_pc;
    const uint32_t opcode = m_bus.program_read(m_pc++);
    uint32_t cycles = 1;
    if (m_trace_remaining > 0) {
        --m_trace_remaining;
        std::cerr << "[TGP DSP trace] " << Hex{m_ppc, 4} << ": " << Hex{opcode} << "  A=" << Hex{m_a} << " B=" << Hex{m_b}
                  << " D=" << Hex{m_d} << " P=" << Hex{m_p} << " ST=" << Hex{m_st} << " B0=" << Hex{m_b0, 4}
                  << " B1=" << Hex{m_b1, 4} << " X0=" << Hex{m_x0, 4} << " X1=" << Hex{m_x1, 4} << '\n';
    }

    // Each case returns early on a stall (before any side effect). The
    // macro keeps the many read sites readable.
#define MB86233_CHECK_STALL() \
    if (m_stall) { outcome = Outcome::Stall; break; }

    Outcome outcome = Outcome::Normal;
    switch ((opcode >> 26) & 0x3F) {
    case 0x00: { // lab: load A and B from memory
        const uint32_t r1 = opcode & 0x1FF;
        const uint32_t r2 = (opcode >> 9) & 0x1FF;
        const uint32_t alu = (opcode >> 21) & 0x1F;
        const uint32_t op = (opcode >> 18) & 0x7;
        alu_pre(alu);
        uint32_t v1 = 0;
        uint32_t v2 = 0;
        switch (op) {
        case 0:
        case 1: // lab mem, mem (e)
            v1 = m_bus.data_read(ea_pre_0(r1));
            MB86233_CHECK_STALL();
            v2 = m_bus.io_read(ea_pre_1(r2));
            MB86233_CHECK_STALL();
            break;
        case 3: // lab mem, mem + 0x200
            v1 = m_bus.data_read(ea_pre_0(r1));
            MB86233_CHECK_STALL();
            v2 = m_bus.data_read(static_cast<uint16_t>(ea_pre_1(r2) + 0x200));
            MB86233_CHECK_STALL();
            break;
        case 4: // lab mem + 0x200, mem
            v1 = m_bus.data_read(static_cast<uint16_t>(ea_pre_0(r1) + 0x200));
            MB86233_CHECK_STALL();
            v2 = m_bus.data_read(ea_pre_1(r2));
            MB86233_CHECK_STALL();
            break;
        default:
            log_once("unhandled lab sub-operation", op);
            alu_post_int(alu);
            cycles += alu_post_float(alu);
            goto done;
        }
        if (outcome == Outcome::Stall) {
            break;
        }
        ea_post_0(r1);
        ea_post_1(r2);
        m_a = v1;
        m_b = v2;
        alu_post_int(alu);
        cycles += alu_post_float(alu);
        break;
    }

    case 0x07: { // ld / mov
        const uint32_t r1 = opcode & 0x1FF;
        const uint32_t r2 = (opcode >> 9) & 0x1FF;
        const uint32_t alu = (opcode >> 21) & 0x1F;
        const uint32_t op = (opcode >> 18) & 0x7;
        alu_pre(alu);
        switch (op) {
        case 0:
        case 1: { // mov mem, mem (e)
            const uint32_t v = m_bus.data_read(ea_pre_0(r1));
            MB86233_CHECK_STALL();
            ea_post_0(r1);
            alu_post_int(alu);
            write_io_1(r2, v);
            break;
        }
        case 2: { // mov mem (e), mem
            const uint32_t v = m_bus.io_read(ea_pre_0(r1));
            MB86233_CHECK_STALL();
            ea_post_0(r1);
            alu_post_int(alu);
            write_internal_1(r2, v, false);
            break;
        }
        case 3: { // mov mem, mem + 0x200
            const uint32_t v = m_bus.data_read(ea_pre_0(r1));
            MB86233_CHECK_STALL();
            ea_post_0(r1);
            alu_post_int(alu);
            write_internal_1(r2, v, true);
            break;
        }
        case 4: { // mov mem + 0x200, mem
            const uint32_t v = m_bus.data_read(static_cast<uint16_t>(ea_pre_0(r1) + 0x200));
            MB86233_CHECK_STALL();
            ea_post_0(r1);
            alu_post_int(alu);
            write_internal_1(r2, v, false);
            break;
        }
        case 5: { // mov mem (o), mem
            const uint32_t v = m_bus.program_read(ea_pre_0(r1));
            ea_post_0(r1);
            alu_post_int(alu);
            write_internal_1(r2, v, false);
            break;
        }
        case 7:
            switch (r2 >> 6) {
            case 0: { // mov reg, mem
                const uint32_t v = read_reg(r2);
                MB86233_CHECK_STALL();
                alu_post_int(alu);
                write_internal_1(r1, v, false);
                break;
            }
            case 1: { // mov reg, mem (e)
                const uint32_t v = read_reg(r2);
                MB86233_CHECK_STALL();
                alu_post_int(alu);
                write_io_1(r1, v);
                break;
            }
            case 2: { // mov mem + 0x200, reg
                const uint32_t v = m_bus.data_read(static_cast<uint16_t>(ea_pre_1(r1) + 0x200));
                MB86233_CHECK_STALL();
                ea_post_1(r1);
                alu_post_int(alu);
                write_reg(r2, v);
                break;
            }
            case 3: { // mov mem, reg
                const uint32_t v = m_bus.data_read(ea_pre_1(r1));
                MB86233_CHECK_STALL();
                ea_post_1(r1);
                alu_post_int(alu);
                write_reg(r2, v);
                break;
            }
            case 4: { // mov mem (e), reg
                const uint32_t v = m_bus.io_read(ea_pre_1(r1));
                MB86233_CHECK_STALL();
                ea_post_1(r1);
                alu_post_int(alu);
                write_reg(r2, v);
                break;
            }
            case 5: { // mov mem (o), reg
                const uint32_t v = m_bus.program_read(ea_pre_0(r1));
                ea_post_0(r1);
                alu_post_int(alu);
                write_reg(r2, v);
                break;
            }
            case 6: { // mov reg, reg
                const uint32_t v = read_reg(r1);
                MB86233_CHECK_STALL();
                alu_post_int(alu);
                write_reg(r2, v);
                break;
            }
            default:
                alu_post_int(alu);
                log_once("unhandled ld/mov sub-operation 7/", r2 >> 6);
                break;
            }
            break;
        default:
            alu_post_int(alu);
            log_once("unhandled ld/mov sub-operation", op);
            break;
        }
        if (outcome == Outcome::Stall) {
            break;
        }
        // Floating-point results land after the transfer.
        cycles += alu_post_float(alu);
        break;
    }

    case 0x0D: { // stm / clm: only stmh (mode: bit 0 float, bits 2-1 rounding)
        const uint32_t sub = (opcode >> 17) & 7;
        if (sub == 5) {
            m_m = static_cast<uint16_t>(opcode);
        } else {
            log_once("unimplemented opcode 0D/", sub);
        }
        break;
    }

    case 0x0E: // lipl / lia / lib / lid: 24-bit immediates
        switch ((opcode >> 24) & 0x3) {
        case 0: m_p = (m_p & 0xFF000000u) | (opcode & 0x00FFFFFFu); break;
        case 1: m_a = sext(opcode, 24); break;
        case 2: m_b = sext(opcode, 24); break;
        default: m_d = sext(opcode, 24); break;
        }
        break;

    case 0x0F: { // rep / clr0 / clr1 / set
        const uint32_t alu = (opcode >> 20) & 0x1F;
        const uint32_t sub = (opcode >> 17) & 7;
        alu_pre(alu);
        switch (sub) {
        case 0: // clr0
            if ((opcode & 0x0004) != 0) m_a = 0;
            if ((opcode & 0x0008) != 0) m_b = 0;
            if ((opcode & 0x0010) != 0) m_d = 0;
            break;
        case 1: // clr1: flags mapping unknown
        case 3: // set: flags mapping unknown (0x0800 = enable interrupts)
            break;
        case 2: { // rep: repeat the next instruction
            const auto count = static_cast<uint8_t>((opcode & 0x8000) != 0 ? read_reg(opcode) : opcode);
            if (m_stall) {
                outcome = Outcome::Stall;
                break;
            }
            m_r = count;
            outcome = Outcome::Repeat;
            break;
        }
        default:
            log_once("unimplemented opcode 0F/", sub);
            break;
        }
        if (outcome == Outcome::Normal) {
            alu_post_int(alu);
        }
        break;
    }

    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        write_reg(opcode >> 24, sext(opcode, 24)); // ldi
        break;

    case 0x2F:
    case 0x3F: { // conditional branches of every kind
        const uint32_t cond = (opcode >> 20) & 0x1F;
        const uint32_t subtype = (opcode >> 17) & 7;
        const uint16_t data = static_cast<uint16_t>(opcode);
        bool passed = false;
        switch (cond) {
        case 0x00: passed = (m_st & k_zrd) != 0; break;             // D zero
        case 0x01: passed = (m_st & k_sgd) == 0; break;             // D >= 0
        case 0x02: passed = (m_st & (k_zrd | k_sgd)) != 0; break;   // D <= 0
        case 0x0A: case 0x0B: case 0x0C: case 0x12: passed = false; break; // GPIO inputs: not connected
        case 0x10: passed = (m_st & k_zc0) == 0; break;             // C0 != 1
        case 0x11: passed = (m_st & k_zc1) == 0; break;             // C1 != 1
        case 0x16: passed = true; break;                            // always
        default:
            log_once("unimplemented branch condition", cond);
            break;
        }
        if ((opcode & 0x40000000u) != 0) {
            passed = !passed;
        }
        if (passed) {
            switch (subtype) {
            case 0: m_pc = data; break; // brif #adr
            case 1:                     // brul reg / adr
            case 3: {                   // bsul reg / adr
                uint32_t target = 0;
                if ((opcode & 0x4000) != 0) {
                    target = read_reg(opcode);
                    MB86233_CHECK_STALL();
                } else {
                    target = m_bus.data_read(ea_pre_0(opcode));
                    MB86233_CHECK_STALL();
                    ea_post_0(opcode);
                }
                if (subtype == 3) {
                    pcs_push();
                }
                m_pc = static_cast<uint16_t>(target);
                break;
            }
            case 2: pcs_push(); m_pc = data; break; // bsif #adr
            case 5: pcs_pop(); break;               // rtif
            case 6: {                               // ldif adr, rn
                const uint32_t v = m_bus.data_read(ea_pre_0(opcode));
                MB86233_CHECK_STALL();
                ea_post_0(opcode);
                write_reg(opcode >> 9, v);
                break;
            }
            default:
                log_once("unimplemented branch subtype", subtype);
                break;
            }
            if (outcome == Outcome::Stall) {
                break;
            }
        }
        if (subtype < 2) { // loop counters
            if (cond == 0x10 && m_c0 != 1) {
                if (--m_c0 == 1) m_st |= k_zc0;
            } else if (cond == 0x11 && m_c1 != 1) {
                if (--m_c1 == 1) m_st |= k_zc1;
            }
        }
        break;
    }

    default:
        log_once("unimplemented opcode type", (opcode >> 26) & 0x3F);
        break;
    }
#undef MB86233_CHECK_STALL

done:
    if (outcome == Outcome::Stall) {
        m_pc = m_ppc;
        m_stall = false;
        ++m_stalls;
        return 1;
    }
    if (outcome == Outcome::Normal && m_r != 1) {
        m_pc = m_ppc;
        --m_r;
    }
    ++m_instructions;
    return cycles;
}

} // namespace model1
