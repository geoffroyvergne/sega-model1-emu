#include "core/v60.hpp"

#include "core/bus.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <string_view>

namespace model1 {

namespace {

// Misaligned stack accesses logged before the warning goes quiet.
constexpr uint32_t k_max_stack_warnings = 8;

// Operand size helpers (sizes are 1, 2 or 4 bytes).
constexpr uint32_t size_mask(uint32_t bytes)
{
    return bytes >= 4 ? 0xFFFFFFFFu : ((1u << (bytes * 8)) - 1);
}

constexpr uint32_t sign_bit(uint32_t bytes)
{
    return 1u << (bytes * 8 - 1);
}

// Sign-extends the low `bits` bits (1-32) of `value`.
constexpr int64_t sign_extend_bits(uint32_t value, uint32_t bits)
{
    const uint64_t sign = uint64_t{1} << (bits - 1);
    const uint64_t masked = value & ((sign << 1) - 1);
    return static_cast<int64_t>((masked ^ sign)) - static_cast<int64_t>(sign);
}

constexpr int64_t sign_extend(uint32_t value, uint32_t bytes)
{
    const uint32_t bits = bytes * 8;
    const uint64_t masked = value & size_mask(bytes);
    return static_cast<int64_t>(masked << (64 - bits)) >> (64 - bits);
}

} // namespace

V60::V60(Bus& bus)
    : m_bus(bus)
{
}

void V60::reset()
{
    // General-purpose and stack registers are undefined after a real reset;
    // zero them so runs are deterministic.
    m_reg.fill(0);
    m_pc = k_reset_pc;
    m_psw = k_reset_psw;
    m_isp = 0;
    m_level_sp.fill(0);
    m_sbr = 0;
    m_privileged_other.fill(0);
    m_instruction_pc = k_reset_pc;
    m_opcode = 0;
    m_pending_irqs.reset();
    m_halt_reason = HaltReason::None;
    m_instruction_count = 0;
    m_interrupt_count = 0;
    m_stack_warning_count = 0;
    std::cerr << "[V60] Reset, PC=" << Hex{m_pc} << '\n';
}

// ---------------------------------------------------------------------------
// Main fetch / decode / execute step
// ---------------------------------------------------------------------------

uint32_t V60::execute_cycle()
{
    // Interrupts are sampled between instructions. Accepting one also wakes
    // the CPU from HALT.
    if (can_accept_interrupt()) {
        accept_interrupt();
        return k_average_cycles_per_instruction;
    }

    if (is_halted()) {
        // Idle: time passes, nothing executes.
        return k_average_cycles_per_instruction;
    }

    // Fetch
    m_instruction_pc = m_pc;
    if (m_trace_remaining > 0) {
        --m_trace_remaining;
        std::cerr << "[V60 trace] " << Hex{m_instruction_pc} << ':';
        for (uint32_t i = 0; i < 8; ++i) {
            std::cerr << ' ' << Hex{m_bus.peek_byte((m_instruction_pc + i) & k_address_mask), 2};
        }
        std::cerr << '\n';
    }
    m_opcode = fetch_byte();

    // Decode + execute
    const bool mode_m = (m_opcode & 1) != 0; // one-operand instructions
    const bool bcc8 = m_opcode >= k_op_bcc8_first && m_opcode <= k_op_bcc8_last;
    const bool bcc16 = m_opcode >= k_op_bcc16_first && m_opcode <= k_op_bcc16_last;
    if ((bcc8 || bcc16) && (m_opcode & 0x0F) != 0x0B) { // 0x6B / 0x7B are unused
        op_branch(static_cast<Condition>(m_opcode & 0x0F), bcc16);
    } else {
        switch (m_opcode) {
        case k_op_halt:     op_halt();                                   break;
        case k_op_nop:      op_nop();                                    break;
        case k_op_jmp_m0:
        case k_op_jmp_m1:   op_jmp(mode_m);                              break;
        case k_op_dbcc:     op_dbcc(false);                              break;
        case k_op_dbcc_not: op_dbcc(true);                               break;
        case k_op_bit_field: op_bit_field();                             break;
        case k_op_bit_string: op_bit_string();                           break;
        case k_op_float:    op_float(false);                             break;
        case k_op_float_convert: op_float(true);                         break;
        case k_op_setf:     op_setf();                                   break;
        case k_op_xch_b:    op_exchange(OperandSize::Byte);              break;
        case k_op_xch_h:    op_exchange(OperandSize::Half);              break;
        case k_op_xch_w:    op_exchange(OperandSize::Word);              break;
        case k_op_jsr_m0:
        case k_op_jsr_m1:   op_jsr(mode_m);                              break;
        case k_op_mov_b:    op_mov(OperandSize::Byte);                   break;
        case k_op_mov_h:    op_mov(OperandSize::Half);                   break;
        case k_op_mov_w:    op_mov(OperandSize::Word);                   break;
        case k_op_movd:     op_move_double();                            break;
        case k_op_decimal:  op_decimal();                                break;
        case k_op_rvbit:    op_reverse(true);                            break;
        case k_op_rvbyt:    op_reverse(false);                           break;
        case k_op_dispose:  op_dispose();                                break;
        case k_op_prepare_m0:
        case k_op_prepare_m1: op_prepare(mode_m);                        break;
        case k_op_tasi_m0:
        case k_op_tasi_m1:  op_tasi(mode_m);                             break;
        case k_op_retiu_m0:
        case k_op_retiu_m1: op_retis(mode_m);                            break; // same as RETIS (MAME)
        case k_op_getpsw_m0:
        case k_op_getpsw_m1: op_getpsw(mode_m);                          break;
        case k_op_movs_bh:  op_move_extend(OperandSize::Byte, OperandSize::Half, true);  break;
        case k_op_movz_bh:  op_move_extend(OperandSize::Byte, OperandSize::Half, false); break;
        case k_op_movs_bw:  op_move_extend(OperandSize::Byte, OperandSize::Word, true);  break;
        case k_op_movz_bw:  op_move_extend(OperandSize::Byte, OperandSize::Word, false); break;
        case k_op_movs_hw:  op_move_extend(OperandSize::Half, OperandSize::Word, true);  break;
        case k_op_movz_hw:  op_move_extend(OperandSize::Half, OperandSize::Word, false); break;
        case k_op_movt_hb:  op_move_truncate(OperandSize::Half, OperandSize::Byte);       break;
        case k_op_movt_wb:  op_move_truncate(OperandSize::Word, OperandSize::Byte);       break;
        case k_op_movt_wh:  op_move_truncate(OperandSize::Word, OperandSize::Half);       break;
        case k_op_add_b:    op_add_sub(false, OperandSize::Byte);        break;
        case k_op_add_h:    op_add_sub(false, OperandSize::Half);        break;
        case k_op_add_w:    op_add_sub(false, OperandSize::Word);        break;
        case k_op_sub_b:    op_add_sub(true, OperandSize::Byte);         break;
        case k_op_sub_h:    op_add_sub(true, OperandSize::Half);         break;
        case k_op_sub_w:    op_add_sub(true, OperandSize::Word);         break;
        case k_op_addc_b:   op_add_sub_carry(false, OperandSize::Byte);  break;
        case k_op_addc_h:   op_add_sub_carry(false, OperandSize::Half);  break;
        case k_op_addc_w:   op_add_sub_carry(false, OperandSize::Word);  break;
        case k_op_subc_b:   op_add_sub_carry(true, OperandSize::Byte);   break;
        case k_op_subc_h:   op_add_sub_carry(true, OperandSize::Half);   break;
        case k_op_subc_w:   op_add_sub_carry(true, OperandSize::Word);   break;
        case k_op_dec_b_m0:
        case k_op_dec_b_m1: op_inc_dec(true, OperandSize::Byte, mode_m);  break;
        case k_op_dec_h_m0:
        case k_op_dec_h_m1: op_inc_dec(true, OperandSize::Half, mode_m);  break;
        case k_op_dec_w_m0:
        case k_op_dec_w_m1: op_inc_dec(true, OperandSize::Word, mode_m);  break;
        case k_op_inc_b_m0:
        case k_op_inc_b_m1: op_inc_dec(false, OperandSize::Byte, mode_m); break;
        case k_op_inc_h_m0:
        case k_op_inc_h_m1: op_inc_dec(false, OperandSize::Half, mode_m); break;
        case k_op_inc_w_m0:
        case k_op_inc_w_m1: op_inc_dec(false, OperandSize::Word, mode_m); break;
        case k_op_cmp_b:    op_cmp(OperandSize::Byte);                   break;
        case k_op_ldpr:     op_ldpr();                                   break;
        case k_op_stpr:     op_stpr();                                   break;
        case k_op_cmp_h:    op_cmp(OperandSize::Half);                   break;
        case k_op_cmp_w:    op_cmp(OperandSize::Word);                   break;
        case k_op_and_b:    op_logic(LogicOp::And, OperandSize::Byte);   break;
        case k_op_and_h:    op_logic(LogicOp::And, OperandSize::Half);   break;
        case k_op_and_w:    op_logic(LogicOp::And, OperandSize::Word);   break;
        case k_op_or_b:     op_logic(LogicOp::Or, OperandSize::Byte);    break;
        case k_op_or_h:     op_logic(LogicOp::Or, OperandSize::Half);    break;
        case k_op_or_w:     op_logic(LogicOp::Or, OperandSize::Word);    break;
        case k_op_xor_b:    op_logic(LogicOp::Xor, OperandSize::Byte);   break;
        case k_op_xor_h:    op_logic(LogicOp::Xor, OperandSize::Half);   break;
        case k_op_xor_w:    op_logic(LogicOp::Xor, OperandSize::Word);   break;
        case k_op_not_b:    op_not(OperandSize::Byte);                   break;
        case k_op_neg_b:    op_neg(OperandSize::Byte);                   break;
        case k_op_neg_h:    op_neg(OperandSize::Half);                   break;
        case k_op_neg_w:    op_neg(OperandSize::Word);                   break;
        case k_op_not_h:    op_not(OperandSize::Half);                   break;
        case k_op_not_w:    op_not(OperandSize::Word);                   break;
        case k_op_shl_b:    op_shift(ShiftKind::Logical, OperandSize::Byte);    break;
        case k_op_shl_h:    op_shift(ShiftKind::Logical, OperandSize::Half);    break;
        case k_op_shl_w:    op_shift(ShiftKind::Logical, OperandSize::Word);    break;
        case k_op_sha_b:    op_shift(ShiftKind::Arithmetic, OperandSize::Byte); break;
        case k_op_rot_b:    op_rotate(false, OperandSize::Byte);               break;
        case k_op_rot_h:    op_rotate(false, OperandSize::Half);               break;
        case k_op_rot_w:    op_rotate(false, OperandSize::Word);               break;
        case k_op_rotc_b:   op_rotate(true, OperandSize::Byte);                break;
        case k_op_rotc_h:   op_rotate(true, OperandSize::Half);                break;
        case k_op_rotc_w:   op_rotate(true, OperandSize::Word);                break;
        case k_op_sha_h:    op_shift(ShiftKind::Arithmetic, OperandSize::Half); break;
        case k_op_sha_w:    op_shift(ShiftKind::Arithmetic, OperandSize::Word); break;
        case k_op_mul_b:    op_mul(Signedness::Signed, OperandSize::Byte);      break;
        case k_op_mul_h:    op_mul(Signedness::Signed, OperandSize::Half);      break;
        case k_op_mul_w:    op_mul(Signedness::Signed, OperandSize::Word);      break;
        case k_op_mulu_b:   op_mul(Signedness::Unsigned, OperandSize::Byte);    break;
        case k_op_mulu_h:   op_mul(Signedness::Unsigned, OperandSize::Half);    break;
        case k_op_mulu_w:   op_mul(Signedness::Unsigned, OperandSize::Word);    break;
        case k_op_div_b:    op_div(Signedness::Signed, OperandSize::Byte);      break;
        case k_op_div_h:    op_div(Signedness::Signed, OperandSize::Half);      break;
        case k_op_div_w:    op_div(Signedness::Signed, OperandSize::Word);      break;
        case k_op_divu_b:   op_div(Signedness::Unsigned, OperandSize::Byte);    break;
        case k_op_divu_h:   op_div(Signedness::Unsigned, OperandSize::Half);    break;
        case k_op_divu_w:   op_div(Signedness::Unsigned, OperandSize::Word);    break;
        case k_op_mulx:     op_mul_extended(Signedness::Signed);                break;
        case k_op_mulux:    op_mul_extended(Signedness::Unsigned);              break;
        case k_op_divx:     op_div_extended(Signedness::Signed);                break;
        case k_op_divux:    op_div_extended(Signedness::Unsigned);              break;
        case k_op_rem_b:    op_rem(Signedness::Signed, OperandSize::Byte);      break;
        case k_op_remu_b:   op_rem(Signedness::Unsigned, OperandSize::Byte);    break;
        case k_op_rem_h:    op_rem(Signedness::Signed, OperandSize::Half);      break;
        case k_op_remu_h:   op_rem(Signedness::Unsigned, OperandSize::Half);    break;
        case k_op_rem_w:    op_rem(Signedness::Signed, OperandSize::Word);      break;
        case k_op_remu_w:   op_rem(Signedness::Unsigned, OperandSize::Word);    break;
        case k_op_test1:    op_bit(BitOp::Test);                                break;
        case k_op_set1:     op_bit(BitOp::Set);                                 break;
        case k_op_clr1:     op_bit(BitOp::Clear);                               break;
        case k_op_not1:     op_bit(BitOp::Invert);                              break;
        case k_op_test_b_m0:
        case k_op_test_b_m1: op_test(OperandSize::Byte, mode_m);               break;
        case k_op_test_h_m0:
        case k_op_test_h_m1: op_test(OperandSize::Half, mode_m);               break;
        case k_op_test_w_m0:
        case k_op_test_w_m1: op_test(OperandSize::Word, mode_m);               break;
        case k_op_updpsw_w: op_updpsw(0x00FFFFFF);                          break;
        case k_op_updpsw_h: op_updpsw(0x0000FFFF);                          break;
        case k_op_in_b:     op_in(OperandSize::Byte);                       break;
        case k_op_in_h:     op_in(OperandSize::Half);                       break;
        case k_op_in_w:     op_in(OperandSize::Word);                       break;
        case k_op_out_b:    op_out(OperandSize::Byte);                      break;
        case k_op_out_h:    op_out(OperandSize::Half);                      break;
        case k_op_out_w:    op_out(OperandSize::Word);                      break;
        case k_op_string_b: op_string(OperandSize::Byte);                   break;
        case k_op_string_h: op_string(OperandSize::Half);                   break;
        case k_op_movea_b:  op_movea(OperandSize::Byte);                    break;
        case k_op_movea_h:  op_movea(OperandSize::Half);                    break;
        case k_op_movea_w:  op_movea(OperandSize::Word);                    break;
        case k_op_push_m0:
        case k_op_push_m1:  op_push(mode_m);                             break;
        case k_op_pop_m0:
        case k_op_pop_m1:   op_pop(mode_m);                              break;
        case k_op_pushm_m0:
        case k_op_pushm_m1: op_pushm(mode_m);                            break;
        case k_op_popm_m0:
        case k_op_popm_m1:  op_popm(mode_m);                             break;
        case k_op_call:     op_call();                                   break;
        case k_op_ret_m0:
        case k_op_ret_m1:   op_ret(mode_m);                              break;
        case k_op_bsr:      op_bsr();                                    break;
        case k_op_rsr:      op_rsr();                                    break;
        case k_op_retis_m0:
        case k_op_retis_m1: op_retis(mode_m);                            break;
        default:
            halt(HaltReason::UnimplementedOpcode, m_opcode);
            return k_average_cycles_per_instruction;
        }
    }

    // An instruction that halted on a bad operand did not complete.
    if (m_halt_reason == HaltReason::None || m_halt_reason == HaltReason::HaltInstruction) {
        ++m_instruction_count;
    }
    return k_average_cycles_per_instruction;
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void V60::request_interrupt(uint8_t vector)
{
    if (m_pending_irqs.test(vector)) {
        return; // already pending; requests for the same vector merge
    }
    m_pending_irqs.set(vector);

    if constexpr (k_trace_irq) {
        std::cerr << "[V60] IRQ requested: vector " << Hex{vector, 2}
                  << ((m_psw & k_psw_ie) != 0 ? "" : " (pending, PSW.IE=0)") << '\n';
    }
}

bool V60::can_accept_interrupt() const
{
    const bool line = m_irq_line && m_irq_acknowledge;
    if ((!m_pending_irqs.any() && !line) || (m_psw & k_psw_ie) == 0) {
        return false;
    }
    // A HALT waits for interrupts; any other halt reason is a fatal stop.
    return m_halt_reason == HaltReason::None || m_halt_reason == HaltReason::HaltInstruction;
}

void V60::accept_interrupt()
{
    // Acknowledge cycle: the lowest latched vector number, or the one the
    // interrupt controller supplies.
    uint32_t vector = 0;
    if (m_pending_irqs.any()) {
        while (!m_pending_irqs.test(vector)) {
            ++vector;
        }
        m_pending_irqs.reset(vector);
    } else {
        vector = m_irq_acknowledge();
    }

    const bool woke_from_halt = (m_halt_reason == HaltReason::HaltInstruction);
    m_halt_reason = HaltReason::None;

    // Enter interrupt context: interrupt stack, level 0, IE and trace off.
    const uint32_t old_psw = m_psw;
    uint32_t new_psw = old_psw;
    new_psw &= ~(k_psw_el | k_psw_ie | k_psw_te | k_psw_tp | k_psw_ae | k_psw_em);
    new_psw |= k_psw_is | k_psw_asa;
    write_psw(new_psw); // switches SP to ISP

    const uint32_t return_pc = m_pc;
    push_long(old_psw);
    push_long(return_pc);

    const uint32_t entry = vector + k_irq_vector_base;
    const uint32_t table_address = (m_sbr & ~0xFFFu) + entry * 4;
    m_pc = read_data_long(table_address);
    ++m_interrupt_count;

    if constexpr (k_trace_irq) {
        std::cerr << "[V60] IRQ acknowledged: vector " << Hex{vector, 2}
                  << ", table entry " << Hex{entry, 2} << " @ " << Hex{table_address}
                  << " -> handler " << Hex{m_pc} << " (return PC " << Hex{return_pc}
                  << (woke_from_halt ? ", woke from HALT)\n" : ")\n");
    }
}

// ---------------------------------------------------------------------------
// Instruction stream
// ---------------------------------------------------------------------------

uint8_t V60::fetch_byte()
{
    const uint8_t value = m_bus.read_byte(m_pc & k_address_mask);
    m_pc += 1;
    return value;
}

uint16_t V60::fetch_word()
{
    const uint16_t value = m_bus.read_word(m_pc & k_address_mask);
    m_pc += 2;
    return value;
}

uint32_t V60::fetch_long()
{
    const uint32_t value = m_bus.read_long(m_pc & k_address_mask);
    m_pc += 4;
    return value;
}

uint32_t V60::fetch_displacement(int bytes)
{
    switch (bytes) {
    case 1:  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fetch_byte())));
    case 2:  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fetch_word())));
    default: return fetch_long();
    }
}

// ---------------------------------------------------------------------------
// Data memory and stack
// ---------------------------------------------------------------------------

uint32_t V60::read_data(uint32_t address, OperandSize size)
{
    address &= k_address_mask;
    switch (size) {
    case OperandSize::Byte: return m_bus.read_byte(address);
    case OperandSize::Half: return m_bus.read_word(address);
    case OperandSize::Word: return m_bus.read_long(address);
    case OperandSize::Quad: break; // 64-bit operands are accessed as two words
    }
    return 0;
}

void V60::write_data(uint32_t address, OperandSize size, uint32_t value)
{
    address &= k_address_mask;
    switch (size) {
    case OperandSize::Byte: m_bus.write_byte(address, static_cast<uint8_t>(value));  break;
    case OperandSize::Half: m_bus.write_word(address, static_cast<uint16_t>(value)); break;
    case OperandSize::Word: m_bus.write_long(address, value);                        break;
    case OperandSize::Quad: break; // 64-bit operands are accessed as two words
    }
}

// Defensive stack checks. The V60 allows unaligned accesses and puts no
// limits on SP, so neither case stops execution, but both almost always mean
// a bug (ours or the program's): a misaligned SP, or a stack that has run
// out of work RAM (overflow on push, underflow on pop).
void V60::check_stack_access(const char* operation, uint32_t address)
{
    const bool misaligned = (address & 3) != 0;
    const bool outside_ram = !m_bus.is_work_ram(address & k_address_mask, 4);
    if (!misaligned && !outside_ram) {
        return;
    }
    if (m_stack_warning_count < k_max_stack_warnings) {
        std::cerr << "[V60] WARNING: stack " << operation;
        if (outside_ram) {
            std::cerr << (std::string_view{operation} == "push" ? " overflow" : " underflow")
                      << ": access outside work RAM";
        } else {
            std::cerr << ": misaligned";
        }
        std::cerr << ", SP=" << Hex{address} << " at PC=" << Hex{m_instruction_pc} << '\n';
    } else if (m_stack_warning_count == k_max_stack_warnings) {
        std::cerr << "[V60] WARNING: further stack warnings not logged\n";
    }
    ++m_stack_warning_count;
}

// The stack grows downwards; SP points at the last pushed item.
void V60::push_long(uint32_t value)
{
    m_reg[k_reg_sp] -= 4;
    check_stack_access("push", m_reg[k_reg_sp]);
    write_data_long(m_reg[k_reg_sp], value);
}

uint32_t V60::pop_long()
{
    check_stack_access("pop", m_reg[k_reg_sp]);
    const uint32_t value = read_data_long(m_reg[k_reg_sp]);
    m_reg[k_reg_sp] += 4;
    return value;
}

// ---------------------------------------------------------------------------
// Operand decoding
// ---------------------------------------------------------------------------

V60::Operand V60::decode_operand(bool mode_m, OperandSize size)
{
    const uint8_t mode = fetch_byte();
    const uint8_t group = static_cast<uint8_t>(mode >> 5);  // top 3 bits
    const uint8_t reg = static_cast<uint8_t>(mode & 0x1F);
    const uint32_t bytes = bytes_of(size);
    static constexpr std::array<int, 3> k_disp_bytes = {1, 2, 4};

    Operand operand;
    if (m_bit_addressing && mode_m && (group == 0b100 || group == 0b101)) {
        // Autoincrement / autodecrement have no defined step for a bit field.
        halt(HaltReason::UnimplementedOperand, mode);
        return operand;
    }
    if (mode_m) {
        switch (group) {
        case 0b000:
        case 0b001:
        case 0b010: {
            // Double displacement: disp2[disp1[Rn]]
            const uint32_t outer = fetch_displacement(k_disp_bytes[group]);
            const uint32_t inner = fetch_displacement(k_disp_bytes[group]);
            operand.kind = Operand::Kind::Memory;
            operand.address = read_data_long(m_reg[reg] + outer);
            if (m_bit_addressing) {
                operand.bit_offset = static_cast<int32_t>(inner); // disp2 counts bits
            } else {
                operand.address += inner;
            }
            return operand;
        }
        case 0b011:
            operand.kind = Operand::Kind::Register;
            operand.reg = reg;
            return operand;
        case 0b100:
            // Autoincrement: use Rn, then advance it by the operand size.
            operand.kind = Operand::Kind::Memory;
            operand.address = m_reg[reg];
            m_reg[reg] += bytes;
            return operand;
        case 0b101:
            // Autodecrement: step Rn back by the operand size, then use it.
            m_reg[reg] -= bytes;
            operand.kind = Operand::Kind::Memory;
            operand.address = m_reg[reg];
            return operand;
        case 0b110:
            return decode_indexed(reg, size);
        default:
            // 111: reserved.
            halt(HaltReason::UnimplementedOperand, mode);
            return operand;
        }
    }

    switch (group) {
    case 0b000:
    case 0b001:
    case 0b010:
        // Displacement: disp[Rn] (bit addressing: Rn, disp bits further)
        operand.kind = Operand::Kind::Memory;
        operand.address = m_reg[reg];
        if (m_bit_addressing) {
            operand.bit_offset = static_cast<int32_t>(fetch_displacement(k_disp_bytes[group]));
        } else {
            operand.address += fetch_displacement(k_disp_bytes[group]);
        }
        return operand;
    case 0b011:
        // Register indirect: [Rn]
        operand.kind = Operand::Kind::Memory;
        operand.address = m_reg[reg];
        return operand;
    case 0b100:
    case 0b101:
    case 0b110:
        // Displacement indirect: [disp[Rn]]
        operand.kind = Operand::Kind::Memory;
        operand.address = read_data_long(m_reg[reg] + fetch_displacement(k_disp_bytes[group - 4]));
        return operand;
    default:
        return decode_group7(mode, size);
    }
}

V60::Operand V60::decode_group7(uint8_t mode, OperandSize size)
{
    const uint8_t sub = static_cast<uint8_t>(mode & 0x1F);
    static constexpr std::array<int, 3> k_disp_bytes = {1, 2, 4};

    Operand operand;
    if (sub < 0x10) {
        // Immediate quick: 4-bit unsigned constant inside the mode byte.
        operand.kind = Operand::Kind::Immediate;
        operand.value = sub;
        return operand;
    }
    switch (sub) {
    case 0x10:
    case 0x11:
    case 0x12:
        // PC displacement: disp[PC] (bit addressing: PC, disp bits further)
        operand.kind = Operand::Kind::Memory;
        operand.address = m_instruction_pc;
        if (m_bit_addressing) {
            operand.bit_offset = static_cast<int32_t>(fetch_displacement(k_disp_bytes[sub - 0x10]));
        } else {
            operand.address += fetch_displacement(k_disp_bytes[sub - 0x10]);
        }
        return operand;
    case 0x13:
        // Direct address: /abs32
        operand.kind = Operand::Kind::Memory;
        operand.address = fetch_long();
        return operand;
    case 0x14:
        // Immediate of the operation's size.
        operand.kind = Operand::Kind::Immediate;
        switch (size) {
        case OperandSize::Byte: operand.value = fetch_byte(); break;
        case OperandSize::Half: operand.value = fetch_word(); break;
        case OperandSize::Word: operand.value = fetch_long(); break;
        case OperandSize::Quad:
            // No instruction takes a 64-bit immediate.
            halt(HaltReason::UnimplementedOperand, mode);
            return Operand{};
        }
        return operand;
    case 0x18:
    case 0x19:
    case 0x1A:
        // PC displacement indirect: [disp[PC]]
        operand.kind = Operand::Kind::Memory;
        operand.address = read_data_long(m_instruction_pc + fetch_displacement(k_disp_bytes[sub - 0x18]));
        return operand;
    case 0x1B:
        // Direct address deferred: [/abs32]
        operand.kind = Operand::Kind::Memory;
        operand.address = read_data_long(fetch_long());
        return operand;
    case 0x1C:
    case 0x1D:
    case 0x1E: {
        // PC double displacement: disp2[disp1[PC]]
        const uint32_t outer = fetch_displacement(k_disp_bytes[sub - 0x1C]);
        const uint32_t inner = fetch_displacement(k_disp_bytes[sub - 0x1C]);
        operand.kind = Operand::Kind::Memory;
        operand.address = read_data_long(m_instruction_pc + outer);
        if (m_bit_addressing) {
            operand.bit_offset = static_cast<int32_t>(inner);
        } else {
            operand.address += inner;
        }
        return operand;
    }
    default:
        // 0x15-0x17, 0x1F: reserved.
        halt(HaltReason::UnimplementedOperand, mode);
        return operand;
    }
}

// Indexed modes: effective address = base address + Rx * operand size.
// `index_reg` comes from the first mode byte; a second mode byte selects the
// base form and base register.
V60::Operand V60::decode_indexed(uint8_t index_reg, OperandSize size)
{
    const uint8_t mode2 = fetch_byte();
    const uint8_t form = static_cast<uint8_t>(mode2 >> 5);
    const uint8_t base = static_cast<uint8_t>(mode2 & 0x1F);
    // Bit addressing: the index register is a bit offset, not added here.
    const uint32_t scaled_index = m_bit_addressing ? 0 : m_reg[index_reg] * bytes_of(size);
    static constexpr std::array<int, 3> k_disp_bytes = {1, 2, 4};

    Operand operand;
    operand.kind = Operand::Kind::Memory;
    if (m_bit_addressing) {
        operand.bit_offset = static_cast<int32_t>(m_reg[index_reg]);
    }
    switch (form) {
    case 0b000:
    case 0b001:
    case 0b010:
        // disp[Rb](Rx)
        operand.address = m_reg[base] + fetch_displacement(k_disp_bytes[form]) + scaled_index;
        return operand;
    case 0b011:
        // [Rb](Rx)
        operand.address = m_reg[base] + scaled_index;
        return operand;
    case 0b100:
    case 0b101:
    case 0b110:
        // [disp[Rb]](Rx)
        operand.address = read_data_long(m_reg[base] + fetch_displacement(k_disp_bytes[form - 4])) + scaled_index;
        return operand;
    default:
        break;
    }

    // Form 111: PC-relative and absolute bases; bit 4 must be set.
    switch (mode2 & 0x1F) {
    case 0x10:
    case 0x11:
    case 0x12:
        // disp[PC](Rx)
        operand.address = m_instruction_pc + fetch_displacement(k_disp_bytes[(mode2 & 0x1F) - 0x10]) + scaled_index;
        return operand;
    case 0x13:
        // /abs32(Rx)
        operand.address = fetch_long() + scaled_index;
        return operand;
    case 0x18:
    case 0x19:
    case 0x1A:
        // [disp[PC]](Rx)
        operand.address = read_data_long(m_instruction_pc + fetch_displacement(k_disp_bytes[(mode2 & 0x1F) - 0x18]))
                        + scaled_index;
        return operand;
    case 0x1B:
        // [/abs32](Rx)
        operand.address = read_data_long(fetch_long()) + scaled_index;
        return operand;
    default:
        halt(HaltReason::UnimplementedOperand, mode2);
        return Operand{};
    }
}

V60::OperandPair V60::decode_format12(OperandSize source_size, OperandSize destination_size)
{
    const uint8_t flags = fetch_byte();
    const bool format2 = (flags & 0x80) != 0;
    const bool mode_m = (flags & 0x40) != 0;
    const bool dir_d = (flags & 0x20) != 0;
    const uint8_t reg = static_cast<uint8_t>(flags & 0x1F);

    OperandPair operands;
    if (format2) {
        // Format II: 1 m1 m2 xxxxx, both operands are general fields.
        const bool mode_m2 = (flags & 0x20) != 0;
        operands.source = decode_operand(mode_m, source_size);
        if (operands.source.kind != Operand::Kind::Invalid) {
            operands.destination = decode_operand(mode_m2, destination_size);
        }
        return operands;
    }

    // Format I: 0 m d rrrrr, one operand is register Rr.
    Operand reg_operand;
    reg_operand.kind = Operand::Kind::Register;
    reg_operand.reg = reg;
    if (dir_d) {
        operands.destination = reg_operand;
        operands.source = decode_operand(mode_m, source_size);
    } else {
        operands.source = reg_operand;
        operands.destination = decode_operand(mode_m, destination_size);
    }
    return operands;
}

bool V60::require_valid(const OperandPair& operands) const
{
    // decode_operand() has already halted and logged on failure.
    return operands.source.kind != Operand::Kind::Invalid
        && operands.destination.kind != Operand::Kind::Invalid;
}

uint32_t V60::read_operand(const Operand& operand, OperandSize size)
{
    const uint32_t mask = size_mask(bytes_of(size));
    switch (operand.kind) {
    case Operand::Kind::Register:  return m_reg[operand.reg] & mask;
    case Operand::Kind::Memory:    return read_data(operand.address, size);
    case Operand::Kind::Immediate: return operand.value & mask;
    case Operand::Kind::Invalid:   break;
    }
    return 0;
}

bool V60::write_operand(const Operand& operand, OperandSize size, uint32_t value)
{
    switch (operand.kind) {
    case Operand::Kind::Register: {
        // Byte / halfword writes replace only the low bits of the register.
        const uint32_t mask = size_mask(bytes_of(size));
        m_reg[operand.reg] = (m_reg[operand.reg] & ~mask) | (value & mask);
        return true;
    }
    case Operand::Kind::Memory:
        write_data(operand.address, size, value);
        return true;
    case Operand::Kind::Immediate:
        // On hardware this raises an addressing-mode exception, which is not
        // emulated yet.
        halt(HaltReason::ImmediateDestination, 0);
        return false;
    case Operand::Kind::Invalid:
        break;
    }
    return false;
}

std::optional<uint32_t> V60::operand_address(const Operand& operand)
{
    if (operand.kind == Operand::Kind::Memory) {
        return operand.address;
    }
    if (operand.kind != Operand::Kind::Invalid) {
        // Registers and immediates have no address (an addressing-mode
        // exception on hardware).
        halt(HaltReason::AddressRequired, 0);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Opcode handlers: control flow
// ---------------------------------------------------------------------------

// HALT: stop until an interrupt is accepted.
void V60::op_halt()
{
    halt(HaltReason::HaltInstruction, m_opcode);
}

void V60::op_nop()
{
}

bool V60::condition_holds(Condition condition) const
{
    const bool z = flag(k_psw_z);
    const bool s = flag(k_psw_s);
    const bool ov = flag(k_psw_ov);
    const bool cy = flag(k_psw_cy);
    switch (condition) {
    case Condition::V:  return ov;
    case Condition::NV: return !ov;
    case Condition::L:  return cy;
    case Condition::NL: return !cy;
    case Condition::E:  return z;
    case Condition::NE: return !z;
    case Condition::NH: return cy || z;
    case Condition::H:  return !(cy || z);
    case Condition::N:  return s;
    case Condition::P:  return !s;
    case Condition::R:  return true;
    case Condition::LT: return s != ov;
    case Condition::GE: return s == ov;
    case Condition::LE: return (s != ov) || z;
    case Condition::GT: return !((s != ov) || z);
    }
    return false;
}

// Bcc / BR: branch relative to the opcode address if the condition holds.
void V60::op_branch(Condition condition, bool long_displacement)
{
    const uint32_t displacement = fetch_displacement(long_displacement ? 2 : 1);
    if (condition_holds(condition)) {
        m_pc = m_instruction_pc + displacement;
    }
}

// DBcc Rn, disp16 (0xC6 / 0xC7): byte 1 = ccc rrrrr. Rn = Rn - 1; branch
// (relative to the opcode address) if Rn != 0 and the condition holds.
// 0xC6 conditions: V L E NH N R LT LE; 0xC7: NV NL NE H P (TB) GE GT -
// the same codes as Bcc, 2 * ccc (+1 for 0xC7). VR boot at 0xFE146C:
// DBR R2, -3 (a fill loop).
// 0xC7 with ccc = 5 is TB Rn, disp16: branch if Rn == 0, no decrement.
// VR boot at 0xFFE58F ends a table-driven copy loop on a zero entry with it.
void V60::op_dbcc(bool negated_group)
{
    const uint8_t spec = fetch_byte();
    const uint32_t ccc = spec >> 5;
    const uint32_t reg = spec & 0x1Fu;
    const uint32_t displacement = fetch_displacement(2);
    if (negated_group && ccc == 5) { // TB
        if (m_reg[reg] == 0) {
            m_pc = m_instruction_pc + displacement;
        }
        return;
    }
    const auto condition = static_cast<Condition>(ccc * 2 + (negated_group ? 1 : 0));
    --m_reg[reg];
    if (m_reg[reg] != 0 && condition_holds(condition)) {
        m_pc = m_instruction_pc + displacement;
    }
}

// JMP: jump to the effective address of the operand.
void V60::op_jmp(bool mode_m)
{
    const Operand target = decode_operand(mode_m, OperandSize::Byte);
    if (const std::optional<uint32_t> address = operand_address(target)) {
        m_pc = *address;
    }
}

// ---------------------------------------------------------------------------
// Bit fields (0x5D, Format VII-b / VII-c)
// ---------------------------------------------------------------------------

V60::Operand V60::decode_bit_operand(bool mode_m)
{
    m_bit_addressing = true;
    Operand operand = decode_operand(mode_m, OperandSize::Word);
    m_bit_addressing = false;
    if (operand.kind == Operand::Kind::Register || operand.kind == Operand::Kind::Immediate) {
        halt(HaltReason::AddressRequired, 0);
        return Operand{};
    }
    return operand;
}

bool V60::fetch_field_length(uint32_t& length)
{
    const uint8_t spec = fetch_byte();
    length = (spec & 0x80) != 0 ? m_reg[spec & 0x1F] : spec;
    if (length == 0 || length > 32) {
        std::cerr << "[V60] CRITICAL: bit field length " << length << " outside 1-32 at PC="
                  << Hex{m_instruction_pc} << '\n';
        halt(HaltReason::UnimplementedOperand, spec);
        return false;
    }
    return true;
}

// A field of `length` bits starting `bit_offset` bits after `address` (the
// offset may be negative); bits are numbered from bit 0 of each byte, lowest
// address first. Only the bytes the field spans (up to 5) are accessed.
uint32_t V60::read_field(const Operand& at, uint32_t length)
{
    const uint32_t first = at.address + static_cast<uint32_t>(at.bit_offset >> 3); // floor division
    const uint32_t shift = static_cast<uint32_t>(at.bit_offset) & 7;
    const uint32_t bytes = (shift + length + 7) / 8;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
        bits |= static_cast<uint64_t>(read_data(first + i, OperandSize::Byte)) << (8 * i);
    }
    return static_cast<uint32_t>((bits >> shift) & ((uint64_t{1} << length) - 1));
}

void V60::write_field(const Operand& at, uint32_t length, uint32_t field)
{
    const uint32_t first = at.address + static_cast<uint32_t>(at.bit_offset >> 3);
    const uint32_t shift = static_cast<uint32_t>(at.bit_offset) & 7;
    const uint32_t bytes = (shift + length + 7) / 8;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
        bits |= static_cast<uint64_t>(read_data(first + i, OperandSize::Byte)) << (8 * i);
    }
    const uint64_t mask = ((uint64_t{1} << length) - 1) << shift;
    bits = (bits & ~mask) | ((static_cast<uint64_t>(field) << shift) & mask);
    for (uint32_t i = 0; i < bytes; ++i) {
        write_data(first + i, OperandSize::Byte, static_cast<uint8_t>(bits >> (8 * i)));
    }
}

// 0x5D sub-opcode byte: bit 6 = mode bit of the first operand, bit 5 = of
// the second, bits 4-0 select the operation (as in MAME's op5D table):
//   0x08 EXTBFS src, len, dst   dst = field, sign-extended to 32 bits
//   0x09 EXTBFZ src, len, dst   dst = field, zero-extended
//   0x0A EXTBFL src, len, dst   dst = field << (32 - len) (left-justified)
//   0x18 INSBFR src, dst, len   field = low `len` bits of src
//   0x19 INSBFL src, dst, len   field = top `len` bits of src
// The field operand uses bit addressing; the other operand is a 32-bit
// value. Flags are not affected. VR at 0xFFE0E0: EXTBFL /0x501480, #11, R1.
void V60::op_bit_field()
{
    const uint8_t sub_opcode = fetch_byte();
    const bool mode_m1 = (sub_opcode & 0x40) != 0;
    const bool mode_m2 = (sub_opcode & 0x20) != 0;
    const uint32_t operation = sub_opcode & 0x1Fu;
    uint32_t length = 0;

    switch (operation) {
    case 0x08:
    case 0x09:
    case 0x0A: {
        const Operand source = decode_bit_operand(mode_m1);
        if (source.kind == Operand::Kind::Invalid || !fetch_field_length(length)) {
            return;
        }
        const Operand destination = decode_operand(mode_m2, OperandSize::Word);
        if (destination.kind == Operand::Kind::Invalid) {
            return;
        }
        uint32_t value = read_field(source, length);
        if (operation == 0x08) {
            value = static_cast<uint32_t>(sign_extend_bits(value, length));
        } else if (operation == 0x0A) {
            value = length == 32 ? value : value << (32 - length);
        }
        write_operand(destination, OperandSize::Word, value);
        return;
    }
    case 0x18:
    case 0x19: {
        const Operand source = decode_operand(mode_m1, OperandSize::Word);
        if (source.kind == Operand::Kind::Invalid) {
            return;
        }
        const uint32_t value = read_operand(source, OperandSize::Word);
        const Operand destination = decode_bit_operand(mode_m2);
        if (destination.kind == Operand::Kind::Invalid || !fetch_field_length(length)) {
            return;
        }
        const uint32_t field = operation == 0x19 && length < 32 ? value >> (32 - length) : value;
        write_field(destination, length, field);
        return;
    }
    default:
        halt(HaltReason::UnimplementedOpcode, m_opcode); // the sub-opcode follows on the next line
        std::cerr << "[V60] (bit-field instruction 0x5D, sub-opcode " << Hex{operation, 2} << ")\n";
        return;
    }
}

// Bit strings (0x5B, as MAME's op7a.hxx): operand 1 is a bit address, then
// a length byte (bit 7 set = the length is in register bits 4-0), then
// operand 2. Bits are numbered from bit 0 of the byte at the address
// upward; bit offsets may run past the byte.
//   0x00 / 0x02  SCH0BSU / SCH1BSU src, len, dst: dst (word) = index of the
//                first 0 / 1 bit, or len if none (Z set then)
//   0x08 / 0x09  MOVBSU / MOVBSD src, len, dst: copy len bits, upward from
//                the first bit / downward from the last (overlap-safe)
// R28 (and R27 for the moves) hold the current source (destination) byte
// address as the string is processed. VF at 0xFFAFA5: SCH1BSU.
void V60::op_bit_string()
{
    const uint8_t sub_opcode = fetch_byte();
    const bool mode_m1 = (sub_opcode & 0x40) != 0;
    const bool mode_m2 = (sub_opcode & 0x20) != 0;
    const uint32_t operation = sub_opcode & 0x1Fu;
    if (operation != 0x00 && operation != 0x02 && operation != 0x08 && operation != 0x09) {
        halt(HaltReason::UnimplementedOpcode, m_opcode); // the sub-opcode follows on the next line
        std::cerr << "[V60] (bit-string instruction 0x5B, sub-opcode " << Hex{operation, 2} << ")\n";
        return;
    }
    const Operand source = decode_bit_operand(mode_m1);
    if (source.kind != Operand::Kind::Memory) {
        if (source.kind != Operand::Kind::Invalid) {
            halt(HaltReason::UnimplementedOperand, m_opcode);
        }
        return;
    }
    const uint8_t spec = fetch_byte();
    const uint32_t length = (spec & 0x80) != 0 ? m_reg[spec & 0x1F] : spec;
    // Byte address and bit (0-7) of a bit offset from an operand's address.
    auto locate = [](const Operand& operand, int64_t bit) {
        const int64_t absolute = static_cast<int64_t>(operand.bit_offset) + bit;
        return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(operand.address + static_cast<uint32_t>(absolute >> 3)),
                                             static_cast<uint32_t>(absolute & 7)};
    };

    if (operation == 0x00 || operation == 0x02) {
        const Operand destination = decode_operand(mode_m2, OperandSize::Word);
        if (destination.kind == Operand::Kind::Invalid) {
            return;
        }
        const uint32_t wanted = operation == 0x02 ? 1 : 0;
        uint32_t index = 0;
        for (; index < length; ++index) {
            const auto [address, bit] = locate(source, index);
            m_reg[28] = address;
            if (((read_data(address, OperandSize::Byte) >> bit) & 1) == wanted) {
                break;
            }
        }
        set_flag(k_psw_z, index == length);
        write_operand(destination, OperandSize::Word, index);
        return;
    }

    const Operand destination = decode_bit_operand(mode_m2);
    if (destination.kind != Operand::Kind::Memory) {
        if (destination.kind != Operand::Kind::Invalid) {
            halt(HaltReason::UnimplementedOperand, m_opcode);
        }
        return;
    }
    const bool downward = operation == 0x09;
    for (uint32_t k = 0; k < length; ++k) {
        const int64_t bit_index = downward ? static_cast<int64_t>(length) - 1 - k : k;
        const auto [from, from_bit] = locate(source, bit_index);
        const auto [to, to_bit] = locate(destination, bit_index);
        m_reg[28] = from;
        m_reg[27] = to;
        const uint32_t value = (read_data(from, OperandSize::Byte) >> from_bit) & 1;
        const uint32_t old = read_data(to, OperandSize::Byte);
        write_data(to, OperandSize::Byte, (old & ~(1u << to_bit)) | (value << to_bit));
    }
}

// SETF cond, dst: dst (byte) = 1 if condition `cond` (low 4 bits of the
// first operand) holds, else 0. Codes as Bcc: 0 V, 1 NV, 2 L, 3 NL, 4 E,
// 5 NE, 6 NH, 7 H, 8 N, 9 P, 10 always, 11 never, 12 LT, 13 GE, 14 LE,
// 15 GT. Flags are not affected. VR at 0xFE09BE: CMP.B #1, 0x14[R10] then
// SETF #E, /0x40DD20 stores the comparison's result.
void V60::op_setf()
{
    const OperandPair operands = decode_format12(OperandSize::Byte, OperandSize::Byte);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t code = read_operand(operands.source, OperandSize::Byte) & 0xFu;
    const bool holds = code == 0xB ? false : condition_holds(static_cast<Condition>(code));
    write_operand(operands.destination, OperandSize::Byte, holds ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// Single-precision floating point (0x5C / 0x5F, as in MAME's op2.hxx)
// ---------------------------------------------------------------------------

// Sub-opcode byte: bit 6 = mode bit of the first operand, bit 5 = of the
// second, bits 4-0 select the operation. Operands are 32-bit IEEE single
// floats (in a register or memory), except SCLF's first operand (16-bit
// integer) and CVTWS's source (32-bit integer).
//   0x5C 00  CMPF src1, src2   Z = (src2 == src1), S = (src2 < src1), OV = CY = 0
//   0x5C 08  MOVF.S src, dst   copy, flags unchanged
//   0x5C 09  NEGF.S src, dst   dst = -src
//   0x5C 0A  ABSF.S src, dst   dst = |src|
//   0x5C 10  SCLF.S n, dst     dst = dst * 2^n
//   0x5C 18-1B  ADDF / SUBF / MULF / DIVF.S src, dst   dst = dst op src
//   0x5F 00  CVTWS src, dst    integer -> float
//   0x5F 01  CVTSW src, dst    float -> integer, rounded as TKCW bits 2-0
//                              select (0 nearest, 1 down, 2 up, else toward 0)
// Arithmetic results set Z (+/-0) and S (sign bit), clear OV and CY, as in
// MAME. Differences from MAME, all toward IEEE 754 (which the V60 FPU
// follows): CVTSW rounds to nearest-even (MAME: half away from zero) and
// sets OV for out-of-range and NaN values (result 0x80000000); SCLF uses an
// exact power of two for any n; CMPF compares instead of subtracting (so
// infinities compare correctly); Z is set for -0 too. FPU exceptions
// (division by zero, overflow...) are not emulated: results are IEEE values.
// VR at 0xFED52B: CVTSW R5, R5.
void V60::op_float(bool convert_group)
{
    const uint8_t sub_opcode = fetch_byte();
    const bool mode_m1 = (sub_opcode & 0x40) != 0;
    const bool mode_m2 = (sub_opcode & 0x20) != 0;
    const uint32_t operation = sub_opcode & 0x1Fu;

    const bool known = convert_group ? operation <= 0x01
                                     : (operation == 0x00 || (operation >= 0x08 && operation <= 0x0A)
                                        || operation == 0x10 || (operation >= 0x18 && operation <= 0x1B));
    if (!known) {
        halt(HaltReason::UnimplementedOpcode, m_opcode); // the sub-opcode follows on the next line
        std::cerr << "[V60] (floating-point instruction " << Hex{m_opcode, 2} << ", sub-opcode "
                  << Hex{operation, 2} << ")\n";
        return;
    }

    const bool scale = !convert_group && operation == 0x10;
    const OperandSize source_size = scale ? OperandSize::Half : OperandSize::Word;
    const Operand source = decode_operand(mode_m1, source_size);
    if (source.kind == Operand::Kind::Invalid) {
        return;
    }
    const Operand destination = decode_operand(mode_m2, OperandSize::Word);
    if (destination.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t src_bits = read_operand(source, source_size);
    auto set_float_flags = [this](float value) {
        set_flag(k_psw_z, value == 0.0f);
        set_flag(k_psw_s, (std::bit_cast<uint32_t>(value) & 0x80000000u) != 0);
        set_flag(k_psw_ov, false);
        set_flag(k_psw_cy, false);
    };

    if (convert_group) {
        if (operation == 0x00) { // CVTWS
            const auto value = static_cast<float>(static_cast<int32_t>(src_bits));
            if (write_operand(destination, OperandSize::Word, std::bit_cast<uint32_t>(value))) {
                set_float_flags(value);
                set_flag(k_psw_cy, value < 0.0f); // as in MAME
            }
            return;
        }
        // CVTSW
        const double value = std::bit_cast<float>(src_bits);
        double rounded = 0;
        switch (m_privileged_other[8] & 7) { // TKCW rounding control
        case 0:  rounded = std::nearbyint(value); break; // default mode: nearest, ties to even
        case 1:  rounded = std::floor(value); break;
        case 2:  rounded = std::ceil(value); break;
        default: rounded = std::trunc(value); break;
        }
        const bool overflow = !(rounded >= -2147483648.0 && rounded <= 2147483647.0); // also NaN
        const uint32_t result = overflow ? 0x80000000u : static_cast<uint32_t>(static_cast<int32_t>(rounded));
        if (write_operand(destination, OperandSize::Word, result)) {
            set_flag(k_psw_z, result == 0);
            set_flag(k_psw_s, (result & 0x80000000u) != 0);
            set_flag(k_psw_ov, overflow);
        }
        return;
    }

    const float src = std::bit_cast<float>(src_bits);
    switch (operation) {
    case 0x00: { // CMPF: both operands are only read
        const float second = std::bit_cast<float>(read_operand(destination, OperandSize::Word));
        set_flag(k_psw_z, second == src);
        set_flag(k_psw_s, second < src);
        set_flag(k_psw_ov, false);
        set_flag(k_psw_cy, false);
        return;
    }
    case 0x08: // MOVF.S
        write_operand(destination, OperandSize::Word, src_bits);
        return;
    case 0x09:   // NEGF.S
    case 0x0A: { // ABSF.S
        const float value = operation == 0x09 ? -src : std::fabs(src);
        if (write_operand(destination, OperandSize::Word, std::bit_cast<uint32_t>(value))) {
            set_float_flags(value);
        }
        return;
    }
    default:
        break;
    }

    // SCLF / ADDF / SUBF / MULF / DIVF: dst = dst op src.
    const float dst = std::bit_cast<float>(read_operand(destination, OperandSize::Word));
    float value = 0;
    switch (operation) {
    case 0x10: value = std::ldexp(dst, static_cast<int16_t>(src_bits)); break;
    case 0x18: value = dst + src; break;
    case 0x19: value = dst - src; break;
    case 0x1A: value = dst * src; break;
    default:   value = dst / src; break;
    }
    if (write_operand(destination, OperandSize::Word, std::bit_cast<uint32_t>(value))) {
        set_float_flags(value);
    }
}

// XCH op1, op2: swaps the two operands (registers or memory). Flags are not
// affected (as in MAME). VR at 0xFFA6E9: XCH.W R0, R26.
void V60::op_exchange(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t first = read_operand(operands.source, size);
    const uint32_t second = read_operand(operands.destination, size);
    if (write_operand(operands.source, size, second)) {
        write_operand(operands.destination, size, first);
    }
}

// JSR target (one operand): push the address of the next instruction (after
// the operand's bytes), then jump to the operand's effective address.
// Returns with RSR, like BSR. VR boot at 0xFE01E7: JSR disp32[PC].
void V60::op_jsr(bool mode_m)
{
    const Operand target = decode_operand(mode_m, OperandSize::Byte);
    if (const std::optional<uint32_t> address = operand_address(target)) {
        push_long(m_pc);
        m_pc = *address;
    }
}

// CALL target, args: push AP, AP = address of args, push return PC, jump.
void V60::op_call()
{
    const OperandPair operands = decode_format12(OperandSize::Byte, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const std::optional<uint32_t> target = operand_address(operands.source);
    if (!target) {
        return;
    }
    const std::optional<uint32_t> arguments = operand_address(operands.destination);
    if (!arguments) {
        return;
    }
    push_long(m_reg[k_reg_ap]);
    m_reg[k_reg_ap] = *arguments;
    push_long(m_pc); // address of the next instruction
    m_pc = *target;
}

// RET #frame: pop PC, pop AP, then discard `frame` bytes of arguments.
void V60::op_ret(bool mode_m)
{
    const Operand frame = decode_operand(mode_m, OperandSize::Word);
    if (frame.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t frame_size = read_operand(frame, OperandSize::Word);
    m_pc = pop_long();
    m_reg[k_reg_ap] = pop_long();
    m_reg[k_reg_sp] += frame_size;
}

// BSR disp16: push return PC, branch relative to the opcode address.
void V60::op_bsr()
{
    const uint32_t displacement = fetch_displacement(2);
    push_long(m_pc);
    m_pc = m_instruction_pc + displacement;
}

// RSR: return from BSR.
void V60::op_rsr()
{
    m_pc = pop_long();
}

// RETIS #frame: return from interrupt. Pops PC, then PSW, discards `frame`
// extra bytes of stack (16-bit operand), then restores the PSW, which swaps
// back to the interrupted code's stack and IE state.
void V60::op_retis(bool mode_m)
{
    const Operand frame = decode_operand(mode_m, OperandSize::Half);
    if (frame.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t frame_size = read_operand(frame, OperandSize::Half);

    const uint32_t return_pc = pop_long();
    const uint32_t saved_psw = pop_long();
    m_reg[k_reg_sp] += frame_size;
    write_psw(saved_psw);
    m_pc = return_pc;
}

// ---------------------------------------------------------------------------
// Opcode handlers: stack
// ---------------------------------------------------------------------------

// PUSH src: SP -= 4, [SP] = 32-bit value.
void V60::op_push(bool mode_m)
{
    const Operand source = decode_operand(mode_m, OperandSize::Word);
    if (source.kind == Operand::Kind::Invalid) {
        return;
    }
    push_long(read_operand(source, OperandSize::Word));
}

// POP dst: value = [SP], SP += 4, then dst = value. The stack is popped
// before the destination is decoded, so SP-relative destinations see the
// updated SP.
void V60::op_pop(bool mode_m)
{
    const uint32_t value = pop_long();
    const Operand destination = decode_operand(mode_m, OperandSize::Word);
    if (destination.kind == Operand::Kind::Invalid) {
        return;
    }
    write_operand(destination, OperandSize::Word, value);
}

// PUSHM list: bit 31 = PSW, bits 30-0 = R30-R0. Pushes PSW first, then
// registers from R30 down to R0, so R0 ends up at the lowest address.
void V60::op_pushm(bool mode_m)
{
    const Operand list_operand = decode_operand(mode_m, OperandSize::Word);
    if (list_operand.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t list = read_operand(list_operand, OperandSize::Word);
    if ((list & 0x80000000u) != 0) {
        push_long(m_psw);
    }
    for (int r = 30; r >= 0; --r) {
        if ((list & (1u << r)) != 0) {
            push_long(m_reg[static_cast<std::size_t>(r)]);
        }
    }
}

// POPM list: the mirror of PUSHM. Pops R0 up to R30, then (bit 31) the low
// 16 bits of the PSW (flags); the upper PSW bits are kept.
void V60::op_popm(bool mode_m)
{
    const Operand list_operand = decode_operand(mode_m, OperandSize::Word);
    if (list_operand.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t list = read_operand(list_operand, OperandSize::Word);
    for (int r = 0; r <= 30; ++r) {
        if ((list & (1u << r)) != 0) {
            m_reg[static_cast<std::size_t>(r)] = pop_long();
        }
    }
    if ((list & 0x80000000u) != 0) {
        const uint32_t low_psw = pop_long() & 0xFFFFu;
        write_psw((m_psw & 0xFFFF0000u) | low_psw);
    }
}

// ---------------------------------------------------------------------------
// Opcode handlers: arithmetic and logic
// ---------------------------------------------------------------------------

// MOV.B / MOV.H / MOV.W src, dst: copy 8 / 16 / 32 bits. Flags are not
// affected; a byte or halfword written to a register replaces only its low
// bits.
void V60::op_mov(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    write_operand(operands.destination, size, read_operand(operands.source, size));
}

// MOVD src, dst: 64-bit move (as MAME's opMOVD). Each operand is a register
// pair (Rn = low word, Rn+1 = high word) or 8 bytes of memory (low word
// first). Flags are not affected. VF at 0xFE4B37: MOVD /0x40BFF6, R1.
void V60::op_move_double()
{
    const OperandPair operands = decode_format12(OperandSize::Quad, OperandSize::Quad);
    if (!require_valid(operands)) {
        return;
    }
    for (const Operand* operand : {&operands.source, &operands.destination}) {
        if (operand->kind == Operand::Kind::Register && operand->reg == 31) {
            halt(HaltReason::InvalidRegisterPair, 0);
            return;
        }
    }
    if (operands.destination.kind == Operand::Kind::Immediate) {
        halt(HaltReason::ImmediateDestination, 0);
        return;
    }
    const Operand& src = operands.source;
    const uint32_t low = src.kind == Operand::Kind::Register ? m_reg[src.reg] : read_data_long(src.address);
    const uint32_t high = src.kind == Operand::Kind::Register ? m_reg[src.reg + 1u] : read_data_long(src.address + 4);
    const Operand& dst = operands.destination;
    if (dst.kind == Operand::Kind::Register) {
        m_reg[dst.reg] = low;
        m_reg[dst.reg + 1u] = high;
    } else {
        write_data_long(dst.address, low);
        write_data_long(dst.address + 4, high);
    }
}

// Decimal (BCD) group 0x59, as MAME's op7a.hxx: sub-opcode, source,
// destination, then a pattern byte (bit 7 set = the value of register
// bits 4-0).
//   0x00 ADDDC src, dst: dst = dst + src + CY (2-digit packed BCD bytes)
//   0x01 SUBDC src, dst: dst = dst - src - CY
//   0x02 SUBRDC src, dst: dst = src - dst - CY
//        CY = decimal carry / borrow; Z cleared if the result or the carry
//        is non-zero, else unchanged (multi-byte arithmetic).
//   0x10 CVTDPZ src (byte), dst (half): packed to unpacked (zoned): the
//        two digits in the low nibbles of two bytes, each ORed with the
//        pattern; Z cleared if src is non-zero.
//   0x18 CVTDZP src (half), dst (byte): unpacked to packed.
void V60::op_decimal()
{
    const uint8_t sub_opcode = fetch_byte();
    const bool mode_m1 = (sub_opcode & 0x40) != 0;
    const bool mode_m2 = (sub_opcode & 0x20) != 0;
    const uint32_t operation = sub_opcode & 0x1Fu;
    if (operation != 0x00 && operation != 0x01 && operation != 0x02 && operation != 0x10 && operation != 0x18) {
        halt(HaltReason::UnimplementedOpcode, m_opcode); // the sub-opcode follows on the next line
        std::cerr << "[V60] (decimal instruction 0x59, sub-opcode " << Hex{operation, 2} << ")\n";
        return;
    }
    const OperandSize source_size = operation == 0x18 ? OperandSize::Half : OperandSize::Byte;
    const OperandSize destination_size = operation == 0x10 ? OperandSize::Half : OperandSize::Byte;
    const Operand source = decode_operand(mode_m1, source_size);
    if (source.kind == Operand::Kind::Invalid) {
        return;
    }
    const Operand destination = decode_operand(mode_m2, destination_size);
    if (destination.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint8_t spec = fetch_byte();
    const uint32_t pattern = (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : spec;
    const uint32_t src = read_operand(source, source_size);

    if (operation == 0x10) { // CVTDPZ
        const uint32_t zoned = (((src >> 4) & 0xF) | ((src & 0xF) << 8) | pattern | (pattern << 8)) & 0xFFFF;
        if ((src & 0xFF) != 0) {
            set_flag(k_psw_z, false);
        }
        write_operand(destination, OperandSize::Half, zoned);
        return;
    }
    if (operation == 0x18) { // CVTDZP
        const uint32_t packed = ((src >> 8) & 0xF) | ((src & 0xF) << 4);
        if (packed != 0) {
            set_flag(k_psw_z, false);
        }
        write_operand(destination, OperandSize::Byte, packed);
        return;
    }
    const uint32_t dst = read_operand(destination, OperandSize::Byte);
    const int a = static_cast<int>((src >> 4) & 0xF) * 10 + static_cast<int>(src & 0xF);
    const int b = static_cast<int>((dst >> 4) & 0xF) * 10 + static_cast<int>(dst & 0xF);
    const int carry = flag(k_psw_cy) ? 1 : 0;
    int result = operation == 0x00 ? b + a + carry : (operation == 0x01 ? b - a - carry : a - b - carry);
    bool carry_out = false;
    if (operation == 0x00 && result >= 100) {
        result -= 100;
        carry_out = true;
    } else if (operation != 0x00 && result < 0) {
        result += 100;
        carry_out = true;
    }
    set_flag(k_psw_cy, carry_out);
    if (result != 0 || carry_out) {
        set_flag(k_psw_z, false);
    }
    write_operand(destination, OperandSize::Byte, static_cast<uint32_t>(((result / 10) << 4) | (result % 10)));
}

// RVBIT src, dst (bytes): dst = src with its 8 bits in reverse order.
// RVBYT src, dst (words): dst = src with its 4 bytes in reverse order.
// Flags are not affected (as MAME).
void V60::op_reverse(bool bits)
{
    const OperandSize size = bits ? OperandSize::Byte : OperandSize::Word;
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    uint32_t value = read_operand(operands.source, size);
    if (bits) {
        uint32_t reversed = 0;
        for (int i = 0; i < 8; ++i) {
            reversed |= ((value >> i) & 1u) << (7 - i);
        }
        value = reversed;
    } else {
        value = (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
    }
    write_operand(operands.destination, size, value);
}

// PREPARE size: push FP, FP = SP, SP -= size (a word operand): a new stack
// frame of `size` bytes of locals. DISPOSE: SP = FP, pop FP. Flags are not
// affected.
void V60::op_prepare(bool mode_m)
{
    const Operand operand = decode_operand(mode_m, OperandSize::Word);
    if (operand.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t size = read_operand(operand, OperandSize::Word);
    push_long(m_reg[k_reg_fp]);
    m_reg[k_reg_fp] = m_reg[k_reg_sp];
    m_reg[k_reg_sp] -= size;
}

void V60::op_dispose()
{
    m_reg[k_reg_sp] = m_reg[k_reg_fp];
    m_reg[k_reg_fp] = pop_long();
}

// TASI dst (byte): test and set. Flags as SUB dst, #0xFF (Z set if dst was
// 0xFF), then dst = 0xFF.
void V60::op_tasi(bool mode_m)
{
    const Operand operand = decode_operand(mode_m, OperandSize::Byte);
    if (operand.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t value = read_operand(operand, OperandSize::Byte);
    add_sub_flags(value, 0xFF, false, true, OperandSize::Byte);
    write_operand(operand, OperandSize::Byte, 0xFF);
}

// GETPSW dst: dst (word) = PSW.
void V60::op_getpsw(bool mode_m)
{
    const Operand operand = decode_operand(mode_m, OperandSize::Word);
    if (operand.kind == Operand::Kind::Invalid) {
        return;
    }
    write_operand(operand, OperandSize::Word, m_psw);
}

// MOVS / MOVZ src, dst: move a byte or halfword into a larger destination,
// sign- or zero-extended (.BH byte -> half, .BW byte -> word, .HW half ->
// word). Flags are not affected. VR boot at 0xFE0264: MOVZ.HW #0x5008, R1.
void V60::op_move_extend(OperandSize source_size, OperandSize destination_size, bool sign_extend_value)
{
    const OperandPair operands = decode_format12(source_size, destination_size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t bytes = bytes_of(source_size);
    const uint32_t value = read_operand(operands.source, source_size);
    const uint32_t extended = sign_extend_value ? static_cast<uint32_t>(sign_extend(value, bytes))
                                                : value & size_mask(bytes);
    write_operand(operands.destination, destination_size, extended);
}

// MOVT src, dst: keep the low bits of a halfword / word (.HB / .WB / .WH).
// OV is set if the value does not fit the smaller size as a signed number
// (the dropped bits are not all copies of the result's sign bit); the other
// flags are unchanged (as in MAME).
void V60::op_move_truncate(OperandSize source_size, OperandSize destination_size)
{
    const OperandPair operands = decode_format12(source_size, destination_size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t value = read_operand(operands.source, source_size);
    const uint32_t bytes = bytes_of(destination_size);
    const uint32_t truncated = value & size_mask(bytes);
    const bool fits = sign_extend(truncated, bytes) == sign_extend(value, bytes_of(source_size));
    if (write_operand(operands.destination, destination_size, truncated)) {
        set_flag(k_psw_ov, !fits);
    }
}

// ADD / SUB src, dst: dst = dst + src or dst - src (CY = carry out, or
// borrow). Byte and halfword forms write only the low bits of a register.
// Sets Z, S, OV, CY.
void V60::op_add_sub(bool subtract, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);
    const uint32_t saved_psw = m_psw;
    const uint32_t result = add_sub_flags(dst, src, false, subtract, size);
    if (!write_operand(operands.destination, size, result)) {
        m_psw = saved_psw; // the CPU halted: leave the flags as they were
    }
}

// dst + src + carry_in, or dst - src - carry_in, at the operand size.
// Sets Z, S, OV and CY (carry out, or borrow for subtraction) and returns
// the result; the caller decides whether to write it.
uint32_t V60::add_sub_flags(uint32_t dst, uint32_t src, bool carry_in, bool subtract, OperandSize size)
{
    const uint32_t bytes = bytes_of(size);
    const uint32_t mask = size_mask(bytes);
    const uint64_t d = dst & mask;
    const uint64_t s = src & mask;
    const uint64_t c = carry_in ? 1 : 0;
    const uint64_t wide = subtract ? d - s - c : d + s + c;
    const auto result = static_cast<uint32_t>(wide) & mask;

    set_zs_flags(result, size);
    // Carry out of / borrow into the top bit: both show up above the mask.
    set_flag(k_psw_cy, (wide & ~static_cast<uint64_t>(mask)) != 0);
    const auto d32 = static_cast<uint32_t>(d);
    const auto s32 = static_cast<uint32_t>(s);
    const uint32_t overflow = subtract ? (d32 ^ s32) & (d32 ^ result) : (result ^ s32) & (result ^ d32);
    set_flag(k_psw_ov, (overflow & sign_bit(bytes)) != 0);
    return result;
}

// ADDC / SUBC src, dst: dst = dst + src + CY, or dst - src - CY (CY is the
// borrow). Used to chain multi-word arithmetic and to round after a right
// shift (VR boot: SHL.W #-2, R3 then ADDC.W #0, R3). Sets Z, S, OV, CY.
void V60::op_add_sub_carry(bool subtract, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);
    const uint32_t saved_psw = m_psw;
    const uint32_t result = add_sub_flags(dst, src, flag(k_psw_cy), subtract, size);
    if (!write_operand(operands.destination, size, result)) {
        m_psw = saved_psw; // the CPU halted: leave the flags as they were
    }
}

// CMP src1, src2: sets Z, S, OV, CY from src2 - src1 (CY = borrow, i.e.
// src2 < src1 unsigned). Neither operand is written, so both may be
// immediates. VR boot at 0xFE0134: CMP.H #0x100, R3 followed by a branch.
void V60::op_cmp(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t first = read_operand(operands.source, size);
    const uint32_t second = read_operand(operands.destination, size);
    add_sub_flags(second, first, false, true, size);
}

// AND / OR / XOR src, dst: dst = dst op src. Sets Z, S; clears OV; CY kept.
void V60::op_logic(LogicOp op, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);
    uint32_t result = 0;
    switch (op) {
    case LogicOp::And: result = dst & src; break;
    case LogicOp::Or:  result = dst | src; break;
    case LogicOp::Xor: result = dst ^ src; break;
    }
    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_ov, false);
}

// NEG src, dst: dst = 0 - src. Flags as SUB: Z, S, OV (negating the most
// negative value), CY = borrow, i.e. set unless src is 0 (as in MAME).
// VR at 0xFE6793: BGE +5 ; NEG.W R0, R0 - an absolute value.
void V60::op_neg(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t saved_psw = m_psw;
    const uint32_t result = add_sub_flags(0, src, false, true, size);
    if (!write_operand(operands.destination, size, result)) {
        m_psw = saved_psw;
    }
}

// NOT src, dst: dst = ~src. Sets Z, S; clears OV; CY kept.
void V60::op_not(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t result = ~read_operand(operands.source, size) & size_mask(bytes_of(size));
    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_ov, false);
}

// ROT / ROTC count, dst. The count is a signed byte: positive rotates left,
// negative rotates right. ROT feeds each bit leaving one end into the other;
// ROTC rotates through CY (a size + 1 bit rotation). CY receives the last
// bit rotated out; OV is cleared; Z and S reflect the result. A count of 0
// leaves the value unchanged; ROT then clears CY, ROTC keeps it (CY is part
// of the rotated value) - an assumption not checked against NEC's manual.
// VR boot at 0xFE0E7E: ROT.H #8, R0 (byte swap of the low halfword).
void V60::op_rotate(bool through_carry, OperandSize size)
{
    const OperandPair operands = decode_format12(OperandSize::Byte, size);
    if (!require_valid(operands)) {
        return;
    }
    const auto count = static_cast<int8_t>(read_operand(operands.source, OperandSize::Byte));
    const uint32_t bytes = bytes_of(size);
    const uint32_t mask = size_mask(bytes);
    const uint32_t msb = sign_bit(bytes);
    uint32_t value = read_operand(operands.destination, size);

    bool carry = through_carry && flag(k_psw_cy);
    const uint32_t steps = count < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(count))
                                     : static_cast<uint32_t>(count);
    for (uint32_t i = 0; i < steps; ++i) {
        if (count > 0) {
            const bool out = (value & msb) != 0;
            const uint32_t in_bit = through_carry ? (carry ? 1u : 0u) : (out ? 1u : 0u);
            value = ((value << 1) | in_bit) & mask;
            carry = out;
        } else {
            const bool out = (value & 1) != 0;
            const uint32_t in_bit = through_carry ? (carry ? msb : 0u) : (out ? msb : 0u);
            value = (value >> 1) | in_bit;
            carry = out;
        }
    }

    if (!write_operand(operands.destination, size, value)) {
        return;
    }
    set_zs_flags(value, size);
    set_flag(k_psw_cy, carry);
    set_flag(k_psw_ov, false);
}

// SHL / SHA count, dst. The count is a signed byte: positive shifts left,
// negative shifts right. SHL shifts in zeros; SHA's right shift copies the
// sign bit. CY receives the last bit shifted out; OV is cleared, except for
// SHA left shifts, where it reports a result that does not fit (value
// changed sign or lost significant bits). A count of 0 leaves the value
// unchanged and clears CY and OV. Z and S always reflect the result.
void V60::op_shift(ShiftKind kind, OperandSize size)
{
    const OperandPair operands = decode_format12(OperandSize::Byte, size);
    if (!require_valid(operands)) {
        return;
    }
    const auto count = static_cast<int8_t>(read_operand(operands.source, OperandSize::Byte));
    const uint32_t bytes = bytes_of(size);
    const uint32_t bits = bytes * 8;
    const uint32_t mask = size_mask(bytes);
    const uint32_t value = read_operand(operands.destination, size);

    uint32_t result = value;
    bool carry = false;
    bool overflow = false;

    if (count > 0) {
        const auto n = static_cast<uint32_t>(count);
        carry = n <= bits && ((value >> (bits - n)) & 1) != 0;
        result = n >= bits ? 0 : (value << n) & mask;
        if (kind == ShiftKind::Arithmetic) {
            overflow = n >= bits ? value != 0
                                 : sign_extend(result, bytes) != sign_extend(value, bytes) * (int64_t{1} << n);
        }
    } else if (count < 0) {
        const auto n = static_cast<uint32_t>(-static_cast<int32_t>(count));
        const bool negative = (value & sign_bit(bytes)) != 0;
        if (kind == ShiftKind::Logical) {
            carry = n <= bits && ((value >> (n - 1)) & 1) != 0;
            result = n >= bits ? 0 : value >> n;
        } else {
            carry = n <= bits ? ((value >> (n - 1)) & 1) != 0 : negative;
            result = n >= bits ? (negative ? mask : 0)
                               : static_cast<uint32_t>(sign_extend(value, bytes) >> n) & mask;
        }
    }

    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_cy, carry);
    set_flag(k_psw_ov, overflow);
}

// MUL / MULU src, dst: dst = dst * src (truncated to the operand size).
// Sets Z and S from the truncated result; OV when the full product does not
// fit in the operand size (signed or unsigned range); CY unchanged.
void V60::op_mul(Signedness signedness, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t bytes = bytes_of(size);
    const uint32_t mask = size_mask(bytes);
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);

    uint32_t result = 0;
    bool overflow = false;
    if (signedness == Signedness::Signed) {
        const int64_t product = sign_extend(dst, bytes) * sign_extend(src, bytes);
        result = static_cast<uint32_t>(product) & mask;
        overflow = sign_extend(result, bytes) != product;
    } else {
        const uint64_t product = static_cast<uint64_t>(dst) * static_cast<uint64_t>(src);
        result = static_cast<uint32_t>(product) & mask;
        overflow = product > mask;
    }

    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_ov, overflow);
}

// DIV / DIVU src, dst: dst = dst / src, rounded toward zero. Sets Z and S
// from the quotient; OV for the signed overflow case (most negative value /
// -1), in which dst is left unchanged; CY unchanged.
// Division by zero raises a zero-divide exception on hardware, which is not
// emulated. Instead dst is left unchanged (as in MAME), OV is set to flag the
// failed division (our choice; MAME leaves it clear), Z and S reflect the
// unchanged dst, and an error is logged.
void V60::op_div(Signedness signedness, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t bytes = bytes_of(size);
    const uint32_t mask = size_mask(bytes);
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);

    uint32_t result = dst;
    bool overflow = false;
    if (src == 0) {
        std::cerr << "[V60] ERROR: division by zero at PC=" << Hex{m_instruction_pc}
                  << " (zero-divide exception not emulated), destination unchanged, OV set\n";
        overflow = true;
    } else if (signedness == Signedness::Signed) {
        const int64_t dividend = sign_extend(dst, bytes);
        const int64_t divisor = sign_extend(src, bytes);
        const int64_t quotient = dividend / divisor; // 64-bit: cannot overflow
        overflow = sign_extend(static_cast<uint32_t>(quotient) & mask, bytes) != quotient;
        if (!overflow) {
            result = static_cast<uint32_t>(quotient) & mask;
        }
    } else {
        result = dst / src;
    }

    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_ov, overflow);
}

// REM / REMU src, dst: dst = dst % src (signed: the remainder has the
// dividend's sign; computed in 64 bits, so most negative % -1 = 0). Sets Z
// and S from the remainder, clears OV; CY unchanged. Division by zero is
// handled like DIV: dst unchanged, OV set, error logged.
// VR boot at 0xFC3BB2: DIVU.W R2, R0 ; REMU.W R2, R1 ; BE ; INC.W R0 -
// rounds R0 up to a multiple of R2.
void V60::op_rem(Signedness signedness, OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t bytes = bytes_of(size);
    const uint32_t mask = size_mask(bytes);
    const uint32_t src = read_operand(operands.source, size);
    const uint32_t dst = read_operand(operands.destination, size);

    uint32_t result = dst;
    bool overflow = false;
    if (src == 0) {
        std::cerr << "[V60] ERROR: remainder by zero at PC=" << Hex{m_instruction_pc}
                  << " (zero-divide exception not emulated), destination unchanged, OV set\n";
        overflow = true;
    } else if (signedness == Signedness::Signed) {
        const int64_t remainder = sign_extend(dst, bytes) % sign_extend(src, bytes);
        result = static_cast<uint32_t>(remainder) & mask;
    } else {
        result = dst % src;
    }

    if (!write_operand(operands.destination, size, result)) {
        return;
    }
    set_zs_flags(result, size);
    set_flag(k_psw_ov, overflow);
}

// MULX / MULUX src, dst: 32 x 32 -> 64-bit product. dst is a register pair
// (Rn = low word, Rn+1 = high word) or an 8-byte memory operand (low word
// first); its low word is the multiplicand. Sets S from bit 63 and Z if the
// whole product is zero; OV and CY unchanged.
void V60::op_mul_extended(Signedness signedness)
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Quad);
    if (!require_valid(operands)) {
        return;
    }
    const Operand& dst = operands.destination;
    if (dst.kind == Operand::Kind::Register && dst.reg == 31) {
        halt(HaltReason::InvalidRegisterPair, 0);
        return;
    }
    if (dst.kind == Operand::Kind::Immediate) {
        halt(HaltReason::ImmediateDestination, 0);
        return;
    }
    const uint32_t src = read_operand(operands.source, OperandSize::Word);
    const uint32_t multiplicand = dst.kind == Operand::Kind::Register ? m_reg[dst.reg] : read_data_long(dst.address);

    uint64_t product = 0;
    if (signedness == Signedness::Signed) {
        product = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(multiplicand))
                                        * static_cast<int64_t>(static_cast<int32_t>(src)));
    } else {
        product = static_cast<uint64_t>(multiplicand) * static_cast<uint64_t>(src);
    }
    const auto low = static_cast<uint32_t>(product);
    const auto high = static_cast<uint32_t>(product >> 32);

    if (dst.kind == Operand::Kind::Register) {
        m_reg[dst.reg] = low;
        m_reg[dst.reg + 1u] = high;
    } else {
        write_data_long(dst.address, low);
        write_data_long(dst.address + 4, high);
    }
    set_flag(k_psw_s, (high & 0x80000000u) != 0);
    set_flag(k_psw_z, product == 0);
}

// DIVX / DIVUX src, dst: 64 / 32 division. dst is a register pair (Rn = low,
// Rn+1 = high word of the dividend) or an 8-byte memory operand. On success
// the quotient goes to the low word and the remainder to the high word;
// S and Z follow the quotient, OV is cleared. If the divisor is zero (error
// logged) or the quotient does not fit in 32 bits, dst is left unchanged and
// OV is set; MAME does not handle either case, so this behaviour is our own.
void V60::op_div_extended(Signedness signedness)
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Quad);
    if (!require_valid(operands)) {
        return;
    }
    const Operand& dst = operands.destination;
    if (dst.kind == Operand::Kind::Register && dst.reg == 31) {
        halt(HaltReason::InvalidRegisterPair, 0);
        return;
    }
    if (dst.kind == Operand::Kind::Immediate) {
        halt(HaltReason::ImmediateDestination, 0);
        return;
    }
    const uint32_t divisor = read_operand(operands.source, OperandSize::Word);
    const uint32_t low = dst.kind == Operand::Kind::Register ? m_reg[dst.reg] : read_data_long(dst.address);
    const uint32_t high = dst.kind == Operand::Kind::Register ? m_reg[dst.reg + 1u] : read_data_long(dst.address + 4);
    const uint64_t dividend = (static_cast<uint64_t>(high) << 32) | low;

    if (divisor == 0) {
        std::cerr << "[V60] ERROR: division by zero at PC=" << Hex{m_instruction_pc}
                  << " (zero-divide exception not emulated), destination unchanged, OV set\n";
        set_flag(k_psw_ov, true);
        return;
    }

    uint32_t quotient = 0;
    uint32_t remainder = 0;
    bool overflow = false;
    if (signedness == Signedness::Signed) {
        const auto signed_dividend = static_cast<int64_t>(dividend);
        const auto signed_divisor = static_cast<int64_t>(static_cast<int32_t>(divisor));
        // INT64_MIN / -1 does not fit in 64 bits either; it is an overflow.
        if (signed_dividend == INT64_MIN && signed_divisor == -1) {
            overflow = true;
        } else {
            const int64_t q = signed_dividend / signed_divisor;
            overflow = q < INT32_MIN || q > INT32_MAX;
            quotient = static_cast<uint32_t>(q);
            remainder = static_cast<uint32_t>(signed_dividend % signed_divisor);
        }
    } else {
        const uint64_t q = dividend / divisor;
        overflow = q > 0xFFFFFFFFu;
        quotient = static_cast<uint32_t>(q);
        remainder = static_cast<uint32_t>(dividend % divisor);
    }

    set_flag(k_psw_ov, overflow);
    if (overflow) {
        return;
    }
    if (dst.kind == Operand::Kind::Register) {
        m_reg[dst.reg] = quotient;
        m_reg[dst.reg + 1u] = remainder;
    } else {
        write_data_long(dst.address, quotient);
        write_data_long(dst.address + 4, remainder);
    }
    set_zs_flags_long(quotient);
}

// TEST1 / SET1 / CLR1 / NOT1 bit, dst: operate on bit (bit & 31) of a 32-bit
// word. CY receives the bit's previous value and Z its inverse; other flags
// are unchanged. TEST1 only reads dst.
void V60::op_bit(BitOp op)
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t bit = 1u << (read_operand(operands.source, OperandSize::Word) & 31);
    const uint32_t value = read_operand(operands.destination, OperandSize::Word);
    const bool was_set = (value & bit) != 0;

    uint32_t result = value;
    switch (op) {
    case BitOp::Test:   break;
    case BitOp::Set:    result = value | bit;  break;
    case BitOp::Clear:  result = value & ~bit; break;
    case BitOp::Invert: result = value ^ bit;  break;
    }
    if (op != BitOp::Test && !write_operand(operands.destination, OperandSize::Word, result)) {
        return;
    }
    set_flag(k_psw_cy, was_set);
    set_flag(k_psw_z, !was_set);
}

// UPDPSW.W / UPDPSW.H value, mask: PSW = (PSW & ~mask) | (value & mask).
// The mask is limited to the low 24 bits (.W: condition codes and control
// fields) or 16 bits (.H: condition codes), so the privileged top byte (IS,
// EL, ...) cannot be changed this way. Both operands are 32-bit reads.
void V60::op_updpsw(uint32_t field_limit)
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t value = read_operand(operands.source, OperandSize::Word) & field_limit;
    const uint32_t mask = read_operand(operands.destination, OperandSize::Word) & field_limit;
    write_psw((m_psw & ~mask) | (value & mask));
}

// MOVEA.B / .H / .W src, dst: dst (32-bit) = effective address of src.
// The size only affects how src is addressed (index scale, autoincrement
// step); flags are not affected. A register or immediate source has no
// address and halts.
void V60::op_movea(OperandSize size)
{
    const OperandPair operands = decode_format12(size, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    if (const std::optional<uint32_t> address = operand_address(operands.source)) {
        write_operand(operands.destination, OperandSize::Word, *address);
    }
}

// IN.B / .H / .W port, dst: reads the I/O-space location at the address of
// the first operand into dst. Flags are not affected.
void V60::op_in(OperandSize size)
{
    const OperandPair operands = decode_format12(size, size);
    if (!require_valid(operands)) {
        return;
    }
    const std::optional<uint32_t> port = operand_address(operands.source);
    if (!port) {
        return;
    }
    const uint32_t value = m_bus.io_space_read(*port & k_address_mask, bytes_of(size));
    write_operand(operands.destination, size, value);
}

// OUT.B / .H / .W src, port: writes src to the I/O-space location at the
// address of the second operand. Flags are not affected.
void V60::op_out(OperandSize size)
{
    const OperandPair operands = decode_format12(size, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t value = read_operand(operands.source, size);
    if (const std::optional<uint32_t> port = operand_address(operands.destination)) {
        m_bus.io_space_write(*port & k_address_mask, bytes_of(size), value);
    }
}

// String instructions: dispatch on the sub-opcode.
void V60::op_string(OperandSize size)
{
    const uint8_t sub_opcode = fetch_byte();
    switch (sub_opcode & 0x1F) {
    case 0x08: op_move_string_up(size, false, false); return; // MOVCU
    case 0x0A: op_move_string_up(size, true, false);  return; // MOVCFU
    case 0x0C: op_move_string_up(size, false, true);  return; // MOVCSU
    case 0x09: op_move_string_down(size, false);        return; // MOVCD
    case 0x0B: op_move_string_down(size, true);         return; // MOVCFD
    case 0x18: op_search_string_up(size, true);         return; // SCHCU
    case 0x1A: op_search_string_up(size, false);        return; // SKPCU
    case 0x00: op_compare_string(size, false, false);   return; // CMPC
    case 0x01: op_compare_string(size, true, false);    return; // CMPCF
    case 0x02: op_compare_string(size, false, true);    return; // CMPCS
    case 0x19: op_search_string_down(size, true);       return; // SCHCD
    case 0x1B: op_search_string_down(size, false);      return; // SKPCD
    default:
        halt(HaltReason::UnimplementedOpcode, m_opcode); // the sub-opcode follows on the next line
        std::cerr << "[V60] (string instruction " << Hex{m_opcode, 2} << ", sub-opcode "
                  << Hex{static_cast<uint32_t>(sub_opcode & 0x1F), 2} << ")\n";
        return;
    }
}

// MOVCU / MOVCFU / MOVCSU src, len1, dst, len2 (upward): copies
// min(len1, len2) elements from src to dst, lowest address first. MOVCSU
// stops after copying an element equal to R26; MOVCFU then fills the rest
// of dst (up to len2) with R26. Afterwards R28 points past the last source
// element read and R27 past the last destination element written.
void V60::op_move_string_up(OperandSize size, bool fill, bool stop_at_r26)
{
    const uint8_t sub_opcode = m_bus.peek_byte((m_pc - 1) & k_address_mask);
    auto read_length = [this]() {
        const uint8_t spec = fetch_byte();
        return (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : static_cast<uint32_t>(spec & 0x7F);
    };
    const Operand source = decode_operand((sub_opcode & 0x40) != 0, size);
    if (source.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t source_length = read_length();
    const Operand destination = decode_operand((sub_opcode & 0x20) != 0, size);
    if (destination.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t destination_length = read_length();
    const std::optional<uint32_t> src = operand_address(source);
    const std::optional<uint32_t> dst = src ? operand_address(destination) : std::nullopt;
    if (!src || !dst) {
        return;
    }

    const uint32_t step = bytes_of(size);
    const uint32_t mask = size_mask(step);
    const uint32_t count = std::min(source_length, destination_length);
    uint32_t i = 0;
    for (; i < count; ++i) {
        const uint32_t value = read_data(*src + i * step, size);
        write_data(*dst + i * step, size, value);
        if (stop_at_r26 && value == (m_reg[26] & mask)) {
            break;
        }
    }
    m_reg[28] = *src + i * step;
    m_reg[27] = *dst + i * step;
    if (fill && source_length < destination_length) {
        for (; i < destination_length; ++i) {
            write_data(*dst + i * step, size, m_reg[26]);
        }
        m_reg[27] = *dst + i * step;
    }
}

// MOVCD / MOVCFD src, len1, dst, len2 (downward): copies min(len1, len2)
// elements from src to dst, highest index first (safe for overlapping
// moves towards higher addresses); src and dst are the strings' lowest
// addresses. Afterwards R28 = src + (len1 - n - 1) * size and R27 =
// dst + (len2 - n - 1) * size, n = elements copied (as in MAME). MOVCFD then
// fills dst's remaining upper elements (indexes len1 .. len2 - 1) with R26.
// (MAME's halfword fill omits that offset and overwrites the copy; its byte
// version, followed here for both sizes, does not.)
// VR at 0xFFE52D: MOVCD.H (attract mode's ranking scroll).
void V60::op_move_string_down(OperandSize size, bool fill)
{
    const uint8_t sub_opcode = m_bus.peek_byte((m_pc - 1) & k_address_mask);
    auto read_length = [this]() {
        const uint8_t spec = fetch_byte();
        return (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : static_cast<uint32_t>(spec & 0x7F);
    };
    const Operand source = decode_operand((sub_opcode & 0x40) != 0, size);
    if (source.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t source_length = read_length();
    const Operand destination = decode_operand((sub_opcode & 0x20) != 0, size);
    if (destination.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t destination_length = read_length();
    const std::optional<uint32_t> src = operand_address(source);
    const std::optional<uint32_t> dst = src ? operand_address(destination) : std::nullopt;
    if (!src || !dst) {
        return;
    }

    const uint32_t step = bytes_of(size);
    const uint32_t count = std::min(source_length, destination_length);
    uint32_t i = 0;
    for (; i < count; ++i) {
        const uint32_t index = count - i - 1;
        write_data(*dst + index * step, size, read_data(*src + index * step, size));
    }
    m_reg[28] = *src + (source_length - i - 1) * step;
    m_reg[27] = *dst + (destination_length - i - 1) * step;
    if (fill && source_length < destination_length) {
        for (; i < destination_length; ++i) {
            write_data(*dst + (count + destination_length - i - 1) * step, size, m_reg[26]);
        }
        m_reg[27] = *dst + (destination_length - i - 1) * step;
    }
}

// SCHCU / SKPCU str, len, char (upward): scans `len` elements of `str` for
// the first one equal to (SCHCU) / different from (SKPCU) `char`. R28 = the
// address where the scan stopped (str + len * size if it ran out), R27 = its
// index. Z is set when nothing was found and cleared when found - the
// opposite of NEC's manual, but what the hardware does according to MAME.
// VR at 0xFEDD9B: SCHCU.H [R1], R9, R0.
// CMPC / CMPCF / CMPCS src, len1, dst, len2 (as MAME's opCMPSTRB/H):
// compare two strings element by element. CMPCF first pads the shorter one
// to the other's length with R26; CMPCS stops at an element equal to R26
// (CY cleared then, else set). S = the first string is greater, Z = equal
// (lengths included). R28 / R27 = each length + the index reached.
void V60::op_compare_string(OperandSize size, bool fill, bool stop_at_r26)
{
    const uint8_t sub_opcode = m_bus.peek_byte((m_pc - 1) & k_address_mask);
    auto read_length = [this]() {
        const uint8_t spec = fetch_byte();
        return (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : static_cast<uint32_t>(spec & 0x7F);
    };
    const Operand first = decode_operand((sub_opcode & 0x40) != 0, size);
    if (first.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t length1 = read_length();
    const Operand second = decode_operand((sub_opcode & 0x20) != 0, size);
    if (second.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t length2 = read_length();
    const std::optional<uint32_t> a = operand_address(first);
    const std::optional<uint32_t> b = a ? operand_address(second) : std::nullopt;
    if (!a || !b) {
        return;
    }
    const uint32_t step = bytes_of(size);
    const uint32_t mask = size_mask(step);
    const uint32_t stop = m_reg[26] & mask;
    if (fill) {
        for (uint32_t i = length1; i < length2; ++i) {
            write_data(*a + i * step, size, stop);
        }
        for (uint32_t i = length2; i < length1; ++i) {
            write_data(*b + i * step, size, stop);
        }
    }
    const uint32_t count = std::min(length1, length2);
    set_flag(k_psw_z, false);
    set_flag(k_psw_s, false);
    if (stop_at_r26) {
        set_flag(k_psw_cy, true);
    }
    uint32_t i = 0;
    for (; i < count; ++i) {
        const uint32_t c1 = read_data(*a + i * step, size);
        const uint32_t c2 = read_data(*b + i * step, size);
        if (c1 != c2) {
            set_flag(k_psw_s, c1 > c2);
            break;
        }
        if (stop_at_r26 && (c1 == stop || c2 == stop)) {
            set_flag(k_psw_cy, false);
            break;
        }
    }
    m_reg[28] = length1 + i;
    m_reg[27] = length2 + i;
    if (i == count) {
        set_flag(k_psw_s, length1 > length2);
        set_flag(k_psw_z, length1 == length2);
    }
}

// SCHCD / SKPCD str, len, char (as MAME's opSEARCHDB/H): search downward
// for the first element equal (SCHCD) or not equal (SKPCD) to char. MAME
// starts at index len and goes down to 0, and sets Z when the search stops
// at the starting index (its source notes this is the opposite of NEC's
// manual); followed here. R28 = the element address, R27 = its index.
void V60::op_search_string_down(OperandSize size, bool search_equal)
{
    const uint8_t sub_opcode = m_bus.peek_byte((m_pc - 1) & k_address_mask);
    const Operand string = decode_operand((sub_opcode & 0x40) != 0, size);
    if (string.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint8_t spec = fetch_byte();
    const uint32_t length = (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : static_cast<uint32_t>(spec & 0x7F);
    const Operand character = decode_operand((sub_opcode & 0x20) != 0, size);
    if (character.kind == Operand::Kind::Invalid) {
        return;
    }
    const std::optional<uint32_t> base = operand_address(string);
    if (!base) {
        return;
    }
    const uint32_t value = read_operand(character, size);
    const uint32_t step = bytes_of(size);
    auto i = static_cast<int64_t>(length);
    for (; i >= 0; --i) {
        const bool equal = read_data(*base + static_cast<uint32_t>(i) * step, size) == value;
        if (equal == search_equal) {
            break;
        }
    }
    m_reg[28] = *base + static_cast<uint32_t>(i) * step;
    m_reg[27] = static_cast<uint32_t>(i);
    set_flag(k_psw_z, static_cast<uint32_t>(i) == length);
}

void V60::op_search_string_up(OperandSize size, bool search_equal)
{
    const uint8_t sub_opcode = m_bus.peek_byte((m_pc - 1) & k_address_mask);
    const Operand string = decode_operand((sub_opcode & 0x40) != 0, size);
    if (string.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint8_t spec = fetch_byte();
    const uint32_t length = (spec & 0x80) != 0 ? m_reg[spec & 0x1Fu] : static_cast<uint32_t>(spec & 0x7F);
    const Operand character = decode_operand((sub_opcode & 0x20) != 0, size);
    if (character.kind == Operand::Kind::Invalid) {
        return;
    }
    const std::optional<uint32_t> base = operand_address(string);
    if (!base) {
        return;
    }
    const uint32_t value = read_operand(character, size);
    const uint32_t step = bytes_of(size);
    uint32_t i = 0;
    for (; i < length; ++i) {
        const bool equal = read_data(*base + i * step, size) == value;
        if (equal == search_equal) {
            break;
        }
    }
    m_reg[28] = *base + i * step;
    m_reg[27] = i;
    set_flag(k_psw_z, i == length);
}

// TEST src: sets Z and S from the operand, clears CY and OV. Nothing is
// written.
void V60::op_test(OperandSize size, bool mode_m)
{
    const Operand operand = decode_operand(mode_m, size);
    if (operand.kind == Operand::Kind::Invalid) {
        return;
    }
    set_zs_flags(read_operand(operand, size), size);
    set_flag(k_psw_cy, false);
    set_flag(k_psw_ov, false);
}

// INC / DEC dst (one operand): dst = dst + 1 or dst - 1. Sets Z, S, OV,
// CY like ADD / SUB with a source of 1. VR boot at 0xFE0143: INC.W R2.
void V60::op_inc_dec(bool decrement, OperandSize size, bool mode_m)
{
    const Operand operand = decode_operand(mode_m, size);
    if (operand.kind == Operand::Kind::Invalid) {
        return;
    }
    const uint32_t value = read_operand(operand, size);
    const uint32_t saved_psw = m_psw;
    const uint32_t result = add_sub_flags(value, 1, false, decrement, size);
    if (!write_operand(operand, size, result)) {
        m_psw = saved_psw; // the CPU halted: leave the flags as they were
    }
}

// The stack pointer currently in use lives in R31; its bank slot (ISP or
// LnSP) is only refreshed when the PSW switches stacks, so it is accessed
// through R31.
bool V60::read_privileged(uint32_t number, uint32_t& value)
{
    if (number <= 4) {
        const bool active = (number == 0) == ((m_psw & k_psw_is) != 0)
            && (number == 0 || number - 1 == ((m_psw & k_psw_el) >> k_psw_el_shift));
        value = active ? m_reg[k_reg_sp] : (number == 0 ? m_isp : m_level_sp[number - 1]);
        return true;
    }
    if (number == 5) {
        value = m_sbr;
        return true;
    }
    if (number >= k_privileged_count || (number >= 10 && number <= 14)) {
        return false;
    }
    value = m_privileged_other[number];
    return true;
}

bool V60::write_privileged(uint32_t number, uint32_t value)
{
    if (number <= 4) {
        const bool active = (number == 0) == ((m_psw & k_psw_is) != 0)
            && (number == 0 || number - 1 == ((m_psw & k_psw_el) >> k_psw_el_shift));
        if (active) {
            m_reg[k_reg_sp] = value;
        } else if (number == 0) {
            m_isp = value;
        } else {
            m_level_sp[number - 1] = value;
        }
        return true;
    }
    if (number == 5) {
        m_sbr = value;
        return true;
    }
    if (number >= k_privileged_count || (number >= 10 && number <= 14)) {
        return false;
    }
    m_privileged_other[number] = value;
    return true;
}

// LDPR src, #reg: privileged register `reg` = src (32 bits). Execution
// level 0 only. VR boot at 0xFE01D5: LDPR R1, #5 sets SBR = 0xFFF000, the
// vector table base. Flags are not affected.
void V60::op_ldpr()
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    if ((m_psw & k_psw_el) != 0) {
        halt(HaltReason::PrivilegedInstruction, m_opcode);
        return;
    }
    const uint32_t value = read_operand(operands.source, OperandSize::Word);
    const uint32_t number = read_operand(operands.destination, OperandSize::Word);
    if (!write_privileged(number, value)) {
        halt(HaltReason::ReservedPrivilegedRegister, static_cast<uint8_t>(number));
        return;
    }
    if (number > 5 && number != 8) { // 8 = TKCW: its rounding control is used by CVTSW
        std::cerr << "[V60] LDPR: privileged register " << number << " = " << Hex{value}
                  << " (stored, no effect emulated)\n";
    }
}

// STPR #reg, dst: dst = privileged register `reg`. Execution level 0 only.
void V60::op_stpr()
{
    const OperandPair operands = decode_format12(OperandSize::Word, OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    if ((m_psw & k_psw_el) != 0) {
        halt(HaltReason::PrivilegedInstruction, m_opcode);
        return;
    }
    const uint32_t number = read_operand(operands.source, OperandSize::Word);
    uint32_t value = 0;
    if (!read_privileged(number, value)) {
        halt(HaltReason::ReservedPrivilegedRegister, static_cast<uint8_t>(number));
        return;
    }
    write_operand(operands.destination, OperandSize::Word, value);
}

// ---------------------------------------------------------------------------
// PSW, flags and halting
// ---------------------------------------------------------------------------

uint32_t& V60::stack_pointer_slot()
{
    if ((m_psw & k_psw_is) != 0) {
        return m_isp;
    }
    return m_level_sp[(m_psw & k_psw_el) >> k_psw_el_shift];
}

void V60::write_psw(uint32_t value)
{
    const uint32_t changed = m_psw ^ value;
    const bool stack_changes = (changed & k_psw_is) != 0
        || ((m_psw & k_psw_is) == 0 && (changed & k_psw_el) != 0);

    if (stack_changes) {
        stack_pointer_slot() = m_reg[k_reg_sp]; // save outgoing stack
    }
    m_psw = value;
    if (stack_changes) {
        m_reg[k_reg_sp] = stack_pointer_slot(); // load incoming stack
    }
}

void V60::set_flag(uint32_t mask, bool state)
{
    if (state) {
        m_psw |= mask;
    } else {
        m_psw &= ~mask;
    }
}

void V60::set_zs_flags(uint32_t result, OperandSize size)
{
    const uint32_t bytes = bytes_of(size);
    set_flag(k_psw_z, (result & size_mask(bytes)) == 0);
    set_flag(k_psw_s, (result & sign_bit(bytes)) != 0);
}

void V60::halt(HaltReason reason, uint8_t detail)
{
    if (is_halted()) {
        return;
    }
    m_halt_reason = reason;

    switch (reason) {
    case HaltReason::HaltInstruction:
        std::cerr << "[V60] HALT at PC=" << Hex{m_instruction_pc} << ", waiting for interrupt\n";
        return;
    case HaltReason::UnimplementedOpcode:
        std::cerr << "[V60] CRITICAL: unimplemented opcode " << Hex{detail, 2}
                  << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
        break;
    case HaltReason::UnimplementedOperand:
        std::cerr << "[V60] CRITICAL: unhandled addressing mode (mode byte " << Hex{detail, 2}
                  << ") in opcode " << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc}
                  << ", CPU halted\n";
        break;
    case HaltReason::ImmediateDestination:
        std::cerr << "[V60] CRITICAL: immediate operand used as destination in opcode "
                  << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
        break;
    case HaltReason::AddressRequired:
        std::cerr << "[V60] CRITICAL: register or immediate used where an address is required in opcode "
                  << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
        break;
    case HaltReason::InvalidRegisterPair:
        std::cerr << "[V60] CRITICAL: 64-bit operand in R31 (no R32 to pair with) in opcode "
                  << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
        break;
    case HaltReason::PrivilegedInstruction:
        std::cerr << "[V60] CRITICAL: privileged instruction " << Hex{m_opcode, 2}
                  << " outside execution level 0 (exception not emulated) at PC=" << Hex{m_instruction_pc}
                  << ", CPU halted\n";
        break;
    case HaltReason::ReservedPrivilegedRegister:
        std::cerr << "[V60] CRITICAL: reserved privileged register " << static_cast<uint32_t>(detail)
                  << " in opcode " << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc}
                  << ", CPU halted\n";
        break;
    case HaltReason::None:
        return;
    }

    // Rewind so PC points at the instruction that could not be executed.
    m_pc = m_instruction_pc;
}

} // namespace model1
