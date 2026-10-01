#include "core/v60.hpp"

#include "core/bus.hpp"
#include "core/log.hpp"

#include <iostream>

namespace model1 {

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
    m_instruction_pc = k_reset_pc;
    m_opcode = 0;
    m_pending_irqs.reset();
    m_halt_reason = HaltReason::None;
    m_instruction_count = 0;
    m_interrupt_count = 0;
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
    m_opcode = fetch_byte();

    // Decode + execute
    switch (m_opcode) {
    case k_op_halt:     op_halt();       break;
    case k_op_nop:      op_nop();        break;
    case k_op_br8:      op_br8();        break;
    case k_op_br16:     op_br16();       break;
    case k_op_jmp_m0:   op_jmp(false);   break;
    case k_op_jmp_m1:   op_jmp(true);    break;
    case k_op_mov_w:    op_mov_w();      break;
    case k_op_add_w:    op_add_w();      break;
    case k_op_sub_w:    op_sub_w();      break;
    case k_op_retis_m0: op_retis(false); break;
    case k_op_retis_m1: op_retis(true);  break;
    default:
        halt(HaltReason::UnimplementedOpcode, m_opcode);
        return k_average_cycles_per_instruction;
    }

    ++m_instruction_count;
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
    if (!m_pending_irqs.any() || (m_psw & k_psw_ie) == 0) {
        return false;
    }
    // A HALT waits for interrupts; any other halt reason is a fatal stop.
    return m_halt_reason == HaltReason::None || m_halt_reason == HaltReason::HaltInstruction;
}

void V60::accept_interrupt()
{
    // Acknowledge cycle: take the lowest pending vector number.
    uint32_t vector = 0;
    while (!m_pending_irqs.test(vector)) {
        ++vector;
    }
    m_pending_irqs.reset(vector);

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

// ---------------------------------------------------------------------------
// Data memory and stack
// ---------------------------------------------------------------------------

uint32_t V60::read_data_long(uint32_t address)
{
    return m_bus.read_long(address & k_address_mask);
}

void V60::write_data_long(uint32_t address, uint32_t value)
{
    m_bus.write_long(address & k_address_mask, value);
}

// The stack grows downwards; SP points at the last pushed item.
void V60::push_long(uint32_t value)
{
    m_reg[k_reg_sp] -= 4;
    write_data_long(m_reg[k_reg_sp], value);
}

uint32_t V60::pop_long()
{
    const uint32_t value = read_data_long(m_reg[k_reg_sp]);
    m_reg[k_reg_sp] += 4;
    return value;
}

// ---------------------------------------------------------------------------
// Operand decoding
// ---------------------------------------------------------------------------

V60::Operand V60::decode_general_operand(bool mode_m, OperandSize size)
{
    const uint8_t mode = fetch_byte();
    const uint8_t group = static_cast<uint8_t>(mode >> 5);  // top 3 bits
    const uint8_t low5 = static_cast<uint8_t>(mode & 0x1F);

    Operand operand;
    if (mode_m && group == 0b011) {
        // Register direct: Rn
        operand.kind = Operand::Kind::Register;
        operand.reg = low5;
    } else if (!mode_m && group == 0b111 && low5 < 0x10) {
        // Immediate quick: 4-bit unsigned constant inside the mode byte
        operand.kind = Operand::Kind::Immediate;
        operand.value = low5;
    } else if (!mode_m && mode == 0xF4) {
        // Immediate: constant of the operation's size follows
        operand.kind = Operand::Kind::Immediate;
        switch (size) {
        case OperandSize::Byte: operand.value = fetch_byte(); break;
        case OperandSize::Half: operand.value = fetch_word(); break;
        case OperandSize::Word: operand.value = fetch_long(); break;
        }
    } else {
        halt(HaltReason::UnimplementedOperand, mode);
    }
    return operand;
}

V60::OperandPair V60::decode_format12(OperandSize size)
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
        operands.source = decode_general_operand(mode_m, size);
        if (operands.source.kind != Operand::Kind::Invalid) {
            operands.destination = decode_general_operand(mode_m2, size);
        }
        return operands;
    }

    // Format I: 0 m d rrrrr, one operand is register Rr.
    Operand reg_operand;
    reg_operand.kind = Operand::Kind::Register;
    reg_operand.reg = reg;
    if (dir_d) {
        operands.destination = reg_operand;
        operands.source = decode_general_operand(mode_m, size);
    } else {
        operands.source = reg_operand;
        operands.destination = decode_general_operand(mode_m, size);
    }
    return operands;
}

bool V60::require_valid(const OperandPair& operands)
{
    // decode_general_operand() has already halted and logged on failure.
    return operands.source.kind != Operand::Kind::Invalid
        && operands.destination.kind != Operand::Kind::Invalid;
}

uint32_t V60::read_operand(const Operand& operand, OperandSize size) const
{
    uint32_t value = 0;
    switch (operand.kind) {
    case Operand::Kind::Register:  value = m_reg[operand.reg]; break;
    case Operand::Kind::Immediate: value = operand.value; break;
    case Operand::Kind::Invalid:   return 0;
    }
    switch (size) {
    case OperandSize::Byte: return value & 0xFFu;
    case OperandSize::Half: return value & 0xFFFFu;
    case OperandSize::Word: return value;
    }
    return value;
}

bool V60::write_operand_long(const Operand& operand, uint32_t value)
{
    if (operand.kind != Operand::Kind::Register) {
        // An immediate cannot be a destination. On hardware this raises an
        // addressing-mode exception, which is not emulated yet.
        halt(HaltReason::ImmediateDestination, 0);
        return false;
    }
    m_reg[operand.reg] = value;
    return true;
}

// ---------------------------------------------------------------------------
// Opcode handlers
// ---------------------------------------------------------------------------

// HALT: stop until an interrupt is accepted.
void V60::op_halt()
{
    halt(HaltReason::HaltInstruction, m_opcode);
}

void V60::op_nop()
{
}

// BR disp8: unconditional branch, relative to the opcode address.
void V60::op_br8()
{
    const auto displacement = static_cast<int8_t>(fetch_byte());
    m_pc = m_instruction_pc + static_cast<uint32_t>(static_cast<int32_t>(displacement));
}

// BR disp16: unconditional branch, relative to the opcode address.
void V60::op_br16()
{
    const auto displacement = static_cast<int16_t>(fetch_word());
    m_pc = m_instruction_pc + static_cast<uint32_t>(static_cast<int32_t>(displacement));
}

// JMP: jump to the effective address of a general operand (Format III).
// The low opcode bit is the operand's mode bit m.
void V60::op_jmp(bool mode_m)
{
    const uint8_t mode = fetch_byte();
    const uint8_t group = static_cast<uint8_t>(mode >> 5);

    uint32_t target = 0;
    if (!mode_m && group == 0b011) {
        // Register indirect: [Rn]
        target = m_reg[mode & 0x1F];
    } else if (!mode_m && mode == 0xF0) {
        // PC displacement, 8-bit: disp8[PC]
        target = m_instruction_pc + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fetch_byte())));
    } else if (!mode_m && mode == 0xF1) {
        // PC displacement, 16-bit: disp16[PC]
        target = m_instruction_pc + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fetch_word())));
    } else if (!mode_m && mode == 0xF2) {
        // PC displacement, 32-bit: disp32[PC]
        target = m_instruction_pc + fetch_long();
    } else if (!mode_m && mode == 0xF3) {
        // Direct address: /abs32
        target = fetch_long();
    } else {
        halt(HaltReason::UnimplementedOperand, mode);
        return;
    }
    m_pc = target;
}

// MOV.W src, dst: copy 32 bits. Flags are not affected.
void V60::op_mov_w()
{
    const OperandPair operands = decode_format12(OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    write_operand_long(operands.destination, read_operand(operands.source, OperandSize::Word));
}

// ADD.W src, dst: dst = dst + src. Sets Z, S, OV, CY.
void V60::op_add_w()
{
    const OperandPair operands = decode_format12(OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, OperandSize::Word);
    const uint32_t dst = read_operand(operands.destination, OperandSize::Word);
    const uint64_t wide = static_cast<uint64_t>(dst) + static_cast<uint64_t>(src);
    const auto result = static_cast<uint32_t>(wide);

    if (!write_operand_long(operands.destination, result)) {
        return;
    }
    set_zs_flags_long(result);
    set_flag(k_psw_cy, (wide >> 32) != 0);
    // Signed overflow: both inputs have the same sign and the result differs.
    set_flag(k_psw_ov, ((result ^ src) & (result ^ dst) & 0x80000000u) != 0);
}

// SUB.W src, dst: dst = dst - src. Sets Z, S, OV, CY (CY = borrow).
void V60::op_sub_w()
{
    const OperandPair operands = decode_format12(OperandSize::Word);
    if (!require_valid(operands)) {
        return;
    }
    const uint32_t src = read_operand(operands.source, OperandSize::Word);
    const uint32_t dst = read_operand(operands.destination, OperandSize::Word);
    const uint32_t result = dst - src;

    if (!write_operand_long(operands.destination, result)) {
        return;
    }
    set_zs_flags_long(result);
    set_flag(k_psw_cy, dst < src);
    // Signed overflow: inputs have different signs and the result's sign
    // differs from the minuend's.
    set_flag(k_psw_ov, ((dst ^ src) & (dst ^ result) & 0x80000000u) != 0);
}

// RETIS #frame: return from interrupt. Pops PC, then PSW, discards `frame`
// extra bytes of stack (16-bit operand), then restores the PSW, which swaps
// back to the interrupted code's stack and IE state.
void V60::op_retis(bool mode_m)
{
    const Operand frame = decode_general_operand(mode_m, OperandSize::Half);
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

void V60::set_zs_flags_long(uint32_t result)
{
    set_flag(k_psw_z, result == 0);
    set_flag(k_psw_s, (result & 0x80000000u) != 0);
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
        std::cerr << "[V60] CRITICAL: unimplemented operand (mode byte " << Hex{detail, 2}
                  << ") in opcode " << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc}
                  << ", CPU halted\n";
        break;
    case HaltReason::ImmediateDestination:
        std::cerr << "[V60] CRITICAL: immediate operand used as destination in opcode "
                  << Hex{m_opcode, 2} << " at PC=" << Hex{m_instruction_pc} << ", CPU halted\n";
        break;
    case HaltReason::None:
        return;
    }

    // Rewind so PC points at the instruction that could not be executed.
    m_pc = m_instruction_pc;
}

} // namespace model1
