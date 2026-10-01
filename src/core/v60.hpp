#pragma once

#include <array>
#include <bitset>
#include <cstdint>

namespace model1 {

class Bus;

// NEC uPD70616 (V60) main CPU.
//
// Programmer-visible state:
//   R0-R31  32-bit general-purpose registers
//           (R29 = AP argument pointer, R30 = FP frame pointer, R31 = SP)
//   PC      program counter
//   PSW     program status word (see k_psw_* below)
// Privileged state used by interrupts:
//   ISP     interrupt stack pointer (SP while PSW.IS = 1)
//   L0SP-L3SP  per-execution-level stack pointers (SP while PSW.IS = 0)
//   SBR     system base register; bits 31-12 locate the vector table
//
// The V60 drives a 24-bit external address bus: every address the CPU puts
// on the bus is masked with k_address_mask. After reset the CPU fetches from
// 0xFFFFFFF0, which appears on the bus as 0xFFFFF0.
//
// Instruction encoding (subset implemented so far):
//   Byte 0 is the opcode. Two-operand instructions ("Format I/II") follow
//   with a flags byte:
//     Format I  : 0 m d rrrrr  - one operand is register Rr; the other is a
//                                general operand field with mode bit m.
//                                d=1: Rr is the destination; d=0: the source.
//     Format II : 1 m1 m2 xxxxx - both operands are general operand fields.
//   A general operand field starts with one mode byte (+ optional data):
//     m=1, 011 rrrrr         register direct  Rr
//     m=0, 111 0iiii         immediate quick  #i (0-15)
//     m=0, 1111 0100 + imm   immediate        #imm (size of the operation)
//   Relative branches and PC-relative addresses are computed from the
//   address of the opcode byte, not from the byte after it.
//
// Maskable interrupts:
//   An external device requests an interrupt with a vector number. Before
//   each instruction, if a request is pending and PSW.IE = 1, the CPU:
//     1. switches to the interrupt stack (PSW.IS = 1, SP = ISP),
//     2. clears PSW.IE (and the trace/exception bits), sets EL = 0,
//     3. pushes the old PSW, then the return PC,
//     4. jumps through vector table entry (vector + 0x40) at SBR & ~0xFFF.
//   RETIS pops PC and PSW, which restores the previous stack and IE state.
class V60 {
public:
    static constexpr uint32_t k_reset_pc = 0xFFFFFFF0;
    static constexpr uint32_t k_reset_psw = 0x10000000; // IS=1: interrupt stack
    static constexpr uint32_t k_address_mask = 0x00FFFFFF; // 24-bit bus

    // PSW bits.
    static constexpr uint32_t k_psw_z  = 1u << 0;  // zero
    static constexpr uint32_t k_psw_s  = 1u << 1;  // sign
    static constexpr uint32_t k_psw_ov = 1u << 2;  // overflow
    static constexpr uint32_t k_psw_cy = 1u << 3;  // carry / borrow
    static constexpr uint32_t k_psw_te = 1u << 16; // trace enable
    static constexpr uint32_t k_psw_ae = 1u << 17; // address trap enable
    static constexpr uint32_t k_psw_ie = 1u << 18; // maskable interrupt enable
    static constexpr uint32_t k_psw_el_shift = 24;  // execution level, 2 bits
    static constexpr uint32_t k_psw_el = 3u << k_psw_el_shift;
    static constexpr uint32_t k_psw_tp = 1u << 27; // trace pending
    static constexpr uint32_t k_psw_is = 1u << 28; // running on interrupt stack
    static constexpr uint32_t k_psw_em = 1u << 29; // emulation mode
    static constexpr uint32_t k_psw_asa = 1u << 31; // address space architecture

    // Maskable interrupt vector N uses vector table entry N + this offset.
    static constexpr uint32_t k_irq_vector_base = 0x40;

    // Placeholder timing: real per-instruction cycle counts are not modelled
    // yet, so every instruction (and interrupt entry) is charged this
    // average. MAME's V60 core uses the same approximation.
    static constexpr uint32_t k_average_cycles_per_instruction = 8;

    static constexpr int k_register_count = 32;
    static constexpr int k_reg_ap = 29;
    static constexpr int k_reg_fp = 30;
    static constexpr int k_reg_sp = 31;

    explicit V60(Bus& bus);

    V60(const V60&) = delete;
    V60& operator=(const V60&) = delete;
    V60(V60&&) = delete;
    V60& operator=(V60&&) = delete;

    // Power-on / RESET pin state.
    void reset();

    // Executes one step and returns the number of clock cycles it consumed:
    // either takes a pending interrupt, or fetches the opcode byte at PC,
    // advances PC, decodes and executes one instruction.
    // A halted CPU consumes cycles without executing anything; a CPU halted by
    // the HALT instruction wakes up when it accepts an interrupt.
    uint32_t execute_cycle();

    // Latches a maskable interrupt request with the vector number the
    // requesting device would supply during the acknowledge cycle. The request
    // stays pending until the CPU accepts it (PSW.IE = 1). When several are
    // pending, the lowest vector number is taken first.
    void request_interrupt(uint8_t vector);

    [[nodiscard]] bool is_halted() const { return m_halt_reason != HaltReason::None; }
    [[nodiscard]] bool has_pending_interrupt() const { return m_pending_irqs.any(); }
    [[nodiscard]] uint64_t instruction_count() const { return m_instruction_count; }
    [[nodiscard]] uint64_t interrupt_count() const { return m_interrupt_count; }

    // State access for the motherboard, debuggers and tests.
    [[nodiscard]] uint32_t reg(int index) const { return m_reg[static_cast<std::size_t>(index & 0x1F)]; }
    void set_reg(int index, uint32_t value) { m_reg[static_cast<std::size_t>(index & 0x1F)] = value; }
    [[nodiscard]] uint32_t pc() const { return m_pc; }
    void set_pc(uint32_t value) { m_pc = value; }
    [[nodiscard]] uint32_t psw() const { return m_psw; }
    // Writes the PSW with hardware semantics (swaps SP on IS / EL changes).
    void set_psw(uint32_t value) { write_psw(value); }
    [[nodiscard]] uint32_t isp() const { return m_isp; }
    void set_isp(uint32_t value) { m_isp = value; }
    [[nodiscard]] uint32_t level_sp(int level) const { return m_level_sp[static_cast<std::size_t>(level & 3)]; }
    void set_level_sp(int level, uint32_t value) { m_level_sp[static_cast<std::size_t>(level & 3)] = value; }
    [[nodiscard]] uint32_t sbr() const { return m_sbr; }
    void set_sbr(uint32_t value) { m_sbr = value; }

private:
    // Opcode bytes. Values are from the V60 instruction set.
    enum Opcode : uint8_t {
        k_op_halt    = 0x00,
        k_op_mov_w   = 0x2D,
        k_op_br8     = 0x6A, // BR with 8-bit displacement
        k_op_br16    = 0x7A, // BR with 16-bit displacement
        k_op_add_w   = 0x84,
        k_op_sub_w   = 0xAC,
        k_op_nop     = 0xCD,
        k_op_jmp_m0  = 0xD6, // JMP, operand mode bit m=0
        k_op_jmp_m1  = 0xD7, // JMP, operand mode bit m=1
        k_op_retis_m0 = 0xFA, // RETIS, operand mode bit m=0
        k_op_retis_m1 = 0xFB, // RETIS, operand mode bit m=1
    };

    enum class HaltReason {
        None,
        HaltInstruction,      // HALT executed; waits for an interrupt
        UnimplementedOpcode,
        UnimplementedOperand, // addressing mode not supported yet
        ImmediateDestination, // immediate used as a write target
    };

    enum class OperandSize { Byte, Half, Word };

    // A decoded general operand. Only non-memory forms exist so far.
    struct Operand {
        enum class Kind { Register, Immediate, Invalid };
        Kind     kind  = Kind::Invalid;
        uint8_t  reg   = 0; // valid when kind == Register
        uint32_t value = 0; // valid when kind == Immediate
    };

    struct OperandPair {
        Operand source;
        Operand destination;
    };

    // --- Instruction stream (advance PC) ------------------------------------
    uint8_t  fetch_byte();
    uint16_t fetch_word();
    uint32_t fetch_long();

    // --- Data memory ----------------------------------------------------------
    uint32_t read_data_long(uint32_t address);
    void     write_data_long(uint32_t address, uint32_t value);
    void     push_long(uint32_t value);
    uint32_t pop_long();

    // --- Operand decoding ---------------------------------------------------
    Operand     decode_general_operand(bool mode_m, OperandSize size);
    OperandPair decode_format12(OperandSize size);
    uint32_t    read_operand(const Operand& operand, OperandSize size) const;
    bool        write_operand_long(const Operand& operand, uint32_t value);
    bool        require_valid(const OperandPair& operands);

    // --- Opcode handlers ----------------------------------------------------
    void op_halt();
    void op_nop();
    void op_br8();
    void op_br16();
    void op_jmp(bool mode_m);
    void op_mov_w();
    void op_add_w();
    void op_sub_w();
    void op_retis(bool mode_m);

    // --- Interrupts -----------------------------------------------------------
    [[nodiscard]] bool can_accept_interrupt() const;
    void accept_interrupt();

    // --- PSW ------------------------------------------------------------------
    // Writes the PSW. If PSW.IS changes, or PSW.EL changes while IS = 0, the
    // current SP is saved to the old stack's register and the new one loaded.
    void write_psw(uint32_t value);
    uint32_t& stack_pointer_slot();
    void set_flag(uint32_t mask, bool state);
    void set_zs_flags_long(uint32_t result);

    // Stops execution and logs why, including the faulting instruction address.
    void halt(HaltReason reason, uint8_t detail);

    Bus& m_bus;

    std::array<uint32_t, k_register_count> m_reg{};
    uint32_t m_pc  = k_reset_pc;
    uint32_t m_psw = k_reset_psw;

    // Privileged registers.
    uint32_t m_isp = 0;
    std::array<uint32_t, 4> m_level_sp{};
    uint32_t m_sbr = 0;

    // Address of the opcode byte of the instruction currently executing.
    uint32_t m_instruction_pc = k_reset_pc;
    uint8_t  m_opcode = 0;

    // One bit per interrupt vector number awaiting acknowledgement.
    std::bitset<256> m_pending_irqs;

    HaltReason m_halt_reason = HaltReason::None;
    uint64_t   m_instruction_count = 0;
    uint64_t   m_interrupt_count = 0;
};

} // namespace model1
